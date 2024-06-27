// SPDX-License-Identifier: GPL-2.0
/*
 * DMABUF restricted heap exporter for MediaTek
 *
 * Copyright (C) 2024 MediaTek Inc.
 */
#define pr_fmt(fmt)     "rheap_mtk: " fmt
#include <linux/cma.h>
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_reserved_mem.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/tee_drv.h>
#include <linux/uuid.h>

#include "restricted_heap.h"

#define TZ_TA_MEM_UUID_MTK		"4477588a-8476-11e2-ad15-e41f1390d676"

#define TEE_PARAM_NUM			4
#define TEE_RESULT_OOM			0xFFFF000C

enum mtk_secure_mem_type {
	/*
	 * MediaTek static chunk memory carved out for TrustZone. The memory
	 * management is inside the TEE.
	 */
	MTK_SECURE_MEMORY_TYPE_CM_TZ	= 1,
	/*
	 * MediaTek dynamic chunk memory carved out from CMA.
	 * In normal case, the CMA could be used in kernel. When SVP start,
	 * The CMA reserved memory will be divided into several blocks in TEE.
	 * We will allocate a block and pass the block PA and size into TEE to
	 * protect it, then the detail memory management also is inside the TEE.
	 */
	MTK_SECURE_MEMORY_TYPE_CM_CMA	= 2,
};

/* This structure also is sync with tee, thus not use the phys_addr_t */
struct mtk_tee_scatterlist {
	u64		pa;
	u32		length;
}__packed;

enum mtk_secure_buffer_tee_cmd {
	/*
	 * Allocate the zeroed secure memory from TEE.
	 *
	 * [in]  value[0].a: The buffer size.
	 *       value[0].b: alignment.
	 * [in]  value[1].a: enum mtk_secure_mem_type.
	 * [inout] [in]  value[2].a: pa base in cma case.
	 *               value[2].b: The buffer size in cma case.
	 *         [out] value[2].a: numbers of mem block. 1 means the memory is continuous.
	 *               value[2].b: buffer PA base.
	 * [out] value[3].a: The secure handle.
	 */
	MTK_TZCMD_SECMEM_ZALLOC	= 0x10000, /* MTK TEE Command ID Base */

	/*
	 * Free secure memory.
	 *
	 * [in]  value[0].a: The secure handle of this buffer, It's value[3].a of
	 *                   MTK_TZCMD_SECMEM_ZALLOC.
	 * [out] value[1].a: return value, 0 means successful, otherwise fail.
	 */
	MTK_TZCMD_SECMEM_FREE	= 0x10001,

	/*
	 * Get secure memory sg-list.
	 *
	 * [in]  value[0].a: The secure handle of this buffer, It's value[3].a of
	 *                   MTK_TZCMD_SECMEM_ZALLOC.
	 * [inout] [in]  value[1].mem.buffer: sg_shm.
	 *               value[1].mem.size: size of sg_shm.
	 *         [out] value[1].mem.buffer: the array of sg items (struct mtk_tee_scatterlist).
	 *               value[1].mem.size: size of sg items.
	 */
	MTK_TZCMD_SECMEM_RETRIEVE_SG	= 0x10002,

	/*
	 * Get secure region number.
	 *
	 * [in]   value[0].a: The CMA reserved memory start address.
	 *        value[0].b: The total size of CMA reserved memory.
	 * [out]  value[0].a: The total region number of secure CMA.
	 */
	MTK_TZCMD_SECMEM_GET_REGION_NUM	= 0x10003,
};

struct mtk_restricted_heap_data {
	struct tee_context		*tee_ctx;
	u32				tee_session;

	const enum mtk_secure_mem_type	mem_type;

	u32				cma_page_index; /* index of cma_pages array */
	u32				cma_used_size;
	u32				cma_hold_size; /* size of blocks held by dma heap */
	u32				cma_hold_block_mask; /* bit mask of blocks held by dma heap */
	u32				cma_block_size; /* size per block */
	u32				cma_block_count; /* number of blocks */
	struct mutex			lock; /* lock for cma_used_size */
	bool				oom_retry; /* true if TEE return OOM */
};

static int mtk_tee_ctx_match(struct tee_ioctl_version_data *ver, const void *data)
{
	return ver->impl_id == TEE_IMPL_ID_OPTEE;
}

static int mtk_tee_session_init(struct mtk_restricted_heap_data *data)
{
	struct tee_param t_param[TEE_PARAM_NUM] = {0};
	struct tee_ioctl_open_session_arg arg = {0};
	uuid_t ta_mem_uuid;
	int ret;

	data->tee_ctx = tee_client_open_context(NULL, mtk_tee_ctx_match, NULL, NULL);
	if (IS_ERR(data->tee_ctx)) {
		pr_err_once("%s: open context failed, ret=%ld\n", __func__,
			    PTR_ERR(data->tee_ctx));
		return -ENODEV;
	}

	arg.num_params = TEE_PARAM_NUM;
	arg.clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
	ret = uuid_parse(TZ_TA_MEM_UUID_MTK, &ta_mem_uuid);
	if (ret)
		goto close_context;
	memcpy(&arg.uuid, &ta_mem_uuid.b, sizeof(ta_mem_uuid));

	ret = tee_client_open_session(data->tee_ctx, &arg, t_param);
	if (ret < 0 || arg.ret) {
		pr_err_once("%s: open session failed, ret=%d:%d\n",
			    __func__, ret, arg.ret);
		ret = -EINVAL;
		goto close_context;
	}
	data->tee_session = arg.session;
	return 0;

close_context:
	tee_client_close_context(data->tee_ctx);
	return ret;
}

static int mtk_tee_service_call(struct tee_context *tee_ctx, u32 session,
				unsigned int command, struct tee_param *params)
{
	struct tee_ioctl_invoke_arg arg = {0};
	int ret;

	arg.num_params = TEE_PARAM_NUM;
	arg.session = session;
	arg.func = command;

	ret = tee_client_invoke_func(tee_ctx, &arg, params);
	if (ret < 0 || arg.ret) {
		pr_err("%s: cmd 0x%x ret %d:%x\n", __func__, command, ret, arg.ret);
		if (arg.ret == TEE_RESULT_OOM)
			ret = -ENOMEM;
		else
			ret = -EOPNOTSUPP;
	}
	return ret;
}

static int mtk_tee_get_cma_region_num(struct restricted_heap *heap)
{
	struct mtk_restricted_heap_data *data = heap->priv_data;
	struct tee_param params[TEE_PARAM_NUM] = {0};
	int ret = 0;

	/*
	 * Send the start address and total size of CMA reserved memory
	 * to TEE at the first time. And get secure region number from TEE.
	 */
	params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT;
	params[0].u.value.a = heap->cma_paddr;
	params[0].u.value.b = heap->cma_size;
	ret = mtk_tee_service_call(data->tee_ctx, data->tee_session,
				   MTK_TZCMD_SECMEM_GET_REGION_NUM, params);
	if (ret)
		return ret;
	data->cma_block_count = params[0].u.value.a;
	if (data->cma_block_count == 0)
		return -EINVAL;
	data->cma_block_size = roundup(heap->cma_size / data->cma_block_count, SZ_4M);
	data->cma_page_index = 0;

	return ret;
}

static int mtk_restricted_memory_cma_allocate(struct restricted_heap *heap,
					      struct restricted_buffer *buf)
{
	struct mtk_restricted_heap_data *data = heap->priv_data;
	unsigned long block_start, block_size, cm_mem_end;
	int ret = 0, i;

	if (!data->cma_block_count) {
		ret = mtk_tee_get_cma_region_num(heap);
		if (ret) {
			pr_err("%s: failed to get cma region num %d\n",
			       __func__, ret);
			return ret;
		}
	}

	/*
	 * The reserved memory will be divided into several blocks in TEE.
	 * Allocate a block from CMA when allocating buffer return OOM in TEE.
	 * The variable "cma_hold_size" and "cma_used_size" will be
	 * increased. Actually the memory allocating is within the TEE.
	 */
	cm_mem_end = heap->cma_paddr + heap->cma_size;
	mutex_lock(&data->lock);
	if ((buf->size + data->cma_used_size) > heap->cma_size) {
		pr_err("%s: failed used 0x%x total_size 0x%lx needed 0x%lx\n",
			__func__, data->cma_used_size, heap->cma_size, buf->size);
		ret = -ENOMEM;
		goto out_unlock;
	}
	if (((buf->size + data->cma_used_size) > data->cma_hold_size) || data->oom_retry) {
		for (i = 0; i < data->cma_block_count; ++i) {
			if (data->cma_hold_block_mask & BIT(i))
				continue;

			block_start = heap->cma_paddr + data->cma_block_size * i;
			block_size = min(data->cma_block_size, cm_mem_end - block_start);
			ret = alloc_contig_range(PFN_DOWN(block_start),
						 PFN_DOWN(block_start + block_size),
						 MIGRATE_CMA, GFP_KERNEL);
			if (ret) {
				pr_err("%s: failed to alloc block %d mask 0x%x, ret %d\n",
					__func__, i, data->cma_hold_block_mask, ret);
				continue;
			}
			data->cma_hold_block_mask |= BIT(i);
			data->cma_hold_size += block_size;
			data->cma_page_index = i;
			break;
		}
	}
	if (data->oom_retry) {
		data->oom_retry = false;
		goto out_unlock;
	}
	data->cma_used_size += buf->size;

out_unlock:
	mutex_unlock(&data->lock);
	return ret;
}

static int mtk_tee_restrict_memory(struct restricted_heap *heap, struct restricted_buffer *buf)
{
	struct mtk_restricted_heap_data *data = heap->priv_data;
	unsigned long block_start, block_size, cm_mem_end;
	struct tee_param params[TEE_PARAM_NUM] = {0};
	struct mtk_tee_scatterlist *tee_sg_item;
	struct mtk_tee_scatterlist *tee_sg_buf;
	unsigned int sg_num, size, i;
	struct tee_shm *sg_shm;
	struct scatterlist *sg;
	phys_addr_t pa_tee;
	int ret = 0, index;
	u64 r_addr;

	/* Alloc the secure buffer and get the sg-list number from TEE */
	params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	params[0].u.value.a = buf->size;
	params[0].u.value.b = PAGE_SIZE;
	params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	params[1].u.value.a = data->mem_type;
	params[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT;

	do {
		if (heap->cma && data->mem_type == MTK_SECURE_MEMORY_TYPE_CM_CMA) {
			mutex_lock(&data->lock);
			index = data->cma_page_index;
			mutex_unlock(&data->lock);
			cm_mem_end = heap->cma_paddr + heap->cma_size;
			block_start = heap->cma_paddr + index * data->cma_block_size;
			block_size = min(data->cma_block_size, cm_mem_end - block_start);
			params[2].u.value.a = block_start;
			params[2].u.value.b = block_size;
		}
		params[3].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;
		ret = mtk_tee_service_call(data->tee_ctx, data->tee_session,
					   MTK_TZCMD_SECMEM_ZALLOC, params);
		if (!ret)
			break;
		else if (ret != -ENOMEM) {
			pr_err("%s failed to alloc buffer in TEE %d\n", __func__, ret);
			return ret;
		}

		/* try again when return OOM in TEE */
		if (!heap->cma || data->mem_type != MTK_SECURE_MEMORY_TYPE_CM_CMA)
			return ret;

		/* TEE require more memory to save meta data, alloc a new block here */
		mutex_lock(&data->lock);
		data->oom_retry = true;
		mutex_unlock(&data->lock);
		ret = mtk_restricted_memory_cma_allocate(heap, buf);
		if (ret)
			return ret;
	} while(!ret);
	r_addr = params[3].u.value.a;
	sg_num = params[2].u.value.a;

	/* If there is only one time, It means the buffer is continous,Get the PA directly. */
	if (sg_num == 1) {
		pa_tee = params[2].u.value.b;
		if (sg_alloc_table(&buf->sg_table, 1, GFP_KERNEL))
			goto tee_secmem_free;
		sg_set_page(buf->sg_table.sgl, phys_to_page(pa_tee), buf->size, 0);
		buf->restricted_addr = r_addr;
		return 0;
	}

	/*
	 * If the buffer inside TEE are discontinuous, Use sharemem to retrieve
	 * the detail sg list from TEE.
	 */
	tee_sg_buf = kmalloc_array(sg_num, sizeof(*tee_sg_item), GFP_KERNEL);
	if (!tee_sg_buf) {
		ret = -ENOMEM;
		goto tee_secmem_free;
	}

	size = sg_num * sizeof(*tee_sg_item);
        sg_shm = tee_shm_register_kernel_buf(data->tee_ctx, tee_sg_buf, size);
        if (!sg_shm)
		goto free_sg_buf;

	memset(params, 0, sizeof(params));
	params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	params[0].u.value.a = r_addr;
	params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
	params[1].u.memref.shm = sg_shm;
	params[1].u.memref.size = size;
	ret = mtk_tee_service_call(data->tee_ctx, data->tee_session,
				   MTK_TZCMD_SECMEM_RETRIEVE_SG, params);
	if (ret)
		goto put_shm;

	if (sg_alloc_table(&buf->sg_table, sg_num, GFP_KERNEL))
		goto put_shm;

	for_each_sgtable_sg(&buf->sg_table, sg, i) {
		tee_sg_item = tee_sg_buf + i;
		sg_set_page(sg, phys_to_page(tee_sg_item->pa),
			    tee_sg_item->length, 0);
	}

	tee_shm_put(sg_shm);
	kfree(tee_sg_buf);
	buf->restricted_addr = r_addr;
	return 0;

put_shm:
	tee_shm_put(sg_shm);
free_sg_buf:
	kfree(tee_sg_buf);
tee_secmem_free:
	params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	params[0].u.value.a = r_addr;
	params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;
	mtk_tee_service_call(data->tee_ctx, data->tee_session,
			     MTK_TZCMD_SECMEM_FREE, params);
	return ret;
}

static void mtk_tee_unrestrict_memory(struct restricted_heap *heap, struct restricted_buffer *buf)
{
	struct mtk_restricted_heap_data *data = heap->priv_data;
	struct tee_param params[TEE_PARAM_NUM] = {0};

	sg_free_table(&buf->sg_table);

	params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	params[0].u.value.a = buf->restricted_addr;
	params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;

	mtk_tee_service_call(data->tee_ctx, data->tee_session,
			     MTK_TZCMD_SECMEM_FREE, params);
	if (params[1].u.value.a)
		pr_err("%s, Unrestrict buffer(0x%llx) fail(%lld) from TEE.\n",
		       heap->name, buf->restricted_addr, params[1].u.value.a);
}

static int
mtk_restricted_memory_allocate(struct restricted_heap *heap, struct restricted_buffer *buf)
{
	/* The memory allocating is within the TEE. */
	return 0;
}

static void
mtk_restricted_memory_free(struct restricted_heap *heap, struct restricted_buffer *buf)
{
}

static void mtk_restricted_memory_cma_free(struct restricted_heap *heap,
					   struct restricted_buffer *buf)
{
	struct mtk_restricted_heap_data *data = heap->priv_data;
	unsigned long block_start, block_size, cm_mem_end;
	int i;

	cm_mem_end = heap->cma_paddr + heap->cma_size;
	mutex_lock(&data->lock);
	data->cma_used_size -= buf->size;
	if (!data->cma_used_size) {
		for (i = 0; i < data->cma_block_count; ++i) {
			if (!(data->cma_hold_block_mask & BIT(i)))
				continue;

			block_start = heap->cma_paddr + data->cma_block_size * i;
			block_size = min(data->cma_block_size, cm_mem_end - block_start);
			free_contig_range(PFN_DOWN(block_start), block_size >> PAGE_SHIFT);
		}
		data->cma_page_index = 0;
		data->cma_hold_size = 0;
		data->cma_block_count = 0;
		data->cma_hold_block_mask = 0;
		data->oom_retry = false;
	}

	mutex_unlock(&data->lock);
}

static struct sg_table
*mtk_restricted_map_dma_buf(struct sg_table *table, struct restricted_buffer *buf,
			   enum dma_data_direction direct)
{
	struct scatterlist *buf_sg = buf->sg_table.sgl;
	struct scatterlist *sg;
	unsigned int i;

	for_each_sgtable_sg(table, sg, i) {
		sg_dma_address(sg) = sg_phys(buf_sg);
		sg_dma_len(sg) = buf_sg->length;
		buf_sg = sg_next(buf_sg);
	}
	return table;
}

static void
mtk_restricted_unmap_dma_buf(struct sg_table *table, struct restricted_buffer *buf,
			     enum dma_data_direction direct)
{
}

static int mtk_restricted_heap_init(struct restricted_heap *heap)
{
	struct mtk_restricted_heap_data *data = heap->priv_data;

	if (!data->tee_ctx)
		return mtk_tee_session_init(data);
	return 0;
}

static const struct restricted_heap_ops mtk_restricted_heap_ops = {
	.heap_init		= mtk_restricted_heap_init,
	.memory_alloc		= mtk_restricted_memory_allocate,
	.memory_free		= mtk_restricted_memory_free,
	.memory_restrict	= mtk_tee_restrict_memory,
	.memory_unrestrict	= mtk_tee_unrestrict_memory,
	.map_dma_buf		= mtk_restricted_map_dma_buf,
	.unmap_dma_buf		= mtk_restricted_unmap_dma_buf,
};

static struct mtk_restricted_heap_data mtk_restricted_heap_data = {
	.mem_type		= MTK_SECURE_MEMORY_TYPE_CM_TZ,
};

static const struct restricted_heap_ops mtk_restricted_heap_ops_cma = {
	.heap_init		= mtk_restricted_heap_init,
	.memory_alloc		= mtk_restricted_memory_cma_allocate,
	.memory_free		= mtk_restricted_memory_cma_free,
	.memory_restrict	= mtk_tee_restrict_memory,
	.memory_unrestrict	= mtk_tee_unrestrict_memory,
	.map_dma_buf		= mtk_restricted_map_dma_buf,
	.unmap_dma_buf		= mtk_restricted_unmap_dma_buf,
};

static struct mtk_restricted_heap_data mtk_restricted_heap_data_cma = {
	.mem_type		= MTK_SECURE_MEMORY_TYPE_CM_CMA,
};

static struct restricted_heap mtk_restricted_heaps[] = {
	{
		.name		= "restricted_mtk_cm",
		.ops		= &mtk_restricted_heap_ops,
		.priv_data	= &mtk_restricted_heap_data,
	},
	{
		.name		= "restricted_mtk_cma",
		.ops		= &mtk_restricted_heap_ops_cma,
		.priv_data	= &mtk_restricted_heap_data_cma,
	},
};

static int __init mtk_restricted_cma_init(struct reserved_mem *rmem)
{
	struct restricted_heap *rstrd_heap = mtk_restricted_heaps, *rstrd_heap_cma = NULL;
	struct mtk_restricted_heap_data *data;
	struct cma *cma;
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(mtk_restricted_heaps); i++, rstrd_heap++) {
		data = rstrd_heap->priv_data;
		if (data->mem_type == MTK_SECURE_MEMORY_TYPE_CM_CMA) {
			rstrd_heap_cma = rstrd_heap;
			break;
		}
	}
	if (!rstrd_heap_cma)
		return -EINVAL;

	ret = cma_init_reserved_mem(rmem->base, rmem->size, 0, rmem->name,
				    &cma);
	if (ret) {
		pr_err("%s: %s set up CMA fail. ret %d.\n", __func__, rmem->name, ret);
		return ret;
	}

	rstrd_heap_cma->cma = cma;
	rstrd_heap_cma->cma_paddr = rmem->base;
	rstrd_heap_cma->cma_size = rmem->size;
	return 0;
}

RESERVEDMEM_OF_DECLARE(restricted_cma, "mediatek,dynamic-restricted-region",
		       mtk_restricted_cma_init);

static int mtk_restricted_heap_initialize(void)
{
	struct restricted_heap *rstrd_heap = mtk_restricted_heaps;
	struct mtk_restricted_heap_data *data;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mtk_restricted_heaps); i++, rstrd_heap++) {
		data = rstrd_heap->priv_data;
		if (data->mem_type == MTK_SECURE_MEMORY_TYPE_CM_CMA && !rstrd_heap->cma)
			continue;
		if (!restricted_heap_add(rstrd_heap))
			mutex_init(&data->lock);
	}
	return 0;
}
module_init(mtk_restricted_heap_initialize);
MODULE_DESCRIPTION("MediaTek Restricted Heap Driver");
MODULE_LICENSE("GPL");
