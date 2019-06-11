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

static struct list_head *btrfs_get_discard_list(
					struct btrfs_discard_ctl *discard_ctl,
					struct btrfs_block_group_cache *cache)
{
	return &discard_ctl->discard_list[cache->discard_index];
}

void btrfs_add_to_discard_list(struct btrfs_discard_ctl *discard_ctl,
			       struct btrfs_block_group_cache *cache)
{
	spin_lock(&discard_ctl->lock);

	if (list_empty(&cache->discard_list))
		cache->discard_eligible_time = (ktime_get_ns() +
						BTRFS_DISCARD_DELAY);

	list_move_tail(&cache->discard_list,
		       btrfs_get_discard_list(discard_ctl, cache));

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
 *
 * This wraps find_next_cache() and sets the cache to be in use.
 */
static struct btrfs_block_group_cache *peek_discard_list(
					struct btrfs_discard_ctl *discard_ctl)
{
	struct btrfs_block_group_cache *cache;
	u64 now = ktime_get_ns();

	spin_lock(&discard_ctl->lock);

	cache = find_next_cache(discard_ctl, now);

	if (cache && now < cache->discard_eligible_time)
		cache = NULL;

	discard_ctl->cache = cache;

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
		u64 delay = 0;

		if (now < cache->discard_eligible_time)
			delay = nsecs_to_jiffies(cache->discard_eligible_time -
						 now);

		mod_delayed_work(discard_ctl->discard_workers,
				 &discard_ctl->work,
				 delay);
	}

out:
	spin_unlock(&discard_ctl->lock);
}

/**
 * btrfs_discard_workfn - discard work function
 * @work: work
 *
 * This finds the next cache to start discarding and then discards it.
 */
static void btrfs_discard_workfn(struct work_struct *work)
{
	struct btrfs_discard_ctl *discard_ctl;
	struct btrfs_block_group_cache *cache;
	u64 trimmed = 0;

	discard_ctl = container_of(work, struct btrfs_discard_ctl, work.work);

	cache = peek_discard_list(discard_ctl);
	if (!cache || !btrfs_run_discard_work(discard_ctl))
		return;

	btrfs_trim_block_group(cache, &trimmed, cache->key.objectid,
			       btrfs_block_group_end(cache), 0);

	remove_from_discard_list(discard_ctl, cache);

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

void btrfs_discard_resume(struct btrfs_fs_info *fs_info)
{
	if (!btrfs_test_opt(fs_info, DISCARD_ASYNC)) {
		btrfs_discard_cleanup(fs_info);
		return;
	}

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
}

void btrfs_discard_cleanup(struct btrfs_fs_info *fs_info)
{
	btrfs_discard_stop(fs_info);
	cancel_delayed_work_sync(&fs_info->discard_ctl.work);
}
