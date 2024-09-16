// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022 MediaTek Inc.
 */

#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include "soc_temp_lvts.h"

enum mt8188_lvts_mcu_sensor_enum {
	MT8188_TS1_0,
	MT8188_TS1_1,
	MT8188_TS1_2,
	MT8188_TS1_3,
	MT8188_TS2_0,
	MT8188_TS2_1,
	MT8188_NUM_TS_MCU
};

enum mt8188_lvts_ap_sensor_enum {
	MT8188_TS3_1,
	MT8188_TS4_0,
	MT8188_TS4_1,
	MT8188_TS4_2,
	MT8188_TS5_0,
	MT8188_TS5_1,
	MT8188_TS6_0,
	MT8188_TS6_1,
	MT8188_NUM_TS_AP
};

static void mt8188_mcu_efuse_to_cal_data(struct lvts_data *lvts_data)
{
	struct lvts_sensor_cal_data *cal_data = &lvts_data->cal_data;

	cal_data->golden_temp = GET_CAL_DATA_BITMASK(0, lvts_data, 27, 20);

	cal_data->count_r[MT8188_TS1_0] = (GET_CAL_DATA_BITMASK(6, lvts_data, 7, 0) << 16) +
		GET_CAL_DATA_BITMASK(5, lvts_data, 31, 16);
	cal_data->count_r[MT8188_TS1_1] = GET_CAL_DATA_BITMASK(6, lvts_data, 31, 8);
	cal_data->count_r[MT8188_TS1_2] = GET_CAL_DATA_BITMASK(7, lvts_data, 23, 0);
	cal_data->count_r[MT8188_TS1_3] = (GET_CAL_DATA_BITMASK(8, lvts_data, 15, 0) << 8) +
		GET_CAL_DATA_BITMASK(7, lvts_data, 31, 24);
	cal_data->count_r[MT8188_TS2_0] = (GET_CAL_DATA_BITMASK(9, lvts_data, 7, 0) << 16) +
		GET_CAL_DATA_BITMASK(8, lvts_data, 31, 16);
	cal_data->count_r[MT8188_TS2_1] = GET_CAL_DATA_BITMASK(9, lvts_data, 31, 8);

	cal_data->count_rc[MT8188_TS1_0] = GET_CAL_DATA_BITMASK(1, lvts_data, 23, 0);
	cal_data->count_rc[MT8188_TS2_0] = (GET_CAL_DATA_BITMASK(2, lvts_data, 15, 0) << 8) +
		GET_CAL_DATA_BITMASK(1, lvts_data, 31, 24);
}

static void mt8188_ap_efuse_to_cal_data(struct lvts_data *lvts_data)
{
	struct lvts_sensor_cal_data *cal_data = &lvts_data->cal_data;

	cal_data->golden_temp = GET_CAL_DATA_BITMASK(0, lvts_data, 27, 20);

	cal_data->count_r[MT8188_TS3_1] = GET_CAL_DATA_BITMASK(10, lvts_data, 23, 0);
	cal_data->count_r[MT8188_TS4_0] = (GET_CAL_DATA_BITMASK(11, lvts_data, 15, 0) << 8) +
		GET_CAL_DATA_BITMASK(10, lvts_data, 31, 24);
	cal_data->count_r[MT8188_TS4_1] = (GET_CAL_DATA_BITMASK(12, lvts_data, 7, 0) << 16) +
		GET_CAL_DATA_BITMASK(11, lvts_data, 31, 16);
	cal_data->count_r[MT8188_TS4_2] = GET_CAL_DATA_BITMASK(12, lvts_data, 31, 8);
	cal_data->count_r[MT8188_TS5_0] = GET_CAL_DATA_BITMASK(13, lvts_data, 23, 0);
	cal_data->count_r[MT8188_TS5_1] = (GET_CAL_DATA_BITMASK(14, lvts_data, 15, 0) << 8) +
		GET_CAL_DATA_BITMASK(13, lvts_data, 31, 24);
	cal_data->count_r[MT8188_TS6_0] = (GET_CAL_DATA_BITMASK(15, lvts_data, 7, 0) << 16) +
		GET_CAL_DATA_BITMASK(14, lvts_data, 31, 16);
	cal_data->count_r[MT8188_TS6_1] = GET_CAL_DATA_BITMASK(15, lvts_data, 31, 8);

	cal_data->count_rc[MT8188_TS3_1] = (GET_CAL_DATA_BITMASK(3, lvts_data, 7, 0) << 16) +
		GET_CAL_DATA_BITMASK(2, lvts_data, 31, 16);
	cal_data->count_rc[MT8188_TS4_0] = GET_CAL_DATA_BITMASK(3, lvts_data, 31, 8);
	cal_data->count_rc[MT8188_TS5_0] = GET_CAL_DATA_BITMASK(4, lvts_data, 23, 0);
	cal_data->count_rc[MT8188_TS6_0] = (GET_CAL_DATA_BITMASK(5, lvts_data, 15, 0) << 8) +
		GET_CAL_DATA_BITMASK(4, lvts_data, 31, 24);
}

static struct lvts_speed_settings tc_speed_mt8188 = {
	.period_unit = PERIOD_UNIT,
	.group_interval_delay = GROUP_INTERVAL_DELAY,
	.filter_interval_delay = FILTER_INTERVAL_DELAY,
	.sensor_interval_delay = SENSOR_INTERVAL_DELAY,
};

static const struct lvts_tc_settings mt8188_tc_mcu_settings[] = {
	[0] = {
		.dev_id = 0x81,
		.addr_offset = 0x0,
		.num_sensor = 4,
		.ts_offset = 0,
		.sensor_map = {MT8188_TS1_0, MT8188_TS1_1, MT8188_TS1_2, MT8188_TS1_3},
		.tc_speed = &tc_speed_mt8188,
		.hw_filter = LVTS_FILTER_2_OF_4,
		.dominator_sensing_point = SENSING_POINT1,
		.hw_reboot_trip_point = HW_REBOOT_TRIP_POINT,
		.irq_bit = BIT(3),
	},
	[1] = {
		.dev_id = 0x82,
		.addr_offset = 0x100,
		.num_sensor = 2,
		.ts_offset = 0,
		.sensor_map = {MT8188_TS2_0, MT8188_TS2_1},
		.tc_speed = &tc_speed_mt8188,
		.hw_filter = LVTS_FILTER_2_OF_4,
		.dominator_sensing_point = SENSING_POINT0,
		.hw_reboot_trip_point = HW_REBOOT_TRIP_POINT,
		.irq_bit = BIT(4),
	}
};

static const struct lvts_tc_settings mt8188_tc_ap_settings[] = {
	[0] = {
		.dev_id = 0x83,
		.addr_offset = 0x0,
		.num_sensor = 1,
		.ts_offset = 1,
		.sensor_map = {MT8188_TS3_1},
		.tc_speed = &tc_speed_mt8188,
		.hw_filter = LVTS_FILTER_2_OF_4,
		.dominator_sensing_point = SENSING_POINT1,
		.hw_reboot_trip_point = HW_REBOOT_TRIP_POINT,
		.irq_bit = BIT(3),
	},
	[1] = {
		.dev_id = 0x84,
		.addr_offset = 0x100,
		.num_sensor = 3,
		.ts_offset = 0,
		.sensor_map = {MT8188_TS4_0, MT8188_TS4_1, MT8188_TS4_2},
		.tc_speed = &tc_speed_mt8188,
		.hw_filter = LVTS_FILTER_2_OF_4,
		.dominator_sensing_point = SENSING_POINT1,
		.hw_reboot_trip_point = HW_REBOOT_TRIP_POINT,
		.irq_bit = BIT(4),
	},
	[2] = {
		.dev_id = 0x85,
		.addr_offset = 0x200,
		.num_sensor = 2,
		.ts_offset = 0,
		.sensor_map = {MT8188_TS5_0, MT8188_TS5_1},
		.tc_speed = &tc_speed_mt8188,
		.hw_filter = LVTS_FILTER_2_OF_4,
		.dominator_sensing_point = SENSING_POINT1,
		.hw_reboot_trip_point = HW_REBOOT_TRIP_POINT,
		.irq_bit = BIT(5),
	},
	[3] = {
		.dev_id = 0x86,
		.addr_offset = 0x300,
		.num_sensor = 2,
		.ts_offset = 0,
		.sensor_map = {MT8188_TS6_0, MT8188_TS6_1},
		.tc_speed = &tc_speed_mt8188,
		.hw_filter = LVTS_FILTER_2_OF_4,
		.dominator_sensing_point = SENSING_POINT0,
		.hw_reboot_trip_point = HW_REBOOT_TRIP_POINT,
		.irq_bit = BIT(6),
	}
};

static struct lvts_data mt8188_lvts_mcu_data = {
	.num_tc = (ARRAY_SIZE(mt8188_tc_mcu_settings)),
	.tc = mt8188_tc_mcu_settings,
	.num_sensor = MT8188_NUM_TS_MCU,
	.ops = {
		.efuse_to_cal_data = mt8188_mcu_efuse_to_cal_data,
		.device_enable_and_init = lvts_device_enable_and_init_v5,
		.device_enable_auto_rck = lvts_device_enable_auto_rck_v4,
		.device_read_count_rc_n = lvts_device_read_count_rc_n_v4,
		.set_cal_data = lvts_set_calibration_data_v4,
		.init_controller = lvts_init_controller_v4,
	},
	.feature_bitmap = FEATURE_DEVICE_AUTO_RCK | FEATURE_CK26M_ACTIVE,
	.num_efuse_addr = NUM_EFUSE_ADDR_MT8188,
	.num_efuse_block = NUM_EFUSE_BLOCK_MT8188,
	.cal_data = {
		.default_golden_temp = DEFAULT_GOLDEN_TEMP,
		.default_count_r = DEFAULT_CUONT_R,
		.default_count_rc = DEFAULT_CUONT_RC,
	},
	.coeff = {
		.a = COEFF_A,
		.b = COEFF_B,
	},
};

static struct lvts_data mt8188_lvts_ap_data = {
	.num_tc = (ARRAY_SIZE(mt8188_tc_ap_settings)),
	.tc = mt8188_tc_ap_settings,
	.num_sensor = MT8188_NUM_TS_AP,
	.ops = {
		.efuse_to_cal_data = mt8188_ap_efuse_to_cal_data,
		.device_enable_and_init = lvts_device_enable_and_init_v5,
		.device_enable_auto_rck = lvts_device_enable_auto_rck_v4,
		.device_read_count_rc_n = lvts_device_read_count_rc_n_v4,
		.set_cal_data = lvts_set_calibration_data_v4,
		.init_controller = lvts_init_controller_v4,
	},
	.feature_bitmap = FEATURE_DEVICE_AUTO_RCK | FEATURE_CK26M_ACTIVE,
	.num_efuse_addr = NUM_EFUSE_ADDR_MT8188,
	.num_efuse_block = NUM_EFUSE_BLOCK_MT8188,
	.cal_data = {
		.default_golden_temp = DEFAULT_GOLDEN_TEMP,
		.default_count_r = DEFAULT_CUONT_R,
		.default_count_rc = DEFAULT_CUONT_RC,
	},
	.coeff = {
		.a = COEFF_A,
		.b = COEFF_B,
	},
};

static const struct of_device_id lvts_of_match[] = {
	{ .compatible = "mediatek,mt8188-lvts-mcu", .data = &mt8188_lvts_mcu_data, },
	{ .compatible = "mediatek,mt8188-lvts-ap", .data = &mt8188_lvts_ap_data, },
	{},
};
MODULE_DEVICE_TABLE(of, lvts_of_match);

static struct platform_driver soc_temp_lvts = {
	.probe = lvts_probe,
	.remove = lvts_remove,
	.suspend = lvts_suspend,
	.resume = lvts_resume,
	.driver = {
		.name = "mtk-soc-temp-lvts-mt8188",
		.of_match_table = lvts_of_match,
	},
};
module_platform_driver(soc_temp_lvts);

MODULE_AUTHOR("Yu-Chia Chang <ethan.chang@mediatek.com>");
MODULE_AUTHOR("Michael Kao <michael.kao@mediatek.com>");
MODULE_DESCRIPTION("MediaTek soc temperature driver");
MODULE_LICENSE("GPL v2");
