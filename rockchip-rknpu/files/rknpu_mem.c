// SPDX-License-Identifier: GPL-2.0
/*
 * rknpu_mem.c — NPU memory allocation using standard kernel DMA APIs.
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
 * address returned by dma_alloc_coherent, and dma_addr with the device DMA
 * address.  Return the address of the embedded rknpu_mem_object as obj_addr
 * so the submit path dereferences a valid, correctly-populated struct.
 *
 * struct rknpu_mem_buf wraps rknpu_mem_object with the device pointer needed
 * for dma_mmap_coherent in the file mmap callback.  rknpu_mem_object MUST
 * remain the first member so that (struct rknpu_mem_object *)obj_addr is the
 * same address as (struct rknpu_mem_buf *)obj_addr.
 */

#include <linux/anon_inodes.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mm.h>
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
	struct rknpu_mem_object mem;	/* MUST be first — cast target in submit */
	struct device		*dev;	/* device for dma_mmap_coherent / free */
};

static int rknpu_mem_obj_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct rknpu_mem_buf *buf = filp->private_data;

	return dma_mmap_coherent(buf->dev, vma,
				 buf->mem.kv_addr,
				 buf->mem.dma_addr,
				 buf->mem.size);
}

static int rknpu_mem_obj_release(struct inode *inode, struct file *filp)
{
	struct rknpu_mem_buf *buf = filp->private_data;

	dma_free_coherent(buf->dev, buf->mem.size,
			  buf->mem.kv_addr, buf->mem.dma_addr);
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

	buf->dev	  = rknpu_dev->dev;
	buf->mem.flags	  = args.flags;
	buf->mem.size	  = PAGE_ALIGN(args.size);

	/*
	 * dma_alloc_coherent returns physically contiguous, cache-coherent
	 * memory.  In iommu.passthrough=1 mode dma_addr equals the physical
	 * address, which is what the NPU hardware uses for DMA in non-IOMMU
	 * mode.  kv_addr is the kernel virtual address; rknpu_job.c reads it
	 * to locate the task/command array inside the buffer.
	 */
	buf->mem.kv_addr = dma_alloc_coherent(buf->dev, buf->mem.size,
					      &buf->mem.dma_addr, GFP_KERNEL);
	if (!buf->mem.kv_addr) {
		kfree(buf);
		return -ENOMEM;
	}

	/*
	 * Create a userspace-mmappable fd.  Memory freed via release callback
	 * when the fd is closed, matching the lifetime semantics of
	 * rk_dma_heap_bufferfd_alloc in the BSP driver.
	 */
	fd = anon_inode_getfd("[rknpu_mem]", &rknpu_mem_obj_fops, buf,
			      O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		dma_free_coherent(buf->dev, buf->mem.size,
				  buf->mem.kv_addr, buf->mem.dma_addr);
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
	 * Closing the fd triggers rknpu_mem_obj_release → dma_free_coherent.
	 * No explicit destroy action is needed here.
	 */
	return 0;
}

int rknpu_mem_sync_ioctl(struct rknpu_device *rknpu_dev, unsigned long data)
{
	/*
	 * dma_alloc_coherent provides DMA-coherent memory on ARM64; no explicit
	 * cache maintenance is required.
	 */
	return 0;
}
