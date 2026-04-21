// SPDX-License-Identifier: GPL-2.0
/*
 * rknpu_mem.c — stub implementations of the DMA_HEAP memory-management ioctls.
 *
 * The w568w/rknpu-module DMA_HEAP code path declares rknpu_mem_create_ioctl,
 * rknpu_mem_destroy_ioctl, and rknpu_mem_sync_ioctl in rknpu_mem.h and calls
 * them from rknpu_misc_ioctl, but provides no definitions.  This file supplies
 * no-op stubs so the module links cleanly.
 *
 * librknnrt.so v2.3.x allocates inference buffers through /dev/dma_heap/system
 * via the standard Linux dma-heap userspace API and uses /dev/rknpu only for
 * RKNPU_SUBMIT.  None of RKNPU_MEM_CREATE / RKNPU_MEM_DESTROY / RKNPU_MEM_SYNC
 * are issued by the library, so returning -ENOSYS here is safe.
 */

#include <linux/errno.h>
#include <linux/fs.h>

#include "include/rknpu_drv.h"
#include "include/rknpu_mem.h"

int rknpu_mem_create_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			   unsigned int cmd, unsigned long data)
{
	return -ENOSYS;
}

int rknpu_mem_destroy_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			    unsigned long data)
{
	return -ENOSYS;
}

int rknpu_mem_sync_ioctl(struct rknpu_device *rknpu_dev, unsigned long data)
{
	return -ENOSYS;
}
