/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#ifndef __HW_LOGGER_H__
#define __HW_LOGGER_H__

struct mtk_apu;
struct mtk_apu_hw_logger;

dma_addr_t mtk_apu_hw_logger_config_init(struct mtk_apu_hw_logger *hw_logger_data);
int mtk_apu_hw_logger_deep_idle_enter_pre(struct mtk_apu_hw_logger *hw_logger_data);
int mtk_apu_hw_logger_deep_idle_enter_post(struct mtk_apu_hw_logger *hw_logger_data);
int mtk_apu_hw_logger_deep_idle_leave(struct mtk_apu_hw_logger *hw_logger_data);
void mtk_apu_hw_log_level_ipi_handler(void *data, unsigned int len, void *priv);
void mtk_apu_hw_logger_set_ipi_send(struct mtk_apu_hw_logger *hw_logger_data, struct mtk_apu *apu,
				    int (*func)(struct mtk_apu*, u32, void*, u32, u32));
void mtk_apu_hw_logger_unset_ipi_send(struct mtk_apu_hw_logger *hw_logger_data);
struct mtk_apu_hw_logger* get_mtk_apu_hw_logger_device(struct device_node *hw_logger_np);

#endif /* __HW_LOGGER_H__ */
