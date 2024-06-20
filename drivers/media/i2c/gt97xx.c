// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Giantec gt97xx VCM lens device
 *
 * Copyright 2024 MediaTek
 *
 * Zhi Mao <zhi.mao@mediatek.com>
 */
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/units.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* gt97xx chip info register and name */
#define GT97XX_IC_INFO_REG CCI_REG8(0x00)
#define GT9768_ID 0xE9
#define GT9769_ID 0xE1
#define GT97XX_NAME "gt97xx"

/*
 * Ring control and Power control register
 * Bit[1] RING_EN
 * 0: Direct mode
 * 1: AAC mode (ringing control mode)
 * Bit[0] PD
 * 0: Normal operation mode
 * 1: Power down mode
 * requires waiting time after PD reset takes place.
 */
#define GT97XX_RING_PD_CONTROL_REG CCI_REG8(0x02)
#define GT97XX_PD_MODE_OFF 0x00
#define GT97XX_PD_MODE_EN BIT(0)
#define GT97XX_AAC_MODE_EN BIT(1)

/*
 * DAC is a 10bit address to control the VCM position.
 * DAC_MSB: D[9:8] (ADD: 0x03)
 * DAC_LSB: D[7:0] (ADD: 0x04)
 */
#define GT97XX_DAC_ADDR_REG CCI_REG16(0x03)

#define GT97XX_MOVE_STEPS 16
#define GT97XX_MAX_FOCUS_POS (BIT(10) - 1)

#define GT97XX_SLEEP_US (1 * USEC_PER_MSEC)

enum vcm_giantec_reg_desc {
	GT_IC_INFO_REG,
	GT_RING_PD_CONTROL_REG,
	GT_DAC_ADDR_REG,
	GT_MAX_REG
};

struct vcm_giantec_of_data {
	unsigned int id;
	unsigned int regs[GT_MAX_REG];
};

static const char *const gt97xx_supply_names[] = {
	"vin",
	"vdd",
};

/* gt97xx device structure */
struct gt97xx {
	struct v4l2_subdev sd;

	struct regulator_bulk_data supplies[ARRAY_SIZE(gt97xx_supply_names)];

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *focus;

	struct regmap *regmap;

	const struct vcm_giantec_of_data *chip;
};

static inline struct gt97xx *sd_to_gt97xx(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct gt97xx, sd);
}

struct regval_list {
	u8 reg_num;
	u8 value;
};

static int gt97xx_set_dac(struct gt97xx *gt97xx, u16 val)
{
	/* Write VCM position to registers */
	return cci_write(gt97xx->regmap,
			 gt97xx->chip->regs[GT_DAC_ADDR_REG], val, NULL);
}

static int gt97xx_identify_module(struct gt97xx *gt97xx)
{
	int ret;
	u64 ic_id;
	struct i2c_client *client = v4l2_get_subdevdata(&gt97xx->sd);

	ret = cci_read(gt97xx->regmap, gt97xx->chip->regs[GT_IC_INFO_REG],
		       &ic_id, NULL);
	if (ret < 0)
		return ret;

	if (ic_id != gt97xx->chip->id) {
		dev_err(&client->dev, "chip id mismatch: 0x%x!=0x%llx",
			gt97xx->chip->id, ic_id);
		return -1;
	}

	return 0;
}

static int gt97xx_init(struct gt97xx *gt97xx)
{
	int ret, val;

	ret = gt97xx_identify_module(gt97xx);
	if (ret < 0)
		return ret;

	/* Reset PD_CONTROL */
	ret = cci_write(gt97xx->regmap,
			gt97xx->chip->regs[GT_RING_PD_CONTROL_REG],
			GT97XX_PD_MODE_OFF, NULL);
	if (ret < 0)
		return ret;

	/* Need waiting delay time after PD reset */
	fsleep(GT97XX_SLEEP_US);

	/* Enable ACC mode */
	ret = cci_write(gt97xx->regmap,
			gt97xx->chip->regs[GT_RING_PD_CONTROL_REG],
			GT97XX_AAC_MODE_EN, NULL);
	if (ret < 0)
		return ret;

	for (val = gt97xx->focus->val % GT97XX_MOVE_STEPS;
	     val <= gt97xx->focus->val; val += GT97XX_MOVE_STEPS) {
		ret = gt97xx_set_dac(gt97xx, val);
		if (ret)
			return ret;

		fsleep(GT97XX_SLEEP_US);
	}

	return 0;
}

static int gt97xx_release(struct gt97xx *gt97xx)
{
	int ret, val;

	val = round_down(gt97xx->focus->val, GT97XX_MOVE_STEPS);
	for (; val >= 0; val -= GT97XX_MOVE_STEPS) {
		ret = gt97xx_set_dac(gt97xx, val);
		if (ret)
			return ret;

		fsleep(GT97XX_SLEEP_US);
	}

	return 0;
}

static int gt97xx_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gt97xx *gt97xx = sd_to_gt97xx(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(gt97xx_supply_names),
				    gt97xx->supplies);
	if (ret < 0)
		dev_err(dev, "failed to enable regulators\n");

	return ret;
}

static int gt97xx_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gt97xx *gt97xx = sd_to_gt97xx(sd);
	int ret;

	ret = regulator_bulk_disable(ARRAY_SIZE(gt97xx_supply_names),
				     gt97xx->supplies);
	if (ret < 0)
		dev_err(dev, "failed to disable regulators\n");

	return ret;
}

static int gt97xx_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gt97xx *gt97xx = sd_to_gt97xx(sd);

	gt97xx_release(gt97xx);
	gt97xx_power_off(dev);

	return 0;
}

static int gt97xx_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gt97xx *gt97xx = sd_to_gt97xx(sd);
	int ret;

	ret = gt97xx_power_on(dev);
	if (ret < 0) {
		dev_err(dev, "failed to power_on\n");
		return ret;
	}

	/* Need waited before sending I2C commands after power-up */
	fsleep(GT97XX_SLEEP_US);

	ret = gt97xx_init(gt97xx);
	if (ret < 0)
		goto disable_power;

	return 0;

disable_power:
	gt97xx_power_off(dev);

	return ret;
}

static int gt97xx_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gt97xx *gt97xx =
		container_of(ctrl->handler, struct gt97xx, ctrls);

	if (ctrl->id == V4L2_CID_FOCUS_ABSOLUTE)
		return gt97xx_set_dac(gt97xx, ctrl->val);

	return 0;
}

static const struct v4l2_ctrl_ops gt97xx_ctrl_ops = {
	.s_ctrl = gt97xx_set_ctrl,
};

static int gt97xx_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int gt97xx_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_put(sd->dev);
}

static const struct v4l2_subdev_internal_ops gt97xx_int_ops = {
	.open = gt97xx_open,
	.close = gt97xx_close,
};

static const struct v4l2_subdev_core_ops gt97xx_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops gt97xx_ops = {
	.core = &gt97xx_core_ops,
};

static int gt97xx_init_controls(struct gt97xx *gt97xx)
{
	struct v4l2_ctrl_handler *hdl = &gt97xx->ctrls;
	const struct v4l2_ctrl_ops *ops = &gt97xx_ctrl_ops;

	v4l2_ctrl_handler_init(hdl, 1);

	gt97xx->focus = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FOCUS_ABSOLUTE, 0,
					  GT97XX_MAX_FOCUS_POS,
					  1, 0);

	if (hdl->error)
		return hdl->error;

	gt97xx->sd.ctrl_handler = hdl;

	return 0;
}

static int gt97xx_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gt97xx *gt97xx;
	unsigned int i;
	int ret;

	gt97xx = devm_kzalloc(dev, sizeof(*gt97xx), GFP_KERNEL);
	if (!gt97xx)
		return -ENOMEM;

	gt97xx->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(gt97xx->regmap))
		return dev_err_probe(dev, PTR_ERR(gt97xx->regmap),
				     "failed to init CCI\n");

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&gt97xx->sd, client, &gt97xx_ops);

	gt97xx->chip = device_get_match_data(dev);

	for (i = 0; i < ARRAY_SIZE(gt97xx_supply_names); i++)
		gt97xx->supplies[i].supply = gt97xx_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(gt97xx_supply_names),
				      gt97xx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "failed to get regulators\n");

	/* Initialize controls */
	ret = gt97xx_init_controls(gt97xx);
	if (ret)
		goto err_free_handler;

	/* Initialize subdev */
	gt97xx->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	gt97xx->sd.internal_ops = &gt97xx_int_ops;
	gt97xx->sd.entity.function = MEDIA_ENT_F_LENS;

	ret = media_entity_pads_init(&gt97xx->sd.entity, 0, NULL);
	if (ret < 0)
		goto err_free_handler;

	/* Power on and initialize hardware */
	ret = gt97xx_runtime_resume(dev);
	if (ret < 0) {
		dev_err_probe(dev, ret, "failed to power on\n");
		goto err_clean_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_idle(dev);

	ret = v4l2_async_register_subdev(&gt97xx->sd);
	if (ret < 0) {
		dev_err_probe(dev, ret, "failed to register V4L2 subdev\n");
		goto err_power_off;
	}

	return 0;

err_power_off:
	pm_runtime_disable(dev);
err_clean_entity:
	media_entity_cleanup(&gt97xx->sd.entity);
err_free_handler:
	v4l2_ctrl_handler_free(&gt97xx->ctrls);

	return ret;
}

static void gt97xx_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gt97xx *gt97xx = sd_to_gt97xx(sd);

	v4l2_async_unregister_subdev(&gt97xx->sd);
	v4l2_ctrl_handler_free(&gt97xx->ctrls);
	media_entity_cleanup(&gt97xx->sd.entity);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		gt97xx_runtime_suspend(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(gt97xx_pm_ops,
				 gt97xx_runtime_suspend,
				 gt97xx_runtime_resume,
				 NULL);

static const struct vcm_giantec_of_data gt9768_data = {
	.id = GT9768_ID,
	.regs[GT_IC_INFO_REG] = GT97XX_IC_INFO_REG,
	.regs[GT_RING_PD_CONTROL_REG] = GT97XX_RING_PD_CONTROL_REG,
	.regs[GT_DAC_ADDR_REG] = GT97XX_DAC_ADDR_REG,
};

static const struct vcm_giantec_of_data gt9769_data = {
	.id = GT9769_ID,
	.regs[GT_IC_INFO_REG] = GT97XX_IC_INFO_REG,
	.regs[GT_RING_PD_CONTROL_REG] = GT97XX_RING_PD_CONTROL_REG,
	.regs[GT_DAC_ADDR_REG] = GT97XX_DAC_ADDR_REG,
};

static const struct of_device_id gt97xx_of_table[] = {
	{ .compatible = "giantec,gt9768", .data = &gt9768_data },
	{ .compatible = "giantec,gt9769", .data = &gt9769_data },
	{}
};
MODULE_DEVICE_TABLE(of, gt97xx_of_table);

static struct i2c_driver gt97xx_i2c_driver = {
	.driver = {
		.name = GT97XX_NAME,
		.pm = pm_ptr(&gt97xx_pm_ops),
		.of_match_table = gt97xx_of_table,
	},
	.probe_new = gt97xx_probe,
	.remove = gt97xx_remove,
};
module_i2c_driver(gt97xx_i2c_driver);

MODULE_AUTHOR("Zhi Mao <zhi.mao@mediatek.com>");
MODULE_DESCRIPTION("GT97xx VCM driver");
MODULE_LICENSE("GPL");
