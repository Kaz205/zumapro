// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/sched/clock.h>

#include <linux/mailbox/mtk-apu-mailbox.h>
#include <linux/remoteproc/mtk_apu.h>
#include <linux/remoteproc/mtk_apu_config.h>
#include <linux/soc/mediatek/mtk_apu_hw_logger.h>
#include "mtk_apu_hw.h"
#include "mtk_apu_rproc.h"

MODULE_IMPORT_NS(MTK_APU_MAILBOX);

static void mtk_apu_config_user_ptr_init(const struct mtk_apu *apu)
{
	struct config_v1 *config;
	struct config_v1_entry_table *entry_table;

	if (!apu || !apu->conf_buf) {
		pr_err("%s: error\n", __func__);
		return;
	}

	config = apu->conf_buf;

	config->header_magic = 0xc0de0101;
	config->header_rev = 0x1;
	config->entry_offset = offsetof(struct config_v1, entry_tbl);
	config->config_size = sizeof(struct config_v1);

	entry_table = (struct config_v1_entry_table *)((void *)config + config->entry_offset);

	entry_table->user_entry[0] = offsetof(struct config_v1, user0_data);
	entry_table->user_entry[1] = offsetof(struct config_v1, user1_data);
	entry_table->user_entry[2] = offsetof(struct config_v1, user2_data);
	entry_table->user_entry[3] = offsetof(struct config_v1, user3_data);
	entry_table->user_entry[4] = offsetof(struct config_v1, user4_data);
	entry_table->user_entry[5] = offsetof(struct config_v1, user5_data);
}

int mtk_apu_config_setup(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;
	unsigned long flags;
	int ret;

	apu->conf_buf = dma_alloc_coherent(apu->dev, CONFIG_SIZE, &apu->conf_da, GFP_KERNEL);

	if (apu->conf_buf == NULL || apu->conf_da == 0) {
		dev_err(dev, "%s: dma_alloc_coherent fail\n", __func__);
		return -ENOMEM;
	}

	dev_dbg(dev, "%s: apu->conf_buf = %p, apu->conf_da = %pad\n", __func__,
		apu->conf_buf, &apu->conf_da);

	mtk_apu_config_user_ptr_init(apu);

	spin_lock_irqsave(&apu->reg_lock, flags);
	/* Set config addr in mbox */
	mtk_apu_mbox_write((u32)apu->conf_da, MBOX_HOST_CONFIG_ADDR);
	spin_unlock_irqrestore(&apu->reg_lock, flags);

	apu->conf_buf->time_offset = sched_clock();

	ret = mtk_apu_ipi_config_init(apu);
	if (ret) {
		dev_err(dev, "apu ipi config init failed: %d\n", ret);
		goto free_apu_conf_buf;
	}

	if (apu->hw_logger_data != NULL) {
		dma_addr_t hw_log_buf_dma_addr;
		struct logger_init_info *st_logger_init_info;

		hw_log_buf_dma_addr = mtk_apu_hw_logger_config_init(apu->hw_logger_data);
		st_logger_init_info = (struct logger_init_info *)
			get_mtk_apu_config_user_ptr(apu->conf_buf, eLOGGER_INIT_INFO);

		if (hw_log_buf_dma_addr) {
			st_logger_init_info->iova = lower_32_bits(hw_log_buf_dma_addr);
			st_logger_init_info->iova_h = upper_32_bits(hw_log_buf_dma_addr);
			dev_dbg(dev, "set st_logger_init_info iova = 0x%x, iova_h = 0x%x\n",
				st_logger_init_info->iova,
				st_logger_init_info->iova_h);
		}
		else {
			dev_err(dev, "hw_log_buf_dma_addr is NULL\n");
		}
	}

	return 0;

free_apu_conf_buf:
	dma_free_coherent(apu->dev, CONFIG_SIZE, apu->conf_buf, apu->conf_da);

	return ret;
}

void mtk_apu_config_remove(struct mtk_apu *apu)
{
	mtk_apu_ipi_config_remove(apu);

	dma_free_coherent(apu->dev, CONFIG_SIZE, apu->conf_buf, apu->conf_da);
}
