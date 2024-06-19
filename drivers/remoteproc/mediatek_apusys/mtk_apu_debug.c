// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/remoteproc/mtk_apu.h>
#include <linux/soc/mediatek/mtk_apu_hw_logger.h>
#include "mtk_apu_rproc.h"

int mtk_apu_debug_init(struct mtk_apu *apu)
{
	int ret = 0;

	ret = mtk_apu_ipi_register(apu, MTK_APU_IPI_LOG_LEVEL,
			mtk_apu_hw_log_level_ipi_handler, NULL);
	if (ret)
		dev_err(apu->dev, "Fail in hw_log_level_ipi_init\n");

	return 0;
}

void mtk_apu_debug_remove(struct mtk_apu *apu)
{
	mtk_apu_ipi_unregister(apu, MTK_APU_IPI_LOG_LEVEL);
}
