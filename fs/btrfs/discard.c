/*
 * Copyright (C) 2019 Facebook.  All rights reserved.
 */

#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/sizes.h>
#include <linux/workqueue.h>
#include "ctree.h"
#include "block-group.h"
#include "discard.h"
#include "free-space-cache.h"

/* This is an initial delay to give some chance for lba reuse. */
#define BTRFS_DISCARD_DELAY		(120ULL * NSEC_PER_SEC)

/* Target completion latency of discarding all discardable extents. */
#define BTRFS_DISCARD_TARGET_MSEC	(6 * 60 * 60ULL * MSEC_PER_SEC)
#define BTRFS_DISCARD_MAX_DELAY		(10000UL)
#define BTRFS_DISCARD_MAX_IOPS		(10UL)

static struct list_head *btrfs_get_discard_list(
					struct btrfs_discard_ctl *discard_ctl,
					struct btrfs_block_group_cache *cache)
{
	return &discard_ctl->discard_list[cache->discard_index];
}

static void __btrfs_add_to_discard_list(struct btrfs_discard_ctl *discard_ctl,
					struct btrfs_block_group_cache *cache)
{
	if (list_empty(&cache->discard_list) ||
	    cache->discard_index == BTRFS_DISCARD_INDEX_UNUSED) {
		if (cache->discard_index == BTRFS_DISCARD_INDEX_UNUSED)
			cache->discard_index = BTRFS_DISCARD_INDEX_START;
		cache->discard_eligible_time = (ktime_get_ns() +
						BTRFS_DISCARD_DELAY);
		cache->discard_state = BTRFS_DISCARD_RESET_CURSOR;
	}

	list_move_tail(&cache->discard_list,
		       btrfs_get_discard_list(discard_ctl, cache));
}

void btrfs_add_to_discard_list(struct btrfs_discard_ctl *discard_ctl,
			       struct btrfs_block_group_cache *cache)
{
	spin_lock(&discard_ctl->lock);

	__btrfs_add_to_discard_list(discard_ctl, cache);

	spin_unlock(&discard_ctl->lock);
}

void btrfs_add_to_discard_unused_list(struct btrfs_discard_ctl *discard_ctl,
				      struct btrfs_block_group_cache *cache)
{
	spin_lock(&discard_ctl->lock);

	if (!list_empty(&cache->discard_list))
		list_del_init(&cache->discard_list);

	cache->discard_index = BTRFS_DISCARD_INDEX_UNUSED;
	cache->discard_eligible_time = ktime_get_ns();
	cache->discard_state = BTRFS_DISCARD_RESET_CURSOR;
	list_add_tail(&cache->discard_list,
		      &discard_ctl->discard_list[BTRFS_DISCARD_INDEX_UNUSED]);

	spin_unlock(&discard_ctl->lock);
}

static bool remove_from_discard_list(struct btrfs_discard_ctl *discard_ctl,
				     struct btrfs_block_group_cache *cache)
{
	bool running = false;

	spin_lock(&discard_ctl->lock);

	if (cache == discard_ctl->cache) {
		running = true;
		discard_ctl->cache = NULL;
	}

	cache->discard_eligible_time = 0;
	list_del_init(&cache->discard_list);

	spin_unlock(&discard_ctl->lock);

	return running;
}

/**
 * find_next_cache - find cache that's up next for discarding
 * @discard_ctl: discard control
 * @now: current time
 *
 * Iterate over the discard lists to find the next block_group up for
 * discarding checking the discard_eligible_time of block_group.
 */
static struct btrfs_block_group_cache *find_next_cache(
					struct btrfs_discard_ctl *discard_ctl,
					u64 now)
{
	struct btrfs_block_group_cache *ret_cache = NULL, *cache;
	int i;

	for (i = 0; i < BTRFS_NR_DISCARD_LISTS; i++) {
		struct list_head *discard_list = &discard_ctl->discard_list[i];

		if (!list_empty(discard_list)) {
			cache = list_first_entry(discard_list,
						 struct btrfs_block_group_cache,
						 discard_list);

			if (!ret_cache)
				ret_cache = cache;

			if (ret_cache->discard_eligible_time < now)
				break;

			if (ret_cache->discard_eligible_time >
			    cache->discard_eligible_time)
				ret_cache = cache;
		}
	}

	return ret_cache;
}

/**
 * peek_discard_list - wrap find_next_cache()
 * @discard_ctl: discard control
 * @discard_state: the discard_state of the block_group after state management
 *
 * This wraps find_next_cache() and sets the cache to be in use.
 * discard_state's control flow is managed here.  Variables related to
 * discard_state are reset here as needed (eg discard_cursor).  @discard_state
 * is remembered as it may change while we're discarding, but we want the
 * discard to execute in the context determined here.
 */
static struct btrfs_block_group_cache *peek_discard_list(
					struct btrfs_discard_ctl *discard_ctl,
					enum btrfs_discard_state *discard_state)
{
	struct btrfs_block_group_cache *cache;
	u64 now = ktime_get_ns();

	spin_lock(&discard_ctl->lock);

again:
	cache = find_next_cache(discard_ctl, now);

	if (cache && now > cache->discard_eligible_time) {
		if (cache->discard_index == BTRFS_DISCARD_INDEX_UNUSED &&
		    btrfs_block_group_used(&cache->item) != 0) {
			__btrfs_add_to_discard_list(discard_ctl, cache);
			goto again;
		}
		if (cache->discard_state == BTRFS_DISCARD_RESET_CURSOR) {
			cache->discard_cursor = cache->key.objectid;
			cache->discard_state = BTRFS_DISCARD_EXTENTS;
		}
		discard_ctl->cache = cache;
		*discard_state = cache->discard_state;
	} else {
		cache = NULL;
	}

	spin_unlock(&discard_ctl->lock);

	return cache;
}

/**
 * btrfs_discard_cancel_work - remove a block_group from the discard lists
 * @discard_ctl: discard control
 * @cache: block_group of interest
 *
 * This removes @cache from the discard lists.  If necessary, it waits on the
 * current work and then reschedules the delayed work.
 */
void btrfs_discard_cancel_work(struct btrfs_discard_ctl *discard_ctl,
			       struct btrfs_block_group_cache *cache)
{
	if (remove_from_discard_list(discard_ctl, cache)) {
		cancel_delayed_work_sync(&discard_ctl->work);
		btrfs_discard_schedule_work(discard_ctl, true);
	}
}

/**
 * btrfs_discard_queue_work - handles queuing the block_groups
 * @discard_ctl: discard control
 * @cache: block_group of interest
 *
 * This maintains the LRU order of the discard lists.
 */
void btrfs_discard_queue_work(struct btrfs_discard_ctl *discard_ctl,
			      struct btrfs_block_group_cache *cache)
{
	if (!cache || !btrfs_test_opt(cache->fs_info, DISCARD_ASYNC))
		return;

	if (btrfs_block_group_used(&cache->item) == 0)
		btrfs_add_to_discard_unused_list(discard_ctl, cache);
	else
		btrfs_add_to_discard_list(discard_ctl, cache);

	if (!delayed_work_pending(&discard_ctl->work))
		btrfs_discard_schedule_work(discard_ctl, false);
}

/**
 * btrfs_discard_schedule_work - responsible for scheduling the discard work
 * @discard_ctl: discard control
 * @override: override the current timer
 *
 * Discards are issued by a delayed workqueue item.  @override is used to
 * update the current delay as the baseline delay interview is reevaluated
 * on transaction commit.  This is also maxed with any other rate limit.
 */
void btrfs_discard_schedule_work(struct btrfs_discard_ctl *discard_ctl,
				 bool override)
{
	struct btrfs_block_group_cache *cache;
	u64 now = ktime_get_ns();

	spin_lock(&discard_ctl->lock);

	if (!btrfs_run_discard_work(discard_ctl))
		goto out;

	if (!override && delayed_work_pending(&discard_ctl->work))
		goto out;

	cache = find_next_cache(discard_ctl, now);
	if (cache) {
		u32 delay = discard_ctl->delay;
		u64 bps_limit = READ_ONCE(discard_ctl->bps_limit);

		/*
		 * A single delayed workqueue item is responsible for
		 * discarding, so we can manage the bytes rate limit by keeping
		 * track of the previous discard.
		 */
		if (bps_limit && discard_ctl->prev_discard) {
			u64 bps_delay = (MSEC_PER_SEC *
					 discard_ctl->prev_discard / bps_limit);

			delay = max_t(u64, delay, msecs_to_jiffies(bps_delay));
		}

		/*
		 * This timeout is to hopefully prevent immediate discarding
		 * in a recently allocated block group.
		 */
		if (now < cache->discard_eligible_time) {
			u64 bg_timeout = cache->discard_eligible_time - now;

			delay = max_t(u64, delay, nsecs_to_jiffies(bg_timeout));
		}

		mod_delayed_work(discard_ctl->discard_workers,
				 &discard_ctl->work,
				 delay);
	}

out:
	spin_unlock(&discard_ctl->lock);
}

/**
 * btrfs_finish_discard_pass - determine next step of a block_group
 *
 * This determines the next step for a block group after it's finished going
 * through a pass on a discard list.  If it is unused and fully trimmed, we can
 * mark it unused and send it to the unused_bgs path.  Otherwise, pass it onto
 * the appropriate filter list or let it fall off.
 */
static void btrfs_finish_discard_pass(struct btrfs_discard_ctl *discard_ctl,
				      struct btrfs_block_group_cache *cache)
{
	remove_from_discard_list(discard_ctl, cache);

	if (btrfs_block_group_used(&cache->item) == 0) {
		if (btrfs_is_free_space_trimmed(cache))
			btrfs_mark_bg_unused(cache);
		else
			btrfs_add_to_discard_unused_list(discard_ctl, cache);
	}
}

/**
 * btrfs_discard_workfn - discard work function
 * @work: work
 *
 * This finds the next cache to start discarding and then discards a single
 * region.  It does this in a two-pass fashion: first extents and second
 * bitmaps.  Completely discarded block groups are sent to the unused_bgs path.
 */
static void btrfs_discard_workfn(struct work_struct *work)
{
	struct btrfs_discard_ctl *discard_ctl;
	struct btrfs_block_group_cache *cache;
	enum btrfs_discard_state discard_state;
	u64 trimmed = 0;

	discard_ctl = container_of(work, struct btrfs_discard_ctl, work.work);

	cache = peek_discard_list(discard_ctl, &discard_state);
	if (!cache || !btrfs_run_discard_work(discard_ctl))
		return;

	/* Perform discarding. */
	if (discard_state == BTRFS_DISCARD_BITMAPS)
		btrfs_trim_block_group_bitmaps(cache, &trimmed,
					       cache->discard_cursor,
					       btrfs_block_group_end(cache),
					       0, true);
	else
		btrfs_trim_block_group_extents(cache, &trimmed,
					       cache->discard_cursor,
					       btrfs_block_group_end(cache),
					       0, true);

	discard_ctl->prev_discard = trimmed;

	/* Determine next steps for a block_group. */
	if (cache->discard_cursor >= btrfs_block_group_end(cache)) {
		if (discard_state == BTRFS_DISCARD_BITMAPS) {
			btrfs_finish_discard_pass(discard_ctl, cache);
		} else {
			cache->discard_cursor = cache->key.objectid;
			spin_lock(&discard_ctl->lock);
			if (cache->discard_state != BTRFS_DISCARD_RESET_CURSOR)
				cache->discard_state = BTRFS_DISCARD_BITMAPS;
			spin_unlock(&discard_ctl->lock);
		}
	}

	spin_lock(&discard_ctl->lock);
	discard_ctl->cache = NULL;
	spin_unlock(&discard_ctl->lock);

	btrfs_discard_schedule_work(discard_ctl, false);
}

/**
 * btrfs_run_discard_work - determines if async discard should be running
 * @discard_ctl: discard control
 *
 * Checks if the file system is writeable and BTRFS_FS_DISCARD_RUNNING is set.
 */
bool btrfs_run_discard_work(struct btrfs_discard_ctl *discard_ctl)
{
	struct btrfs_fs_info *fs_info = container_of(discard_ctl,
						     struct btrfs_fs_info,
						     discard_ctl);

	return (!(fs_info->sb->s_flags & SB_RDONLY) &&
		test_bit(BTRFS_FS_DISCARD_RUNNING, &fs_info->flags));
}

/**
 * btrfs_discard_calc_delay - recalculate the base delay
 * @discard_ctl: discard control
 *
 * Recalculate the base delay which is based off the total number of
 * discardable_extents.  Clamp this with the iops_limit and
 * BTRFS_DISCARD_MAX_DELAY.
 */
void btrfs_discard_calc_delay(struct btrfs_discard_ctl *discard_ctl)
{
	s32 discardable_extents =
		atomic_read(&discard_ctl->discardable_extents);
	s32 iops_limit;
	unsigned long delay;

	if (!discardable_extents)
		return;

	spin_lock(&discard_ctl->lock);

	iops_limit = READ_ONCE(discard_ctl->iops_limit);
	if (iops_limit)
		iops_limit = MSEC_PER_SEC / iops_limit;

	delay = BTRFS_DISCARD_TARGET_MSEC / discardable_extents;
	delay = clamp_t(s32, delay, iops_limit, BTRFS_DISCARD_MAX_DELAY);
	discard_ctl->delay = msecs_to_jiffies(delay);

	spin_unlock(&discard_ctl->lock);
}

/**
 * btrfs_discard_update_discardable - propagate discard counters
 * @cache: block_group of interest
 * @ctl: free_space_ctl of @cache
 *
 * This propagates deltas of counters up to the discard_ctl.  It maintains a
 * current counter and a previous counter passing the delta up to the global
 * stat.  Then the current counter value becomes the previous counter value.
 */
void btrfs_discard_update_discardable(struct btrfs_block_group_cache *cache,
				      struct btrfs_free_space_ctl *ctl)
{
	struct btrfs_discard_ctl *discard_ctl;
	s32 extents_delta;
	s64 bytes_delta;

	if (!cache || !btrfs_test_opt(cache->fs_info, DISCARD_ASYNC))
		return;

	discard_ctl = &cache->fs_info->discard_ctl;

	extents_delta = (ctl->discardable_extents[BTRFS_STAT_CURR] -
			 ctl->discardable_extents[BTRFS_STAT_PREV]);
	if (extents_delta) {
		atomic_add(extents_delta, &discard_ctl->discardable_extents);
		ctl->discardable_extents[BTRFS_STAT_PREV] =
			ctl->discardable_extents[BTRFS_STAT_CURR];
	}

	bytes_delta = (ctl->discardable_bytes[BTRFS_STAT_CURR] -
		       ctl->discardable_bytes[BTRFS_STAT_PREV]);
	if (bytes_delta) {
		atomic64_add(bytes_delta, &discard_ctl->discardable_bytes);
		ctl->discardable_bytes[BTRFS_STAT_PREV] =
			ctl->discardable_bytes[BTRFS_STAT_CURR];
	}
}

/**
 * btrfs_discard_punt_unused_bgs_list - punt unused_bgs list to discard lists
 * @fs_info: fs_info of interest
 *
 * The unused_bgs list needs to be punted to the discard lists because the
 * order of operations is changed.  In the normal sychronous discard path, the
 * block groups are trimmed via a single large trim in transaction commit.  This
 * is ultimately what we are trying to avoid with asynchronous discard.  Thus,
 * it must be done before going down the unused_bgs path.
 */
void btrfs_discard_punt_unused_bgs_list(struct btrfs_fs_info *fs_info)
{
	struct btrfs_block_group_cache *cache, *next;

	spin_lock(&fs_info->unused_bgs_lock);

	/* We enabled async discard, so punt all to the queue. */
	list_for_each_entry_safe(cache, next, &fs_info->unused_bgs, bg_list) {
		list_del_init(&cache->bg_list);
		btrfs_add_to_discard_unused_list(&fs_info->discard_ctl, cache);
	}

	spin_unlock(&fs_info->unused_bgs_lock);
}

/**
 * btrfs_discard_purge_list - purge discard lists
 * @discard_ctl: discard control
 *
 * If we are disabling async discard, we may have intercepted block groups that
 * are completely free and ready for the unused_bgs path.  As discarding will
 * now happen in transaction commit or not at all, we can safely mark the
 * corresponding block groups as unused and they will be sent on their merry
 * way to the unused_bgs list.
 */
static void btrfs_discard_purge_list(struct btrfs_discard_ctl *discard_ctl)
{
	struct btrfs_block_group_cache *cache, *next;
	int i;

	spin_lock(&discard_ctl->lock);

	for (i = 0; i < BTRFS_NR_DISCARD_LISTS; i++) {
		list_for_each_entry_safe(cache, next,
					 &discard_ctl->discard_list[i],
					 discard_list) {
			list_del_init(&cache->discard_list);
			spin_unlock(&discard_ctl->lock);
			if (btrfs_block_group_used(&cache->item) == 0)
				btrfs_mark_bg_unused(cache);
			spin_lock(&discard_ctl->lock);
		}
	}

	spin_unlock(&discard_ctl->lock);
}

void btrfs_discard_resume(struct btrfs_fs_info *fs_info)
{
	if (!btrfs_test_opt(fs_info, DISCARD_ASYNC)) {
		btrfs_discard_cleanup(fs_info);
		return;
	}

	btrfs_discard_punt_unused_bgs_list(fs_info);

	set_bit(BTRFS_FS_DISCARD_RUNNING, &fs_info->flags);
}

void btrfs_discard_stop(struct btrfs_fs_info *fs_info)
{
	clear_bit(BTRFS_FS_DISCARD_RUNNING, &fs_info->flags);
}

void btrfs_discard_init(struct btrfs_fs_info *fs_info)
{
	struct btrfs_discard_ctl *discard_ctl = &fs_info->discard_ctl;
	int i;

	spin_lock_init(&discard_ctl->lock);

	INIT_DELAYED_WORK(&discard_ctl->work, btrfs_discard_workfn);

	for (i = 0; i < BTRFS_NR_DISCARD_LISTS; i++)
		 INIT_LIST_HEAD(&discard_ctl->discard_list[i]);

	discard_ctl->prev_discard = 0;
	atomic_set(&discard_ctl->discardable_extents, 0);
	atomic64_set(&discard_ctl->discardable_bytes, 0);
	discard_ctl->delay = BTRFS_DISCARD_MAX_DELAY;
	discard_ctl->iops_limit = BTRFS_DISCARD_MAX_IOPS;
	discard_ctl->bps_limit = 0;
}

void btrfs_discard_cleanup(struct btrfs_fs_info *fs_info)
{
	btrfs_discard_stop(fs_info);
	cancel_delayed_work_sync(&fs_info->discard_ctl.work);

	btrfs_discard_purge_list(&fs_info->discard_ctl);
}
