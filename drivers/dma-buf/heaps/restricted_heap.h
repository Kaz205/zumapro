/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Restricted heap Header.
 *
 * Copyright (C) 2024 MediaTek, Inc.
 */

#ifndef _DMABUF_RESTRICTED_HEAP_H_
#define _DMABUF_RESTRICTED_HEAP_H_

struct restricted_buffer {
	struct dma_heap		*heap;
	size_t			size;

	struct sg_table		sg_table;

	/* A reference to a buffer in the trusted or secure world. */
	u64			restricted_addr;
};

struct restricted_heap {
	const char		*name;

	const struct restricted_heap_ops *ops;

	struct cma		*cma;
	unsigned long		resv_paddr;
	unsigned long		resv_size;

	void			*priv_data;
};

struct restricted_heap_ops {
	int	(*heap_init)(struct restricted_heap *heap);

	int	(*memory_alloc)(struct restricted_heap *heap, struct restricted_buffer *buf);
	void	(*memory_free)(struct restricted_heap *heap, struct restricted_buffer *buf);

	int	(*memory_restrict)(struct restricted_heap *heap, struct restricted_buffer *buf);
	void	(*memory_unrestrict)(struct restricted_heap *heap, struct restricted_buffer *buf);

	struct sg_table	*(*map_dma_buf)(struct sg_table *table, struct restricted_buffer *buf, enum dma_data_direction direct);
	void	(*unmap_dma_buf)(struct sg_table *table, struct restricted_buffer *buf, enum dma_data_direction direct);
};

int restricted_heap_add(struct restricted_heap *rstrd_heap);

#endif
