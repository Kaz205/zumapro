// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/uaccess.h>

#include "mdw_cmn.h"

static struct dentry *mdw_dbg_root;
static struct mdw_device *mdw_dbg_mdev;

int mdw_dbg_init(struct mdw_device *mdev, struct dentry *dbg_root)
{
	mdw_dbg_mdev = mdev;
	mdev->mdw_klog = 0x0;

	/* create debug root */
	mdw_dbg_root = debugfs_create_dir("midware", dbg_root);

	/* create log level */
	debugfs_create_u32("klog", 0644,
		mdw_dbg_root, &mdev->mdw_klog);

	return 0;
}

void mdw_dbg_deinit(void)
{
	mdw_dbg_mdev = NULL;
}

int mdw_check_log_level(int mask)
{
	return mdw_dbg_mdev->mdw_klog & mask;
}
