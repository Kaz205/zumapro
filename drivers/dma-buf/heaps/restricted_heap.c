// SPDX-License-Identifier: GPL-2.0
/*
 * DMABUF restricted heap exporter
 *
 * Copyright (C) 2024 MediaTek Inc.
 */

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/err.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include "restricted_heap.h"

struct restricted_heap_attachment {
	struct sg_table			*table;
};

static int
restricted_heap_memory_allocate(struct restricted_heap *heap, struct restricted_buffer *buf)
{
	const struct restricted_heap_ops *ops = heap->ops;
	int ret;

	ret = ops->memory_alloc(heap, buf);
	if (ret)
		return ret;

	if (ops->memory_restrict) {
		ret = ops->memory_restrict(heap, buf);
		if (ret)
			goto memory_free;
	}
	return 0;

memory_free:
	ops->memory_free(heap, buf);
	return ret;
}

static void
restricted_heap_memory_free(struct restricted_heap *heap, struct restricted_buffer *buf)
{
	const struct restricted_heap_ops *ops = heap->ops;

	if (ops->memory_unrestrict)
		ops->memory_unrestrict(heap, buf);

	ops->memory_free(heap, buf);
}

static int restricted_heap_attach(struct dma_buf *dmabuf, struct dma_buf_attachment *attachment)
{
	struct restricted_buffer *restricted_buf = dmabuf->priv;
	struct restricted_heap_attachment *a;
	struct sg_table *table;
	int ret;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table) {
		ret = -ENOMEM;
		goto err_free_attach;
	}

	ret = sg_alloc_table(table, 1, GFP_KERNEL);
	if (ret)
		goto err_free_sgt;
	sg_dma_mark_restricted(table->sgl);

	a->table = table;
	attachment->priv = a;

	return 0;

err_free_sgt:
	kfree(table);
err_free_attach:
	kfree(a);
	return ret;
}

static void restricted_heap_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attachment)
{
	struct restricted_heap_attachment *a = attachment->priv;

	sg_free_table(a->table);
	kfree(a->table);
	kfree(a);
}

static struct sg_table *
restricted_heap_map_dma_buf(struct dma_buf_attachment *attachment, enum dma_data_direction direct)
{
	struct restricted_heap_attachment *a = attachment->priv;
	struct dma_buf *dmabuf = attachment->dmabuf;
	struct restricted_buffer *restricted_buf = dmabuf->priv;
	struct sg_table *table = a->table;

	/*
	 * Technically dma_address refers to the address used by HW, But for restricted buffer
	 * we don't know its dma_address in kernel, Instead, we may know its restricted address
	 * which refers to the real buffer in the trusted or secure world. Here use this property
	 * to save the restricted address, and the user will use it to obtain the real address in
	 * trusted or secure world.
	 *
	 * Note: CONFIG_DMA_API_DEBUG requires this to be aligned with PAGE_SIZE.
	 */
	if (sg_dma_is_restricted(table->sgl) && restricted_buf->restricted_addr) {
		sg_dma_address(table->sgl) = restricted_buf->restricted_addr;
		sg_dma_len(table->sgl) = restricted_buf->size;
	}
	return table;
}

static void
restricted_heap_unmap_dma_buf(struct dma_buf_attachment *attachment, struct sg_table *table,
			      enum dma_data_direction direction)
{
	struct restricted_heap_attachment *a = attachment->priv;

	WARN_ON(a->table != table);
	sg_dma_address(table->sgl) = 0;
	sg_dma_len(table->sgl) = 0;
}

static int
restricted_heap_dma_buf_begin_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction direction)
{
	return -EPERM;
}

static int
restricted_heap_dma_buf_end_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction direction)
{
	return -EPERM;
}

static int restricted_heap_dma_buf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	return -EPERM;
}

static void restricted_heap_free(struct dma_buf *dmabuf)
{
	struct restricted_buffer *restricted_buf = dmabuf->priv;
	struct restricted_heap *heap = dma_heap_get_drvdata(restricted_buf->heap);

	restricted_heap_memory_free(heap, restricted_buf);
	kfree(restricted_buf);
}

static const struct dma_buf_ops restricted_heap_buf_ops = {
	.attach		= restricted_heap_attach,
	.detach		= restricted_heap_detach,
	.map_dma_buf	= restricted_heap_map_dma_buf,
	.unmap_dma_buf	= restricted_heap_unmap_dma_buf,
	.begin_cpu_access = restricted_heap_dma_buf_begin_cpu_access,
	.end_cpu_access	= restricted_heap_dma_buf_end_cpu_access,
	.mmap		= restricted_heap_dma_buf_mmap,
	.release	= restricted_heap_free,
};

static struct dma_buf *
restricted_heap_allocate(struct dma_heap *heap, unsigned long size,
			 unsigned long fd_flags, unsigned long heap_flags)
{
	struct restricted_heap *restricted_heap = dma_heap_get_drvdata(heap);
	const struct restricted_heap_ops *ops = restricted_heap->ops;
	struct restricted_buffer *restricted_buf;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	int ret;

	/*
	 * In some implements, TEE is required to protect buffer. However TEE probe
	 * may be late, Thus heap_init is performed when the first buffer is requested.
	 */
	if (ops->heap_init) {
		ret = ops->heap_init(restricted_heap);
		if (ret)
			return ERR_PTR(ret);
	}

	restricted_buf = kzalloc(sizeof(*restricted_buf), GFP_KERNEL);
	if (!restricted_buf)
		return ERR_PTR(-ENOMEM);

	restricted_buf->size = ALIGN(size, PAGE_SIZE);
	restricted_buf->heap = heap;

	ret = restricted_heap_memory_allocate(restricted_heap, restricted_buf);
	if (ret)
		goto err_free_buf;
	exp_info.exp_name = dma_heap_get_name(heap);
	exp_info.ops = &restricted_heap_buf_ops;
	exp_info.size = restricted_buf->size;
	exp_info.flags = fd_flags;
	exp_info.priv = restricted_buf;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto err_free_restricted_mem;
	}

	return dmabuf;

err_free_restricted_mem:
	restricted_heap_memory_free(restricted_heap, restricted_buf);
err_free_buf:
	kfree(restricted_buf);
	return ERR_PTR(ret);
}

static const struct dma_heap_ops restricted_heap_ops = {
	.allocate = restricted_heap_allocate,
};

int restricted_heap_add(struct restricted_heap *rstrd_heap)
{
	struct dma_heap_export_info exp_info;
	struct dma_heap *heap;

	exp_info.name = rstrd_heap->name;
	exp_info.ops = &restricted_heap_ops;
	exp_info.priv = (void *)rstrd_heap;

	heap = dma_heap_add(&exp_info);
	if (IS_ERR(heap))
		return PTR_ERR(heap);
	return 0;
}
EXPORT_SYMBOL_GPL(restricted_heap_add);
