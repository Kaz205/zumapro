/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#ifndef __MTK_APU_MDW_CMN_H__
#define __MTK_APU_MDW_CMN_H__

#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/time.h>

#include "mdw.h"

enum {
	MDW_DBG_DRV = 0x01,
	MDW_DBG_FLW = 0x02,
	MDW_DBG_CMD = 0x04,
	MDW_DBG_MEM = 0x08,
	MDW_DBG_PEF = 0x10,
	MDW_DBG_SUB = 0x20,

	MDW_DBG_EXP = 0x1000,
};

#define mdw_debug(mask, x, ...) do { if (mdw_check_log_level(mask)) \
		pr_debug("[debug] %s/%d " x, __func__, __LINE__, ##__VA_ARGS__); \
	} while (0)

#define mdw_drv_debug(x, ...) mdw_debug(MDW_DBG_DRV, x, ##__VA_ARGS__)
#define mdw_flw_debug(x, ...) mdw_debug(MDW_DBG_FLW, x, ##__VA_ARGS__)
#define mdw_cmd_debug(x, ...) mdw_debug(MDW_DBG_CMD, x, ##__VA_ARGS__)
#define mdw_mem_debug(x, ...) mdw_debug(MDW_DBG_MEM, x, ##__VA_ARGS__)
#define mdw_pef_debug(x, ...) mdw_debug(MDW_DBG_PEF, x, ##__VA_ARGS__)
#define mdw_sub_debug(x, ...) mdw_debug(MDW_DBG_SUB, x, ##__VA_ARGS__)

#endif
