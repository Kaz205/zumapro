// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/sched/clock.h>

#include <linux/remoteproc/mtk_apu.h>
#include <linux/remoteproc/mtk_apu_config.h>
#include <linux/rpmsg/mtk_rpmsg.h>
#include <linux/soc/mediatek/mtk_apu_hw_logger.h>
#include "mtk_apu_rproc.h"

/* cmd */
enum {
	DPIDLE_CMD_LOCK_IPI = 0x5a00,
	DPIDLE_CMD_UNLOCK_IPI = 0x5a01,
	DPIDLE_CMD_PDN_UNLOCK = 0x5a02,
};

/* ack */
enum {
	DPIDLE_ACK_OK = 0,
	DPIDLE_ACK_LOCK_BUSY,
	DPIDLE_ACK_POWER_DOWN_FAIL,
};

struct dpidle_msg {
	uint32_t cmd;
	uint32_t ack;
};

#define BOOT_WARN_LOG_TIME_US 3000

static struct mtk_apu *g_apu;
static struct dpidle_msg recv_msg;

int mtk_apu_deepidle_power_on_aputop(struct mtk_apu *apu)
{
	const struct mtk_apu_hw_ops *hw_ops = &apu->platdata->ops;
	struct device *dev = apu->dev;
	struct timespec64 begin, end, delta;
	uint32_t wait_ms = 10000;
	int retry = 0;
	int ret;
	u64 t;

	if (!pm_runtime_suspended(apu->dev))
		return 0;

	init_waitqueue_head(&apu->run.wq);
	apu->run.signaled = 0;

	apu->conf_buf->time_offset = sched_clock();
	ret = hw_ops->power_on(apu);

	if (ret == 0) {
		if (apu->hw_logger_data != NULL)
			mtk_apu_hw_logger_deep_idle_leave(apu->hw_logger_data);
	}
	else {
		return ret;
	}

	if (!apu->platdata->flags.secure_boot)
		dev_err(dev, "Not support non-secure boot\n");

	ktime_get_ts64(&begin);
wait_for_warm_boot:
	/* wait for remote warm boot done */
	ret = wait_event_interruptible_timeout(apu->run.wq, apu->run.signaled,
							msecs_to_jiffies(wait_ms));
	if (ret == -ERESTARTSYS) {
		ktime_get_ts64(&end);
		delta = timespec64_sub(end, begin);
		if (delta.tv_sec > (wait_ms/1000)) {
			dev_warn(dev, "APU warm boot timeout!!\n");
			/*
			 * since exception is triggered
			 * so bypass power off timeout check
			 */
			apu->bypass_pwr_off_chk = true;
			return -1;
		}
		if (retry % 50 == 0)
			dev_info(dev,
				"%s: wait APU interrupted by a signal, retry again\n",
				__func__);
		retry++;
		msleep(20);
		goto wait_for_warm_boot;
	}

	if (ret == 0) {
		dev_info(dev, "APU warm boot timeout!!\n");
		apu->bypass_pwr_off_chk = true;
		return -1;
	}

	ktime_get_ts64(&end);
	delta = timespec64_sub(end, begin);
	t = timespec64_to_ns(&delta);
	if (t > BOOT_WARN_LOG_TIME_US * 1000)
		dev_info(dev, "%s: warm boot done (%lldns)\n", __func__, t);
	else
		dev_dbg(dev, "%s: warm boot done\n", __func__);

	return 0;
}

static int mtk_apu_deepidle_send_ack(struct mtk_apu *apu, uint32_t cmd, uint32_t ack)
{
	struct dpidle_msg msg;
	int ret;

	msg.cmd = cmd;
	msg.ack = ack;

	ret = mtk_apu_ipi_send(apu, MTK_APU_IPI_DEEP_IDLE, &msg, sizeof(msg), 0);
	if (ret)
		dev_info(apu->dev, "%s: failed to send ack msg, ack=%d, ret=%d\n",
			 __func__, ack, ret);

	return ret;
}

static void __mtk_apu_deepidle(struct mtk_apu *apu)
{
	const struct mtk_apu_hw_ops *hw_ops = &apu->platdata->ops;
	struct dpidle_msg *msg = &recv_msg;
	int ret;

	switch (msg->cmd) {
	case DPIDLE_CMD_LOCK_IPI:
		ret = mtk_apu_ipi_lock(apu);
		if (ret) {
			mtk_apu_deepidle_send_ack(apu, DPIDLE_CMD_LOCK_IPI, DPIDLE_ACK_LOCK_BUSY);
			return;
		}
		mtk_apu_deepidle_send_ack(apu, DPIDLE_CMD_LOCK_IPI, DPIDLE_ACK_OK);
		break;

	case DPIDLE_CMD_UNLOCK_IPI:
		mtk_apu_ipi_unlock(apu);
		mtk_apu_deepidle_send_ack(apu, DPIDLE_CMD_UNLOCK_IPI, DPIDLE_ACK_OK);
		break;

	case DPIDLE_CMD_PDN_UNLOCK:
		if (apu->hw_logger_data != NULL)
			mtk_apu_hw_logger_deep_idle_enter_pre(apu->hw_logger_data);

		mtk_apu_deepidle_send_ack(apu, DPIDLE_CMD_PDN_UNLOCK, DPIDLE_ACK_OK);

		ret = hw_ops->power_off(apu);
		if (ret) {
			dev_info(apu->dev, "failed to power off ret=%d\n", ret);
			if (apu->hw_logger_data != NULL)
				mtk_apu_hw_logger_deep_idle_enter_post(apu->hw_logger_data);
			mtk_apu_ipi_unlock(apu);
			WARN_ON(0);
			return;
		}

		if (apu->hw_logger_data != NULL)
			mtk_apu_hw_logger_deep_idle_enter_post(apu->hw_logger_data);
		mtk_apu_ipi_unlock(apu);
		dev_dbg(apu->dev, "power off done\n");
		break;

	default:
		dev_info(apu->dev, "unknown cmd %x\n", msg->cmd);
		break;
	}
}

static void mtk_apu_deepidle_ipi_handler(void *data, unsigned int len, void *priv)
{
	struct mtk_apu *apu = (struct mtk_apu *)priv;

	memcpy(&recv_msg, data, len);

	__mtk_apu_deepidle(apu);
}

int mtk_apu_deepidle_init(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;
	int ret;

	ret = mtk_apu_ipi_register(apu, MTK_APU_IPI_DEEP_IDLE, mtk_apu_deepidle_ipi_handler, apu);
	if (ret)
		dev_info(dev, "%s: failed to register deepidle ipi, ret=%d\n", __func__, ret);

	g_apu = apu;

	return ret;
}

void mtk_apu_deepidle_exit(struct mtk_apu *apu)
{
	mtk_apu_ipi_unregister(apu, MTK_APU_IPI_DEEP_IDLE);
}
