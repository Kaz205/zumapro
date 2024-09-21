// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/thermal.h>

struct thermal_zone_device *tzd_cpu_little1;
struct thermal_zone_device *tzd_cpu_little2;
struct thermal_zone_device *tzd_cpu_little3;
struct thermal_zone_device *tzd_cpu_little4;
struct thermal_zone_device *tzd_cpu_big0;
struct thermal_zone_device *tzd_cpu_big1;
struct thermal_zone_device *tzd_apu;
struct thermal_zone_device *tzd_gpu1;
struct thermal_zone_device *tzd_gpu2;
struct thermal_zone_device *tzd_soc1;
struct thermal_zone_device *tzd_soc2;
struct thermal_zone_device *tzd_soc3;
struct thermal_zone_device *tzd_cam1;
struct thermal_zone_device *tzd_cam2;

static int vtemp_get_temp(struct thermal_zone_device *tz, int *temp)
{
	int tz_temp0 = 0;
	int tz_temp_max = 0;

	thermal_zone_get_temp(tzd_cpu_little1, &tz_temp0);
	if (tz_temp0 > tz_temp_max)
		tz_temp_max = tz_temp0;

	thermal_zone_get_temp(tzd_cpu_little2, &tz_temp0);
	if (tz_temp0 > tz_temp_max)
		tz_temp_max = tz_temp0;

	thermal_zone_get_temp(tzd_cpu_little3, &tz_temp0);
	if (tz_temp0 > tz_temp_max)
		tz_temp_max = tz_temp0;

	thermal_zone_get_temp(tzd_cpu_little4, &tz_temp0);
	if (tz_temp0 > tz_temp_max)
		tz_temp_max = tz_temp0;

	thermal_zone_get_temp(tzd_cpu_big0, &tz_temp0);
	if (tz_temp0 > tz_temp_max)
		tz_temp_max = tz_temp0;

	thermal_zone_get_temp(tzd_cpu_big1, &tz_temp0);
	if (tz_temp0 > tz_temp_max)
		tz_temp_max = tz_temp0;

	thermal_zone_get_temp(tzd_apu, &tz_temp0);
	if (tz_temp0 > tz_temp_max)
		tz_temp_max = tz_temp0;

	thermal_zone_get_temp(tzd_gpu1, &tz_temp0);
	if (tz_temp0 > tz_temp_max)
		tz_temp_max = tz_temp0;

	thermal_zone_get_temp(tzd_gpu2, &tz_temp0);
	if (tz_temp0 > tz_temp_max)
		tz_temp_max = tz_temp0;

	thermal_zone_get_temp(tzd_soc1, &tz_temp0);
	if (tz_temp0 > tz_temp_max)
		tz_temp_max = tz_temp0;

	thermal_zone_get_temp(tzd_soc2, &tz_temp0);
	if (tz_temp0 > tz_temp_max)
		tz_temp_max = tz_temp0;

	thermal_zone_get_temp(tzd_soc3, &tz_temp0);
	if (tz_temp0 > tz_temp_max)
		tz_temp_max = tz_temp0;

	thermal_zone_get_temp(tzd_cam1, &tz_temp0);
	if (tz_temp0 > tz_temp_max)
		tz_temp_max = tz_temp0;

	thermal_zone_get_temp(tzd_cam2, &tz_temp0);
	if (tz_temp0 > tz_temp_max)
		tz_temp_max = tz_temp0;

	*temp = tz_temp_max;

	/* printk("[thermal_zone_get_temp] *temp:%d\n", *temp); */

	return 0;
}

static const struct thermal_zone_device_ops vtemp_ops = {
	.get_temp = vtemp_get_temp,
};

static int vtemp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct thermal_zone_device *tzdev;

	tzd_cpu_little1 = thermal_zone_get_zone_by_name("cpu_little1");
	tzd_cpu_little2 = thermal_zone_get_zone_by_name("cpu_little2");
	tzd_cpu_little3 = thermal_zone_get_zone_by_name("cpu_little3");
	tzd_cpu_little4 = thermal_zone_get_zone_by_name("cpu_little4");
	tzd_cpu_big0 = thermal_zone_get_zone_by_name("cpu_big0");
	tzd_cpu_big1 = thermal_zone_get_zone_by_name("cpu_big1");
	tzd_apu = thermal_zone_get_zone_by_name("apu");
	tzd_gpu1 = thermal_zone_get_zone_by_name("gpu1");
	tzd_gpu2 = thermal_zone_get_zone_by_name("gpu2");
	tzd_soc1 = thermal_zone_get_zone_by_name("soc1");
	tzd_soc2 = thermal_zone_get_zone_by_name("soc2");
	tzd_soc3 = thermal_zone_get_zone_by_name("soc3");
	tzd_cam1 = thermal_zone_get_zone_by_name("cam1");
	tzd_cam2 = thermal_zone_get_zone_by_name("cam2");

	tzdev = devm_thermal_of_zone_register(dev, 0,
			NULL, &vtemp_ops);
	return 0;
}

static const struct of_device_id vtemp_of_match[] = {
	{
		.compatible = "mediatek,virtual-temp",
	},
	{},
};
MODULE_DEVICE_TABLE(of, vtemp_of_match);

static struct platform_driver vtemp_driver = {
	.probe = vtemp_probe,
	.driver = {
		.name = "mtk-virtual-temp",
		.of_match_table = vtemp_of_match,
	},
};

module_platform_driver(vtemp_driver);

MODULE_AUTHOR("Michael Kao <michael.kao@mediatek.com>");
MODULE_DESCRIPTION("Example on virtual temp driver");
MODULE_LICENSE("GPL v2");
