/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#ifndef MTK_APU_RPROC_H
#define MTK_APU_RPROC_H

#define TCM_SIZE (128UL * 1024UL)
#define CODE_BUF_SIZE (1024UL * 1024UL)
#define CONFIG_SIZE (round_up(sizeof(struct config_v1), PAGE_SIZE))
#define DRAM_OFFSET (0x00000UL)
#define TCM_OFFSET (0x1d000000UL)
#define CODE_BUF_DA (DRAM_OFFSET)
#define MTK_APU_SEC_FW_IOVA (0x200000UL)

int mtk_apu_config_setup(struct mtk_apu *apu);
void mtk_apu_config_remove(struct mtk_apu *apu);
int mtk_apu_debug_init(struct mtk_apu *apu);
void mtk_apu_debug_remove(struct mtk_apu *apu);
int mtk_apu_deepidle_init(struct mtk_apu *apu);
void mtk_apu_deepidle_exit(struct mtk_apu *apu);
int mtk_apu_deepidle_power_on_aputop(struct mtk_apu *apu);
int mtk_apu_exception_init(struct platform_device *pdev, struct mtk_apu *apu);
void mtk_apu_exception_exit(struct platform_device *pdev, struct mtk_apu *apu);
int mtk_apu_ipi_init(struct platform_device *pdev, struct mtk_apu *apu);
int mtk_apu_ipi_send(struct mtk_apu *apu, u32 id, void *data, u32 len, u32 wait_ms);
int mtk_apu_mem_init(struct mtk_apu *apu);
uint32_t mtk_apu_rv_smc_call(struct device *dev, uint32_t smc_id, uint32_t a2);
int mtk_apu_timesync_init(struct mtk_apu *apu);
void mtk_apu_timesync_remove(struct mtk_apu *apu);

int mtk_apu_load(struct rproc *rproc, const struct firmware *fw);

#endif /* MTK_APU_RPROC_H */
