// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/rpmsg.h>
#include <linux/thermal.h>

#include "mtk_apu_pwr_ipi_rx.h"

struct mtk_apu_pwr_rpmsg {
	struct rpmsg_endpoint *ept;
	struct rpmsg_device *rpdev;
	enum mtk_apu_pwr_cmd curr_rpmsg_cmd;

	/* thermal */
	struct thermal_zone_device *mtk_apu_tz;
};

static int mtk_apu_pwr_rx_callback(struct rpmsg_device *rpdev, void *data,
				   int len, void *priv, u32 src)
{
	struct device *dev = &rpdev->dev;
	struct mtk_apu_pwr_rpmsg *power_rpmsg = priv;
	int cmd = *((int*)data);
	int ret = 0, temp = -1;
	struct mtk_apu_pwr_rpmsg_data rpmsg_data;

	if (!len) {
		dev_err(dev, "MTK APU power rpmsg received empty rply\n");
		return -EINVAL;
	}

	switch ((enum mtk_apu_pwr_cmd)cmd) {
	case MTK_APU_PWR_THERMAL_GET_TEMP:
		if (power_rpmsg->mtk_apu_tz == NULL) {
			temp = THERMAL_TEMP_INVALID;
		} else {
			ret = thermal_zone_get_temp(power_rpmsg->mtk_apu_tz, &temp);
			if (ret) {
				dev_err(dev, "%s: thermal zone get temp failed\n", __func__);
				temp = THERMAL_TEMP_INVALID;
			}
		}

		rpmsg_data.cmd = MTK_APU_PWR_THERMAL_GET_TEMP;
		rpmsg_data.data0 = temp;	/* thermal temperature */

		ret = rpmsg_send(power_rpmsg->ept, &rpmsg_data, sizeof(rpmsg_data));
		if (ret)
			dev_info(&power_rpmsg->rpdev->dev, "send reply failed: %d\n", ret);

		break;
	default:
		dev_err(dev, "%s: invalid cmd: %d\n", __func__, cmd);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int mtk_apu_pwr_rx_probe(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct rpmsg_channel_info chinfo = {};
	struct mtk_apu_pwr_rpmsg *power_rpmsg;
	struct device_node *node = dev->of_node;
	const char *tz_name;

	power_rpmsg = devm_kzalloc(dev, sizeof(*power_rpmsg), GFP_KERNEL);
	if (!power_rpmsg)
		return -ENOMEM;

	power_rpmsg->rpdev = rpdev;

	if (of_property_read_string(node, "apusys,thermal-zones", &tz_name)) {
		dev_err(dev, "%s: mtk apu thermal-zones name no found\n", __func__);
	} else {
		power_rpmsg->mtk_apu_tz = thermal_zone_get_zone_by_name(tz_name);
		if (IS_ERR(power_rpmsg->mtk_apu_tz)) {
			dev_err(dev, "%s: thermal zone %s not found\n", __func__, tz_name);
			power_rpmsg->mtk_apu_tz = NULL;
		}
	}

	dev_set_drvdata(dev, power_rpmsg);

	strscpy(chinfo.name, rpdev->id.name, RPMSG_NAME_SIZE);
	chinfo.src = rpdev->src;
	chinfo.dst = RPMSG_ADDR_ANY;
	power_rpmsg->ept = rpmsg_create_ept(rpdev, mtk_apu_pwr_rx_callback, power_rpmsg, chinfo);
	if (!power_rpmsg->ept) {
		dev_err(dev, "failed to create ept\n");
		return -ENODEV;
	}

	return 0;
}

static void mtk_apu_pwr_rx_remove(struct rpmsg_device *rpdev)
{
	struct mtk_apu_pwr_rpmsg *power_rpmsg = dev_get_drvdata(&rpdev->dev);

	rpmsg_destroy_ept(power_rpmsg->ept);
}

static const struct of_device_id mtk_apu_pwr_rx_rpmsg_of_match[] = {
	{ .compatible = "mediatek,apupwr-rx-rpmsg", },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, mtk_apu_pwr_rx_rpmsg_of_match);

static struct rpmsg_driver pwr_rx_rpmsg_drv = {
	.drv	= {
		.name	= "mtk-apu-pwr-rx-rpmsg",
		.owner = THIS_MODULE,
		.of_match_table = mtk_apu_pwr_rx_rpmsg_of_match,
	},
	.probe = mtk_apu_pwr_rx_probe,
	.remove = mtk_apu_pwr_rx_remove,
};

module_rpmsg_driver(pwr_rx_rpmsg_drv);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek apu-rx-ipi rpmsg driver");
