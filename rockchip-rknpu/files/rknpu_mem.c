// SPDX-License-Identifier: GPL-2.0
/*
 * rknpu_mem.c — NPU memory allocation using the kernel DMA non-contiguous API.
 *
 * Bug 47 (initial): rk_dma_heap_find("rk-dma-heap-cma") returns NULL on
 * mainline 6.18 (Rockchip BSP CMA heap absent), and the Bug 36 fallback
 * rk_dma_heap_find("system") also returns NULL because rknpu's internal
 * heap registry does not include the standard system heap.  rknpu_dev->heap
 * therefore remains NULL, causing the stub to return -ENOSYS for all memory
 * ioctls.  librknnrt.so v2.3.x calls RKNPU_MEM_CREATE during init_runtime()
 * to allocate small internal NPU command buffers.
 *
 * Bug 47 (fix, rev 2): The original rev 1 returned obj_addr = cpu_addr (the
 * raw kernel virtual address of the DMA buffer).  The rknpu_job.c submit path
 * casts task_obj_addr as (struct rknpu_mem_object *) and reads ->kv_addr to
 * locate the task/command array.  With a raw cpu_addr as the struct pointer,
 * the kernel read garbage bytes from the beginning of the NPU command data as
 * if they were struct fields; the resulting garbage kv_addr was then used to
 * program the NPU hardware, causing the job to silently time out (3 retries ×
 * 60 s = 180 s) with no interrupt ever firing.
 *
 * Correct fix: embed a struct rknpu_mem_object (as defined in rknpu_mem.h)
 * inside a larger tracking struct.  Fill kv_addr with the kernel virtual
 * address returned by the allocator, and dma_addr with the device IOVA.
 * Return the address of the embedded rknpu_mem_object as obj_addr so the
 * submit path dereferences a valid, correctly-populated struct.
 *
 * Bug 55 (fix): dma_alloc_coherent() fails for large RKLLM model buffers
 * (>= ~1 GB) under IOMMU translated mode.  librknnrt.so calls RKNPU_MEM_CREATE
 * for each model weight tensor; the gemma2:2b model requires a single 2.4 GB
 * allocation that dma_alloc_coherent() cannot satisfy (ENOMEM returned even
 * with 26 GB free RAM) because dma_alloc_coherent requires either physically
 * contiguous memory or a compatible IOMMU remap path that may still fail for
 * large orders.
 *
 * Fix: switch to dma_alloc_noncontiguous() which allocates scatter pages and
 * maps them into a SINGLE CONTIGUOUS IOVA region via the IOMMU.  The NPU
 * hardware sees a contiguous DMA address range (IOVA) regardless of how the
 * physical pages are laid out.  dma_vmap_noncontiguous() provides the
 * contiguous kernel virtual address (kv_addr) that rknpu_job.c needs.
 *
 * Requires: IOMMU translated mode (iommu.passthrough=1 must NOT be set).
 * With IOMMU passthrough, dma_alloc_noncontiguous() falls back to a
 * physically-contiguous path which would again fail for 2.4 GB.
 *
 * struct rknpu_mem_buf wraps rknpu_mem_object with the fields needed for
 * DMA teardown.  rknpu_mem_object MUST remain the first member so that
 * (struct rknpu_mem_object *)obj_addr is the same address as
 * (struct rknpu_mem_buf *)obj_addr.
 */

#include <linux/anon_inodes.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "include/rknpu_drv.h"
#include "include/rknpu_mem.h"

/*
 * rknpu_mem_buf — private per-allocation state.
 *
 * mem MUST be the first member: the submit path in rknpu_job.c does
 *   task_obj = (struct rknpu_mem_object *)(uintptr_t)task_obj_addr
 * where task_obj_addr == obj_addr returned by rknpu_mem_create_ioctl.
 * Placing mem first makes (struct rknpu_mem_object *)obj_addr == &buf->mem.
 */
struct rknpu_mem_buf {
	struct rknpu_mem_object  mem;	/* MUST be first — cast target in submit */
	struct device		*dev;	/* device for DMA ops */
	struct sg_table		*sgt;	/* scatter-gather table (IOMMU-mapped) */
};

static int rknpu_mem_obj_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct rknpu_mem_buf *buf = filp->private_data;

	return dma_mmap_noncontiguous(buf->dev, vma, buf->mem.size, buf->sgt);
}

static int rknpu_mem_obj_release(struct inode *inode, struct file *filp)
{
	struct rknpu_mem_buf *buf = filp->private_data;

	dma_vunmap_noncontiguous(buf->dev, buf->mem.kv_addr);
	dma_free_noncontiguous(buf->dev, buf->mem.size, buf->sgt,
			       DMA_BIDIRECTIONAL);
	kfree(buf);
	return 0;
}

static const struct file_operations rknpu_mem_obj_fops = {
	.owner		= THIS_MODULE,
	.mmap		= rknpu_mem_obj_mmap,
	.release	= rknpu_mem_obj_release,
};

int rknpu_mem_create_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			   unsigned int cmd, unsigned long data)
{
	struct rknpu_mem_create args;
	struct rknpu_mem_buf *buf;
	int fd;

	if (copy_from_user(&args, (void __user *)data, sizeof(args)))
		return -EFAULT;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf->dev	 = rknpu_dev->dev;
	buf->mem.flags	 = args.flags;
	buf->mem.size	 = PAGE_ALIGN(args.size);

	/*
	 * Bug 55: use dma_alloc_noncontiguous() to allocate scatter pages that
	 * are mapped into a single contiguous IOVA region by the IOMMU.  This
	 * succeeds for arbitrarily large buffers (e.g. 2.4 GB model weights)
	 * where dma_alloc_coherent() fails due to physically-contiguous memory
	 * requirements.
	 *
	 * dma_addr (set below) is sg_dma_address(sgt->sgl): the IOVA base of
	 * the contiguous IOMMU mapping, which is what the NPU hardware uses.
	 *
	 * kv_addr comes from dma_vmap_noncontiguous(): a contiguous kernel
	 * virtual address range for the scatter pages, needed by rknpu_job.c
	 * to read the task/command array inside the buffer.
	 */
	buf->sgt = dma_alloc_noncontiguous(buf->dev, buf->mem.size,
					   DMA_BIDIRECTIONAL, GFP_KERNEL, 0);
	if (!buf->sgt) {
		kfree(buf);
		return -ENOMEM;
	}

	buf->mem.kv_addr = dma_vmap_noncontiguous(buf->dev, buf->mem.size,
						  buf->sgt);
	if (!buf->mem.kv_addr) {
		dma_free_noncontiguous(buf->dev, buf->mem.size, buf->sgt,
				       DMA_BIDIRECTIONAL);
		kfree(buf);
		return -ENOMEM;
	}

	/*
	 * IOVA base address for the NPU hardware.  dma_alloc_noncontiguous()
	 * maps all scatter pages into a single contiguous IOVA window via the
	 * IOMMU, so sg_dma_address(sgt->sgl) is the start of that window.
	 */
	buf->mem.dma_addr = sg_dma_address(buf->sgt->sgl);

	/*
	 * Create a userspace-mmappable fd.  Memory freed via release callback
	 * when the fd is closed, matching the lifetime semantics of
	 * rk_dma_heap_bufferfd_alloc in the BSP driver.
	 */
	fd = anon_inode_getfd("[rknpu_mem]", &rknpu_mem_obj_fops, buf,
			      O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		dma_vunmap_noncontiguous(buf->dev, buf->mem.kv_addr);
		dma_free_noncontiguous(buf->dev, buf->mem.size, buf->sgt,
				       DMA_BIDIRECTIONAL);
		kfree(buf);
		return fd;
	}

	args.handle   = (__u32)fd;
	args.dma_addr = buf->mem.dma_addr;
	/*
	 * obj_addr: pointer to the embedded rknpu_mem_object struct.
	 *
	 * rknpu_job.c:rknpu_job_subcore_commit_pc() casts task_obj_addr as
	 *   (struct rknpu_mem_object *)(uintptr_t)task_obj_addr
	 * and then reads ->kv_addr to locate the command/task array in the
	 * buffer.  We must return the address of &buf->mem (not cpu_addr)
	 * so the submit path dereferences a valid, populated struct rather
	 * than treating random NPU command bytes as struct fields.
	 */
	args.obj_addr = (u64)(uintptr_t)&buf->mem;

	if (copy_to_user((void __user *)data, &args, sizeof(args))) {
		/* fd already installed; caller must close it to free memory */
		return -EFAULT;
	}

	return 0;
}

int rknpu_mem_destroy_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			    unsigned long data)
{
	/*
	 * Memory lifetime is tied to the fd returned by rknpu_mem_create_ioctl.
	 * Closing the fd triggers rknpu_mem_obj_release → DMA teardown.
	 * No explicit destroy action is needed here.
	 */
	return 0;
}

int rknpu_mem_sync_ioctl(struct rknpu_device *rknpu_dev, unsigned long data)
{
	/*
	 * dma_alloc_noncontiguous() allocates coherent memory on ARM64 via
	 * SMMUv3; cache maintenance is handled by the DMA subsystem.
	 * No explicit sync is required here.
	 */
	return 0;
}
