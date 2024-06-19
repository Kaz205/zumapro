// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/sched/clock.h>

#include <linux/remoteproc/mtk_apu.h>
#include "mtk_apu_rproc.h"

static void mtk_apu_timesync_work_func(struct work_struct *work)
{
	struct mtk_apu *apu = container_of(work, struct mtk_apu, timesync_work);
	u64 timesync_stamp;

	timesync_stamp = sched_clock();
	mtk_apu_ipi_send(apu, MTK_APU_IPI_TIMESYNC, &timesync_stamp, sizeof(u64), 0);
	pr_info("%s %d\n", __func__, __LINE__);
}

static void mtk_apu_timesync_handler(void *data, u32 len, void *priv)
{
	struct mtk_apu *apu = (struct mtk_apu *)priv;

	dev_dbg(apu->dev, "timesync request received\n");
	queue_work(apu->timesync_workq, &apu->timesync_work);
}

int mtk_apu_timesync_init(struct mtk_apu *apu)
{
	int ret;

	apu->timesync_workq = alloc_workqueue("apu_timesync", WQ_UNBOUND | WQ_HIGHPRI, 0);
	if (!apu->timesync_workq) {
		dev_info(apu->dev, "%s: failed to allocate wq for timesync\n", __func__);
		return -ENOMEM;
	}

	INIT_WORK(&apu->timesync_work, mtk_apu_timesync_work_func);

	ret = mtk_apu_ipi_register(apu, MTK_APU_IPI_TIMESYNC, mtk_apu_timesync_handler, apu);
	if (ret) {
		dev_err(apu->dev, "failed to register APU timesync IPI\n");
		destroy_workqueue(apu->timesync_workq);
		apu->timesync_workq = NULL;
		return ret;
	}

	return 0;
}

void mtk_apu_timesync_remove(struct mtk_apu *apu)
{
	mtk_apu_ipi_unregister(apu, MTK_APU_IPI_TIMESYNC);

	if (apu->timesync_workq)
		destroy_workqueue(apu->timesync_workq);
}
