/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DMABUF Heaps Allocation Infrastructure
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (C) 2019 Linaro Ltd.
 */

#ifndef _DMA_HEAPS_H
#define _DMA_HEAPS_H

#include <linux/device.h>
#include <linux/types.h>

struct dma_heap;

/**
 * struct dma_heap_ops - ops to operate on a given heap
 * @allocate:	allocate dmabuf and return struct dma_buf ptr
 *
 * allocate returns dmabuf on success, ERR_PTR(-errno) on error.
 */
struct dma_heap_ops {
	struct dma_buf *(*allocate)(struct dma_heap *heap,
				    unsigned long len,
				    unsigned long fd_flags,
				    unsigned long heap_flags);
};

/**
 * struct dma_heap_export_info - information needed to export a new dmabuf heap
 * @name:	used for debugging/device-node name
 * @ops:	ops struct for this heap
 * @priv:	heap exporter private data
 *
 * Information needed to export a new dmabuf heap.
 */
struct dma_heap_export_info {
	const char *name;
	const struct dma_heap_ops *ops;
	void *priv;
};

void *dma_heap_get_drvdata(struct dma_heap *heap);

const char *dma_heap_get_name(struct dma_heap *heap);

struct dma_heap *dma_heap_add(const struct dma_heap_export_info *exp_info);

#ifdef CONFIG_DMABUF_HEAPS_CMA
/**
 * dma_heap_add_cma - adds a device CMA heap to dmabuf heaps
 * @dev:	device with a CMA heap to register
 */
int dma_heap_add_cma(struct device *dev);

#endif /* CONFIG_DMABUF_HEAPS_CMA */
void dma_heap_put(struct dma_heap *heap);

#endif /* _DMA_HEAPS_H */
