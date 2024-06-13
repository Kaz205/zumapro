/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#ifndef MTK_APU_CONFIG_H
#define MTK_APU_CONFIG_H

struct mtk_apu;

struct mtk_apu_ipi_config {
	u64 in_buf_da;
	u64 out_buf_da;
};

struct vpu_init_info {
	uint32_t vpu_num;
	uint32_t cfg_addr;
	uint32_t cfg_size;
	uint32_t algo_info_ptr[3 * 2];
	uint32_t rst_vec[3];
	uint32_t dmem_addr[3];
	uint32_t imem_addr[3];
	uint32_t iram_addr[3];
	uint32_t cmd_addr[3];
	uint32_t log_addr[3];
	uint32_t log_size[3];
};

struct apusys_chip_data {
	uint32_t s_code;
	uint32_t b_code;
	uint32_t r_code;
	uint32_t a_code;
};

struct logger_init_info {
	uint32_t iova;
	uint32_t iova_h;
};

struct reviser_init_info {
	uint64_t dram[32];
	uint32_t boundary;
	uint32_t _reserved;
};

struct mvpu_preempt_data {
	uint32_t itcm_buffer_core_0[5];
	uint32_t l1_buffer_core_0[5];
	uint32_t itcm_buffer_core_1[5];
	uint32_t l1_buffer_core_1[5];
};

enum user_config {
	eMTK_APU_IPI_CONFIG = 0x0,
	eVPU_INIT_INFO,
	eAPUSYS_CHIP_DATA,
	eLOGGER_INIT_INFO,
	eREVISER_INIT_INFO,
	eMVPU_PREEMPT_DATA,
	eUSER_CONFIG_MAX
};

struct config_v1_entry_table {
	u32 user_entry[eUSER_CONFIG_MAX];
};

struct config_v1 {
	/* header begin */
	u32 header_magic;
	u32 header_rev;
	u32 entry_offset;
	u32 config_size;
	/* header end */
	/* do not add new member before this line */

	/* system related config begin */
	u64 time_offset;
	u32 ramdump_offset;
	u32 ramdump_type;
	u32 ramdump_module;
	u32 _reserved;
	/* system related config end */

	/* entry table */
	struct config_v1_entry_table entry_tbl;

	/* user data payload begin */
	struct mtk_apu_ipi_config user0_data;
	struct vpu_init_info user1_data;
	struct apusys_chip_data user2_data;
	struct logger_init_info user3_data;
	struct reviser_init_info user4_data;
	struct mvpu_preempt_data user5_data;
	/* user data payload end */
};

void mtk_apu_ipi_config_remove(struct mtk_apu *apu);
int mtk_apu_ipi_config_init(struct mtk_apu *apu);

static inline void *get_mtk_apu_config_user_ptr(struct config_v1 *conf,
	enum user_config user_id)
{
	struct config_v1_entry_table *entry_tbl;

	if (!conf)
		return NULL;

	if (user_id >= eUSER_CONFIG_MAX)
		return NULL;

	entry_tbl = (struct config_v1_entry_table *)
		((void *)conf + conf->entry_offset);

	return (void *)conf + entry_tbl->user_entry[user_id];
}
#endif /* MTK_APU_CONFIG_H */
