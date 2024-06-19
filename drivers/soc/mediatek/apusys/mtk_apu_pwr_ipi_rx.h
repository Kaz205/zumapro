/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#ifndef MTK_APU_PWR_IPI_H
#define MTK_APU_PWR_IPI_H

/* request send from AP to uP */
enum mtk_apu_pwr_cmd {
	MTK_APU_PWR_DEV_CTL,
	MTK_APU_PWR_DEV_SET_OPP,
	MTK_APU_PWR_DUMP_OPP_TBL,
	MTK_APU_PWR_DUMP_OPP_TBL2,
	MTK_APU_PWR_CURR_STATUS,
	MTK_APU_PWR_PWR_PROFILING,
	MTK_APU_PWR_CLK_SET_RATE,
	MTK_APU_PWR_BUK_SET_VOLT,
	MTK_APU_PWR_ARE_DBG,
	MTK_APU_PWR_DPIDLE_SKIP,
	MTK_APU_PWR_THERMAL_GET_TEMP,
	MTK_APU_PWR_RPMSG_CMD_MAX,
};

/* power cmd format (from AP to uP) */
struct mtk_apu_pwr_rpmsg_data {
	enum mtk_apu_pwr_cmd cmd;
	int data0;
	int data1;
	int data2;
	int data3;
};

#endif
