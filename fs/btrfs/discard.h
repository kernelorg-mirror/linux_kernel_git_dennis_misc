/*
 * Copyright (C) 2019 Facebook.  All rights reserved.
 */

#ifndef BTRFS_DISCARD_H
#define BTRFS_DISCARD_H

#include <linux/sizes.h>

struct btrfs_fs_info;
struct btrfs_discard_ctl;
struct btrfs_block_group_cache;

/* Discard size limits. */
#define BTRFS_ASYNC_DISCARD_MAX_SIZE	(SZ_64M)

/* List operations. */
void btrfs_add_to_discard_list(struct btrfs_discard_ctl *discard_ctl,
			       struct btrfs_block_group_cache *cache);
void btrfs_add_to_discard_unused_list(struct btrfs_discard_ctl *discard_ctl,
				      struct btrfs_block_group_cache *cache);

/* Work operations. */
void btrfs_discard_cancel_work(struct btrfs_discard_ctl *discard_ctl,
			       struct btrfs_block_group_cache *cache);
void btrfs_discard_queue_work(struct btrfs_discard_ctl *discard_ctl,
			      struct btrfs_block_group_cache *cache);
void btrfs_discard_schedule_work(struct btrfs_discard_ctl *discard_ctl,
				 bool override);
bool btrfs_run_discard_work(struct btrfs_discard_ctl *discard_ctl);

/* Update operations. */
void btrfs_discard_calc_delay(struct btrfs_discard_ctl *discard_ctl);
void btrfs_discard_update_discardable(struct btrfs_block_group_cache *cache,
				      struct btrfs_free_space_ctl *ctl);

/* Setup/Cleanup operations. */
void btrfs_discard_punt_unused_bgs_list(struct btrfs_fs_info *fs_info);
void btrfs_discard_resume(struct btrfs_fs_info *fs_info);
void btrfs_discard_stop(struct btrfs_fs_info *fs_info);
void btrfs_discard_init(struct btrfs_fs_info *fs_info);
void btrfs_discard_cleanup(struct btrfs_fs_info *fs_info);

#endif
