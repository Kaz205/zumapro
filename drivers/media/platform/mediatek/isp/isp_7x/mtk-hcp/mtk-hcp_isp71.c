// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */
#include <linux/slab.h>
#include <linux/kref.h>
#include "mtk-hcp_isp71.h"

static struct mtk_hcp_reserve_mblock *mb;

static struct mtk_hcp *s_hcp_dev;

struct mtk_hcp_reserve_mblock isp71_reserve_mblock[] = {
	{
		/*share buffer for frame setting, to be sw usage*/
		.name = "IMG_MEM_FOR_HW_ID",
		.num = IMG_MEM_FOR_HW_ID,
		.start_phys = 0x0,
		.start_virt = NULL,
		.start_dma  = 0x0,
		.size = 0x400000,   /*need more than 4MB*/
		.is_dma_buf = true,
		.mmap_cnt = 0,
		.mem_priv = NULL,
		.d_buf = NULL,
		.fd = -1,
		.attach = NULL,
		.sgt = NULL
	},
	{
		.name = "WPE_MEM_C_ID",
		.num = WPE_MEM_C_ID,
		.start_phys = 0x0,
		.start_virt = NULL,
		.start_dma  = 0x0,
		.size = 0xE1000,   /*900KB*/
		.is_dma_buf = true,
		.mmap_cnt = 0,
		.mem_priv = NULL,
		.d_buf = NULL,
		.fd = -1,
		.attach = NULL,
		.sgt = NULL
	},
	{
		.name = "WPE_MEM_T_ID",
		.num = WPE_MEM_T_ID,
		.start_phys = 0x0,
		.start_virt = NULL,
		.start_dma  = 0x0,
		.size = 0x196000,   /*1MB + 600KB*/
		.is_dma_buf = true,
		.mmap_cnt = 0,
		.mem_priv = NULL,
		.d_buf = NULL,
		.fd = -1,
		.attach = NULL,
		.sgt = NULL
	},
	{
		.name = "TRAW_MEM_C_ID",
		.num = TRAW_MEM_C_ID,
		.start_phys = 0x0,
		.start_virt = NULL,
		.start_dma  = 0x0,
		.size = 0x4C8000,   /*4MB + 800KB*/
		.is_dma_buf = true,
		.mmap_cnt = 0,
		.mem_priv = NULL,
		.d_buf = NULL,
		.fd = -1,
		.attach = NULL,
		.sgt = NULL
	},
	{
		.name = "TRAW_MEM_T_ID",
		.num = TRAW_MEM_T_ID,
		.start_phys = 0x0,
		.start_virt = NULL,
		.start_dma  = 0x0,
		.size = 0x1AC8000,   /*26MB + 800KB*/
		.is_dma_buf = true,
		.mmap_cnt = 0,
		.mem_priv = NULL,
		.d_buf = NULL,
		.fd = -1,
		.attach = NULL,
		.sgt = NULL
	},
	{
		.name = "DIP_MEM_C_ID",
		.num = DIP_MEM_C_ID,
		.start_phys = 0x0,
		.start_virt = NULL,
		.start_dma  = 0x0,
		.size = 0x5C8000,   /*5MB + 800KB*/
		.is_dma_buf = true,
		.mmap_cnt = 0,
		.mem_priv = NULL,
		.d_buf = NULL,
		.fd = -1,
		.attach = NULL,
		.sgt = NULL
	},
	{
		.name = "DIP_MEM_T_ID",
		.num = DIP_MEM_T_ID,
		.start_phys = 0x0,
		.start_virt = NULL,
		.start_dma  = 0x0,
		.size = 0x1FAF000,   /*31MB + 700KB*/
		.is_dma_buf = true,
		.mmap_cnt = 0,
		.mem_priv = NULL,
		.d_buf = NULL,
		.fd = -1,
		.attach = NULL,
		.sgt = NULL
	},
	{
		.name = "PQDIP_MEM_C_ID",
		.num = PQDIP_MEM_C_ID,
		.start_phys = 0x0,
		.start_virt = NULL,
		.start_dma  = 0x0,
		.size = 0x100000,   /*1MB*/
		.is_dma_buf = true,
		.mmap_cnt = 0,
		.mem_priv = NULL,
		.d_buf = NULL,
		.fd = -1,
		.attach = NULL,
		.sgt = NULL
	},
	{
		.name = "PQDIP_MEM_T_ID",
		.num = PQDIP_MEM_T_ID,
		.start_phys = 0x0,
		.start_virt = NULL,
		.start_dma  = 0x0,
		.size = 0x170000,   /*1MB + 500KB*/
		.is_dma_buf = true,
		.mmap_cnt = 0,
		.mem_priv = NULL,
		.d_buf = NULL,
		.fd = -1,
		.attach = NULL,
		.sgt = NULL
	},
	{
		.name = "ADL_MEM_C_ID",
		.num = ADL_MEM_C_ID,
		.start_phys = 0x0,
		.start_virt = NULL,
		.start_dma  = 0x0,
		.size = 0x100000,   /*1MB*/
		.is_dma_buf = true,
		.mmap_cnt = 0,
		.mem_priv = NULL,
		.d_buf = NULL,
		.fd = -1,
	},
	{
		.name = "ADL_MEM_T_ID",
		.num = ADL_MEM_T_ID,
		.start_phys = 0x0,
		.start_virt = NULL,
		.start_dma  = 0x0,
		.size = 0x200000,   /*2MB*/
		.is_dma_buf = true,
		.mmap_cnt = 0,
		.mem_priv = NULL,
		.d_buf = NULL,
		.fd = -1,
	},
	{
		.name = "IMG_MEM_G_ID",
		.num = IMG_MEM_G_ID,
		.start_phys = 0x0,
		.start_virt = NULL,
		.start_dma  = 0x0,
		.size = 0x1800000,
		.is_dma_buf = true,
		.mmap_cnt = 0,
		.mem_priv = NULL,
		.d_buf = NULL,
		.fd = -1,
		.attach = NULL,
		.sgt = NULL
	},
};

static void *isp71_get_gce_virt(void)
{
	return mb[IMG_MEM_G_ID].start_virt;
}

static phys_addr_t isp71_get_reserve_mem_phys(unsigned int id)
{
	if (id >= NUMS_MEM_ID) {
		pr_info("[HCP] no reserve memory for %d", id);
		return 0;
	} else {
		return mb[id].start_phys;
	}
}

static void *isp71_get_reserve_mem_virt(unsigned int id)
{
	if (id >= NUMS_MEM_ID)
		return NULL;
	else
		return mb[id].start_virt;
}

static phys_addr_t isp71_get_reserve_mem_dma(unsigned int id)
{
	if (id >= NUMS_MEM_ID) {
		pr_info("[HCP] no reserve memory for %d", id);
		return 0;
	} else {
		return mb[id].start_dma;
	}
}

static phys_addr_t isp71_get_reserve_mem_size(unsigned int id)
{
	if (id >= NUMS_MEM_ID) {
		pr_info("[HCP] no reserve memory for %d", id);
		return 0;
	} else {
		return mb[id].size;
	}
}

static u32 isp71_get_reserve_mem_fd(unsigned int id)
{
	if (id >= NUMS_MEM_ID)
		return 0;
	else
		return mb[id].fd;
}

static phys_addr_t isp71_get_gce_mem_size(void)
{
	return mb[IMG_MEM_G_ID].size;
}

static int isp71_allocate_working_buffer(struct mtk_hcp *hcp_dev, unsigned int mode)
{
	enum isp71_rsv_mem_id_t id;
	struct mtk_hcp_reserve_mblock *mblock;
	unsigned int block_num;
	void *ptr;
	dma_addr_t addr;

	if (mode)
		return -EINVAL;

	mblock = hcp_dev->data->mblock;
	mb = mblock;
	block_num = hcp_dev->data->block_num;
	for (id = 0; id < block_num; id++) {
		if (mblock[id].is_dma_buf) {
			switch (id) {
			case IMG_MEM_FOR_HW_ID:
				/*allocated at probe via dts*/
				break;
			case IMG_MEM_G_ID:
				s_hcp_dev = hcp_dev;

				/* imgsys working buffer */
				ptr = dma_alloc_coherent(hcp_dev->scp_dev, mblock[id].size,
							 &addr, GFP_KERNEL);
				if (!ptr)
					return -ENOMEM;

				mblock[id].start_virt = ptr;
				mblock[id].start_phys = addr;
				mblock[id].start_dma =
					dma_map_resource(hcp_dev->dev, addr, mblock[id].size,
							 DMA_BIDIRECTIONAL, DMA_ATTR_SKIP_CPU_SYNC);
				if (dma_mapping_error(hcp_dev->dev, mblock[id].start_dma)) {
					dev_err(hcp_dev->dev, "failed to map scp iova\n");
					return -ENOMEM;
				}

				kref_init(&mblock[id].kref);
				break;
			default:
				/* imgsys working buffer */
				ptr = dma_alloc_coherent(hcp_dev->scp_dev, mblock[id].size,
							 &addr, GFP_KERNEL);
				if (!ptr)
					return -ENOMEM;

				mblock[id].start_virt = ptr;
				mblock[id].start_phys = addr;
				mblock[id].start_dma =
					dma_map_resource(hcp_dev->dev, addr, mblock[id].size,
							 DMA_TO_DEVICE, DMA_ATTR_SKIP_CPU_SYNC);
				if (dma_mapping_error(hcp_dev->dev, mblock[id].start_dma)) {
					dev_err(hcp_dev->dev, "failed to map scp iova\n");
					return -ENOMEM;
				}
				break;
			}
		} else {
			mblock[id].start_virt =
				kzalloc(mblock[id].size,
					GFP_KERNEL);
			mblock[id].start_phys = virt_to_phys(mblock[id].start_virt);
			mblock[id].start_dma = 0;
		}
	}

	return 0;
}

static void gce_release(struct kref *ref)
{
	struct mtk_hcp_reserve_mblock *mblock =
		container_of(ref, struct mtk_hcp_reserve_mblock, kref);

	mblock->mem_priv = NULL;
	mblock->mmap_cnt = 0;
	mblock->start_dma = 0x0;
	mblock->start_virt = NULL;
	mblock->start_phys = 0x0;
	mblock->d_buf = NULL;
	mblock->fd = -1;
	mblock->attach = NULL;
	mblock->sgt = NULL;
}

static int isp71_release_working_buffer(struct mtk_hcp *hcp_dev)
{
	enum isp71_rsv_mem_id_t id;
	struct mtk_hcp_reserve_mblock *mblock;

	mblock = mb;

	/* release reserved memory */
	for (id = 0; id < NUMS_MEM_ID; id++) {
		if (mblock[id].is_dma_buf) {
			switch (id) {
			case IMG_MEM_FOR_HW_ID:
				/*allocated at probe via dts*/
				break;
			case IMG_MEM_G_ID:
				dma_unmap_page_attrs(hcp_dev->dev,  mblock[IMG_MEM_G_ID].start_dma,
						     mblock[IMG_MEM_G_ID].size,
						     DMA_BIDIRECTIONAL,
						     DMA_ATTR_SKIP_CPU_SYNC);
				dma_free_coherent(hcp_dev->scp_dev, mblock[IMG_MEM_G_ID].size,
						  mblock[IMG_MEM_G_ID].start_virt,
						  mblock[IMG_MEM_G_ID].start_phys);
				kref_put(&mblock[id].kref, gce_release);
				break;
			default:
				dma_unmap_page_attrs(hcp_dev->dev,  mblock[id].start_dma,
						     mblock[id].size,
						     DMA_TO_DEVICE,
						     DMA_ATTR_SKIP_CPU_SYNC);
				dma_free_coherent(hcp_dev->scp_dev, mblock[id].size,
						  mblock[id].start_virt,
						  mblock[id].start_phys);

				mblock[id].mem_priv = NULL;
				mblock[id].mmap_cnt = 0;
				mblock[id].start_dma = 0x0;
				mblock[id].start_virt = NULL;
				mblock[id].start_phys = 0x0;
				mblock[id].d_buf = NULL;
				mblock[id].fd = -1;
				mblock[id].attach = NULL;
				mblock[id].sgt = NULL;
				break;
			}
		} else {
			kfree(mblock[id].start_virt);
			mblock[id].start_virt = NULL;
			mblock[id].start_phys = 0x0;
			mblock[id].start_dma = 0x0;
			mblock[id].mmap_cnt = 0;
		}
	}

	return 0;
}

static int isp71_get_init_info(struct img_init_info *info)
{
	if (!info) {
		pr_info("%s:NULL info\n", __func__);
		return -1;
	}

	info->hw_buf = isp71_get_reserve_mem_phys(DIP_MEM_FOR_HW_ID);
	info->module_info[0].c_wbuf =
				isp71_get_reserve_mem_phys(WPE_MEM_C_ID);
	info->module_info[0].c_wbuf_dma =
				isp71_get_reserve_mem_dma(WPE_MEM_C_ID);
	info->module_info[0].c_wbuf_sz =
				isp71_get_reserve_mem_size(WPE_MEM_C_ID);
	info->module_info[0].c_wbuf_fd =
				isp71_get_reserve_mem_fd(WPE_MEM_C_ID);
	info->module_info[0].t_wbuf =
				isp71_get_reserve_mem_phys(WPE_MEM_T_ID);
	info->module_info[0].t_wbuf_dma =
				isp71_get_reserve_mem_dma(WPE_MEM_T_ID);
	info->module_info[0].t_wbuf_sz =
				isp71_get_reserve_mem_size(WPE_MEM_T_ID);
	info->module_info[0].t_wbuf_fd =
				isp71_get_reserve_mem_fd(WPE_MEM_T_ID);

	info->module_info[1].c_wbuf =
				isp71_get_reserve_mem_phys(ADL_MEM_C_ID);
	info->module_info[1].c_wbuf_dma =
				isp71_get_reserve_mem_dma(ADL_MEM_C_ID);
	info->module_info[1].c_wbuf_sz =
				isp71_get_reserve_mem_size(ADL_MEM_C_ID);
	info->module_info[1].c_wbuf_fd =
				isp71_get_reserve_mem_fd(ADL_MEM_C_ID);
	info->module_info[1].t_wbuf =
				isp71_get_reserve_mem_phys(ADL_MEM_T_ID);
	info->module_info[1].t_wbuf_dma =
				isp71_get_reserve_mem_dma(ADL_MEM_T_ID);
	info->module_info[1].t_wbuf_sz =
				isp71_get_reserve_mem_size(ADL_MEM_T_ID);
	info->module_info[1].t_wbuf_fd =
				isp71_get_reserve_mem_fd(ADL_MEM_T_ID);

	info->module_info[2].c_wbuf =
				isp71_get_reserve_mem_phys(TRAW_MEM_C_ID);
	info->module_info[2].c_wbuf_dma =
				isp71_get_reserve_mem_dma(TRAW_MEM_C_ID);
	info->module_info[2].c_wbuf_sz =
				isp71_get_reserve_mem_size(TRAW_MEM_C_ID);
	info->module_info[2].c_wbuf_fd =
				isp71_get_reserve_mem_fd(TRAW_MEM_C_ID);
	info->module_info[2].t_wbuf =
				isp71_get_reserve_mem_phys(TRAW_MEM_T_ID);
	info->module_info[2].t_wbuf_dma =
				isp71_get_reserve_mem_dma(TRAW_MEM_T_ID);
	info->module_info[2].t_wbuf_sz =
				isp71_get_reserve_mem_size(TRAW_MEM_T_ID);
	info->module_info[2].t_wbuf_fd =
				isp71_get_reserve_mem_fd(TRAW_MEM_T_ID);

	info->module_info[3].c_wbuf =
				isp71_get_reserve_mem_phys(DIP_MEM_C_ID);
	info->module_info[3].c_wbuf_dma =
				isp71_get_reserve_mem_dma(DIP_MEM_C_ID);
	info->module_info[3].c_wbuf_sz =
				isp71_get_reserve_mem_size(DIP_MEM_C_ID);
	info->module_info[3].c_wbuf_fd =
				isp71_get_reserve_mem_fd(DIP_MEM_C_ID);
	info->module_info[3].t_wbuf =
				isp71_get_reserve_mem_phys(DIP_MEM_T_ID);
	info->module_info[3].t_wbuf_dma =
				isp71_get_reserve_mem_dma(DIP_MEM_T_ID);
	info->module_info[3].t_wbuf_sz =
				isp71_get_reserve_mem_size(DIP_MEM_T_ID);
	info->module_info[3].t_wbuf_fd =
				isp71_get_reserve_mem_fd(DIP_MEM_T_ID);

	info->module_info[4].c_wbuf =
				isp71_get_reserve_mem_phys(PQDIP_MEM_C_ID);
	info->module_info[4].c_wbuf_dma =
				isp71_get_reserve_mem_dma(PQDIP_MEM_C_ID);
	info->module_info[4].c_wbuf_sz =
				isp71_get_reserve_mem_size(PQDIP_MEM_C_ID);
	info->module_info[4].c_wbuf_fd =
			isp71_get_reserve_mem_fd(PQDIP_MEM_C_ID);
	info->module_info[4].t_wbuf =
				isp71_get_reserve_mem_phys(PQDIP_MEM_T_ID);
	info->module_info[4].t_wbuf_dma =
				isp71_get_reserve_mem_dma(PQDIP_MEM_T_ID);
	info->module_info[4].t_wbuf_sz =
				isp71_get_reserve_mem_size(PQDIP_MEM_T_ID);
	info->module_info[4].t_wbuf_fd =
				isp71_get_reserve_mem_fd(PQDIP_MEM_T_ID);

	info->g_wbuf_fd = isp71_get_reserve_mem_fd(IMG_MEM_G_ID);
	info->g_wbuf = isp71_get_reserve_mem_phys(IMG_MEM_G_ID);
	info->g_wbuf_sz = isp71_get_reserve_mem_size(IMG_MEM_G_ID);

	return 0;
}

struct mtk_hcp_data isp71_hcp_data = {
	.mblock = isp71_reserve_mblock,
	.block_num = ARRAY_SIZE(isp71_reserve_mblock),
	.allocate = isp71_allocate_working_buffer,
	.release = isp71_release_working_buffer,
	.get_init_info = isp71_get_init_info,
	.get_gce_virt = isp71_get_gce_virt,
	.get_gce_mem_size = isp71_get_gce_mem_size,
	.get_reserve_mem_virt = isp71_get_reserve_mem_virt,
};
EXPORT_SYMBOL_GPL(isp71_hcp_data);

MODULE_LICENSE("GPL");