/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#ifndef MTK_APU_H
#define MTK_APU_H
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/mailbox_client.h>

#include <linux/remoteproc/mtk_apu_config.h>
#include <linux/remoteproc/mtk_apu_ipi.h>

struct mtk_apu;

struct mtk_apu_hw_ops {
	int (*start)(struct mtk_apu *apu);
	int (*stop)(struct mtk_apu *apu);
	int (*mtk_apu_memmap_init)(struct mtk_apu *apu);

	/* power related ops */
	int (*power_on)(struct mtk_apu *apu);
	int (*power_off)(struct mtk_apu *apu);
};

struct mtk_apu_plat_config {
	uint64_t up_code_buf_sz;
	uint64_t up_coredump_buf_sz;
	uint64_t regdump_buf_sz;
	uint64_t mdla_coredump_buf_sz;
	uint64_t mvpu_coredump_buf_sz;
	uint64_t mvpu_sec_coredump_buf_sz;
};

struct mtk_apu_flag {
	bool preload_firmware;
	bool auto_boot;
	bool bypass_iommu;
	bool secure_boot;
	bool secure_coredump;
	bool debug_log_on;
	bool kernel_load_image;
	bool map_iova;
};

struct mtk_apu_platdata {
	struct mtk_apu_flag flags;
	struct mtk_apu_plat_config config;
	struct mtk_apu_hw_ops ops;
	char *fw_name;
};

struct apusys_secure_info_t {
	unsigned int total_sz;
	unsigned int up_code_buf_ofs;
	unsigned int up_code_buf_sz;

	unsigned int up_fw_ofs;
	unsigned int up_fw_sz;
	unsigned int up_xfile_ofs;
	unsigned int up_xfile_sz;
	unsigned int mdla_fw_boot_ofs;
	unsigned int mdla_fw_boot_sz;
	unsigned int mdla_fw_main_ofs;
	unsigned int mdla_fw_main_sz;
	unsigned int mdla_xfile_ofs;
	unsigned int mdla_xfile_sz;
	unsigned int mvpu_fw_ofs;
	unsigned int mvpu_fw_sz;
	unsigned int mvpu_xfile_ofs;
	unsigned int mvpu_xfile_sz;
	unsigned int mvpu_sec_fw_ofs;
	unsigned int mvpu_sec_fw_sz;
	unsigned int mvpu_sec_xfile_ofs;
	unsigned int mvpu_sec_xfile_sz;

	unsigned int up_coredump_ofs;
	unsigned int up_coredump_sz;
	unsigned int mdla_coredump_ofs;
	unsigned int mdla_coredump_sz;
	unsigned int mvpu_coredump_ofs;
	unsigned int mvpu_coredump_sz;
	unsigned int mvpu_sec_coredump_ofs;
	unsigned int mvpu_sec_coredump_sz;
};

struct mtk_apu {
	struct rproc *rproc;
	struct platform_device *pdev;
	struct device *dev;
	void __iomem *md32_tcm;
	void *apu_sec_mem_base;
	int wdt_irq_number;
	spinlock_t reg_lock;

	struct apusys_secure_info_t *apusys_sec_info;
	uint64_t apusys_sec_mem_start;
	uint64_t apusys_sec_mem_size;

	/* hw_logger */
	struct mtk_apu_hw_logger *hw_logger_data;

	/* mailbox */
	struct mbox_client cl;
	struct mbox_chan *ch;

	/* Buffer to place execution area */
	void *code_buf;
	dma_addr_t code_da;

	/* Buffer to place config area */
	struct config_v1 *conf_buf;
	dma_addr_t conf_da;

	/* to synchronize boot status of remote processor */
	struct mtk_apu_run run;

	/* to prevent multiple ipi_send run concurrently */
	struct mutex send_lock;
	spinlock_t usage_cnt_lock;
	struct mtk_apu_ipi_desc ipi_desc[MTK_APU_IPI_MAX];
	u32 ipi_id;
	bool ipi_id_ack[MTK_APU_IPI_MAX]; /* per-ipi ack */
	bool ipi_inbound_locked;
	bool bypass_pwr_off_chk;
	wait_queue_head_t ack_wq; /* for waiting for ipi ack */

	/* ipi share buffer */
	dma_addr_t recv_buf_da;
	struct mtk_share_obj *recv_buf;
	dma_addr_t send_buf_da;
	struct mtk_share_obj *send_buf;
	struct mtk_ipi_task ipi_task;

	struct rproc_subdev *rpmsg_subdev;

	const struct mtk_apu_platdata *platdata;

	/* power status */
	bool boot_done;
	bool top_genpd;

	/* genpd notifier */
	struct notifier_block genpd_nb;

	/* timesync workqueue */
	struct work_struct timesync_work;
	struct workqueue_struct *timesync_workq;

	/* reserved memory */
	uint64_t resv_mem_pa;
	void *resv_mem_va;

	const struct firmware *fw;
};

#endif /* MTK_APU_H */
