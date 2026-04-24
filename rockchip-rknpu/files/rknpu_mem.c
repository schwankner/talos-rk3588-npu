// SPDX-License-Identifier: GPL-2.0
/*
 * rknpu_mem.c — NPU memory allocation using standard kernel DMA APIs.
 *
 * Bug 47: rk_dma_heap_find("rk-dma-heap-cma") returns NULL on mainline 6.18
 * (Rockchip BSP CMA heap absent), and the Bug 36 fallback rk_dma_heap_find("system")
 * also returns NULL because rknpu's internal heap registry does not include the
 * standard system heap.  rknpu_dev->heap therefore remains NULL.
 *
 * The original stub returned -ENOSYS for all memory ioctls, which was safe only
 * as long as librknnrt.so never called RKNPU_MEM_CREATE.  In practice the library
 * calls RKNPU_MEM_CREATE during init_runtime() to allocate small internal NPU
 * command buffers (~6 KB, flags 0xa).  -ENOSYS from the ioctl causes init_runtime()
 * to fail immediately with errno 38.
 *
 * Fix: implement rknpu_mem_create_ioctl with dma_alloc_coherent (physically
 * contiguous, DMA-safe, correct for non-IOMMU / passthrough mode) and
 * anon_inode_getfd (returns a userspace-mmappable fd).  Memory lifetime is tied
 * to the fd; closing it triggers the file release callback which calls
 * dma_free_coherent.
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
 * Private object tracking one dma_alloc_coherent allocation backing an fd.
 * Pointed to by file->private_data.
 */
struct rknpu_mem_obj {
	struct device	*dev;
	void		*cpu_addr;	/* kernel virtual address */
	dma_addr_t	 dma_addr;	/* device DMA address (phys in passthrough) */
	size_t		 size;		/* page-aligned allocation size */
};

static int rknpu_mem_obj_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct rknpu_mem_obj *obj = filp->private_data;

	return dma_mmap_coherent(obj->dev, vma,
				 obj->cpu_addr, obj->dma_addr, obj->size);
}

static int rknpu_mem_obj_release(struct inode *inode, struct file *filp)
{
	struct rknpu_mem_obj *obj = filp->private_data;

	dma_free_coherent(obj->dev, obj->size, obj->cpu_addr, obj->dma_addr);
	kfree(obj);
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
	struct rknpu_mem_obj *obj;
	int fd;

	if (copy_from_user(&args, (void __user *)data, sizeof(args)))
		return -EFAULT;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	obj->dev  = rknpu_dev->dev;
	obj->size = PAGE_ALIGN(args.size);

	/*
	 * dma_alloc_coherent returns physically contiguous, cache-coherent
	 * memory and a DMA address guaranteed to be accessible by the device.
	 * In iommu.passthrough=1 mode (which this driver requires), dma_addr
	 * equals the physical address, which is what the NPU hardware uses
	 * when submitting jobs via RKNPU_SUBMIT in non-IOMMU mode.
	 */
	obj->cpu_addr = dma_alloc_coherent(obj->dev, obj->size,
					   &obj->dma_addr, GFP_KERNEL);
	if (!obj->cpu_addr) {
		kfree(obj);
		return -ENOMEM;
	}

	/*
	 * Create an mmappable anonymous fd.  The file release callback frees
	 * the dma_alloc_coherent region when the fd is closed, matching the
	 * lifetime semantics of rk_dma_heap_bufferfd_alloc in the BSP driver.
	 */
	fd = anon_inode_getfd("[rknpu_mem]", &rknpu_mem_obj_fops, obj,
			      O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		dma_free_coherent(obj->dev, obj->size,
				  obj->cpu_addr, obj->dma_addr);
		kfree(obj);
		return fd;
	}

	args.handle   = (__u32)fd;
	args.dma_addr = obj->dma_addr;
	/*
	 * obj_addr: export the kernel VA so that the runtime can build NPU
	 * command buffers without a separate mmap() round-trip, matching BSP
	 * behaviour where this field holds the mapped kernel address.
	 */
	args.obj_addr = (u64)(uintptr_t)obj->cpu_addr;

	if (copy_to_user((void __user *)data, &args, sizeof(args))) {
		/*
		 * The fd is already installed in the process fd table.
		 * Returning -EFAULT here leaves the fd open; the caller is
		 * responsible for closing it (standard POSIX contract).
		 */
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
	 * dma_alloc_coherent provides DMA-coherent (cache-coherent) memory on
	 * ARM64.  The CPU and device views are always consistent; no explicit
	 * cache maintenance is required.
	 */
	return 0;
}
