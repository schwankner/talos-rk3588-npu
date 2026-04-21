/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for linux/rk-dma-heap.h — Rockchip BSP-only API, absent from mainline.
 *
 * Used when building rknpu out-of-tree against a mainline 6.x kernel with
 * CONFIG_ROCKCHIP_RKNPU_DMA_HEAP defined.  All functions return safe error
 * values; the driver's DMA_HEAP memory allocation path fails gracefully when
 * rk_dma_heap_find() returns NULL.  The /dev/rknpu misc device is still
 * registered regardless of heap availability.
 *
 * Placed in src/include/compat/linux/ so the existing
 * -I$(src)/src/include/compat ccflag picks it up before the kernel tree.
 */
#ifndef _LINUX_RK_DMA_HEAP_H
#define _LINUX_RK_DMA_HEAP_H

#include <linux/dma-buf.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/err.h>

struct rk_dma_heap;

static inline struct rk_dma_heap *rk_dma_heap_find(const char *name)
{
	return NULL;
}

static inline int rk_dma_heap_set_dev(struct device *heap_dev)
{
	return -ENODEV;
}

static inline struct dma_buf *
rk_dma_heap_buffer_alloc(struct rk_dma_heap *heap, size_t len,
			  unsigned int fd_flags, unsigned int heap_flags,
			  const char *name)
{
	return ERR_PTR(-ENODEV);
}

static inline void rk_dma_heap_buffer_free(struct dma_buf *dmabuf)
{
}

static inline int
rk_dma_heap_bufferfd_alloc(struct rk_dma_heap *heap, size_t len,
			    unsigned int fd_flags, unsigned int heap_flags,
			    const char *name)
{
	return -ENODEV;
}

static inline int
rk_dma_heap_alloc_contig_pages(struct rk_dma_heap *heap, size_t len,
				unsigned int heap_flags, struct page **pages)
{
	return -ENODEV;
}

static inline void
rk_dma_heap_free_contig_pages(struct page **pages, size_t len)
{
}

static inline int rk_dma_heap_cma_setup(void)
{
	return 0;
}

#endif /* _LINUX_RK_DMA_HEAP_H */
