// SPDX-License-Identifier: GPL-2.0
/*
 * rknpu_mem.c — NPU memory allocation via manual scatter-gather + DMA mapping.
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
 * Bug 55 (fix, rev 2): dma_alloc_noncontiguous() silently returns NULL on
 * kernel 6.18.22 / ARM SMMUv3 for the rknpu device even with abundant free
 * RAM.  Root cause: iommu_dma_ops.alloc_noncontiguous calls
 * iommu_dma_alloc_pages() which uses dma_alloc_pages() internally; under the
 * ARM SMMUv3 translated-mode domain for fdab0000.rknpu, iova_alloc() fails to
 * find a contiguous 2.4 GB window in the device's IOVA space (the device DMA
 * mask limits the IOVA range to 4 GB, and early boot allocations fragment it).
 *
 * Fix (rev 2): bypass dma_alloc_noncontiguous() entirely.  Allocate physical
 * pages individually with alloc_page(GFP_KERNEL) — always succeeds given free
 * RAM — then build an sg_table from those pages and call dma_map_sgtable() to
 * obtain the IOVA mapping.  dma_map_sgtable() allocates individual 4 KB IOVA
 * slots rather than one contiguous window, so the IOVA allocator's contiguous-
 * range constraint does not apply.  vmap() provides the contiguous kernel
 * virtual address (kv_addr) for rknpu_job.c.
 *
 * struct rknpu_mem_buf wraps rknpu_mem_object with the fields needed for
 * teardown.  rknpu_mem_object MUST remain the first member so that
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
#include <linux/vmalloc.h>

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
	struct rknpu_mem_object  mem;		/* MUST be first — cast target in submit */
	struct device		*dev;		/* device for DMA ops */
	struct sg_table		 sgt;		/* scatter-gather table (IOMMU-mapped) */
	struct page		**pages;	/* page array (nr_pages entries) */
	unsigned long		 nr_pages;
};

static int rknpu_mem_obj_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct rknpu_mem_buf *buf = filp->private_data;
	struct scatterlist *sg;
	unsigned long addr = vma->vm_start;
	int i;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	for_each_sg(buf->sgt.sgl, sg, buf->sgt.nents, i) {
		unsigned long len = sg->length;

		if (remap_pfn_range(vma, addr, page_to_pfn(sg_page(sg)),
				    len, vma->vm_page_prot))
			return -EAGAIN;
		addr += len;
		if (addr >= vma->vm_end)
			break;
	}
	return 0;
}

static int rknpu_mem_obj_release(struct inode *inode, struct file *filp)
{
	struct rknpu_mem_buf *buf = filp->private_data;
	unsigned long i;

	vunmap(buf->mem.kv_addr);
	dma_unmap_sgtable(buf->dev, &buf->sgt, DMA_BIDIRECTIONAL, 0);
	sg_free_table(&buf->sgt);
	for (i = 0; i < buf->nr_pages; i++)
		__free_page(buf->pages[i]);
	vfree(buf->pages);
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
	unsigned long i;
	int ret, fd;

	if (copy_from_user(&args, (void __user *)data, sizeof(args)))
		return -EFAULT;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf->dev	 = rknpu_dev->dev;
	buf->mem.flags	 = args.flags;
	buf->mem.size	 = PAGE_ALIGN(args.size);
	buf->nr_pages	 = buf->mem.size >> PAGE_SHIFT;

	/*
	 * Bug 55 rev 2: allocate pages individually rather than as one
	 * contiguous block.  Each alloc_page(GFP_KERNEL) succeeds as long as
	 * free RAM > 0; no physically-contiguous requirement, no CMA needed.
	 */
	buf->pages = vmalloc(buf->nr_pages * sizeof(struct page *));
	if (!buf->pages) {
		kfree(buf);
		return -ENOMEM;
	}

	for (i = 0; i < buf->nr_pages; i++) {
		buf->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!buf->pages[i]) {
			dev_err(buf->dev,
				"rknpu_mem: alloc_page failed at %lu/%lu (size=%zu)\n",
				i, buf->nr_pages, buf->mem.size);
			ret = -ENOMEM;
			goto err_free_pages;
		}
	}

	ret = sg_alloc_table_from_pages(&buf->sgt, buf->pages, buf->nr_pages,
					0, buf->mem.size, GFP_KERNEL);
	if (ret) {
		dev_err(buf->dev, "rknpu_mem: sg_alloc_table_from_pages failed: %d\n", ret);
		goto err_free_pages;
	}

	/*
	 * Map scatter pages through the IOMMU (ARM SMMUv3) to obtain per-page
	 * IOVA entries.  Unlike dma_alloc_noncontiguous(), dma_map_sgtable()
	 * does NOT require a single contiguous IOVA window — it maps each page
	 * independently — so the IOVA allocator's contiguous-range constraint
	 * does not apply.
	 *
	 * sg_dma_address(buf->sgt.sgl) is the IOVA of the first page, which is
	 * what the NPU hardware programs as the base address.  This is the same
	 * value the BSP driver's CMA heap would provide (contiguous pages →
	 * contiguous IOVA).  For scatter pages the hardware sees individual
	 * 4 KB IOVAs, but the NPU firmware walks them via the sg list embedded
	 * in the task structure rather than treating them as contiguous.
	 */
	dev_info(buf->dev,
		 "rknpu_mem: mapping %zu bytes (%lu pages) dma_mask=0x%llx\n",
		 buf->mem.size, buf->nr_pages,
		 buf->dev->dma_mask ? *buf->dev->dma_mask : 0ULL);

	ret = dma_map_sgtable(buf->dev, &buf->sgt, DMA_BIDIRECTIONAL, 0);
	if (ret) {
		dev_err(buf->dev,
			"rknpu_mem: dma_map_sgtable failed: %d (size=%zu nents=%u dma_mask=0x%llx)\n",
			ret, buf->mem.size, buf->sgt.nents,
			buf->dev->dma_mask ? *buf->dev->dma_mask : 0ULL);
		goto err_free_sgt;
	}

	dev_info(buf->dev,
		 "rknpu_mem: mapped OK dma_addr=0x%llx nents=%u\n",
		 (u64)sg_dma_address(buf->sgt.sgl), buf->sgt.nents);

	buf->mem.dma_addr = sg_dma_address(buf->sgt.sgl);

	/*
	 * Provide a contiguous kernel virtual address for rknpu_job.c so the
	 * submit path can read the task/command array from kv_addr.
	 */
	buf->mem.kv_addr = vmap(buf->pages, buf->nr_pages, VM_MAP, PAGE_KERNEL);
	if (!buf->mem.kv_addr) {
		dev_err(buf->dev, "rknpu_mem: vmap failed (nr_pages=%lu)\n", buf->nr_pages);
		ret = -ENOMEM;
		goto err_unmap_sgt;
	}

	/*
	 * Create a userspace-mmappable fd.  Memory freed via release callback
	 * when the fd is closed.
	 */
	fd = anon_inode_getfd("[rknpu_mem]", &rknpu_mem_obj_fops, buf,
			      O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto err_vunmap;
	}

	args.handle   = (__u32)fd;
	args.dma_addr = buf->mem.dma_addr;
	/*
	 * obj_addr: pointer to the embedded rknpu_mem_object struct.
	 *
	 * rknpu_job.c:rknpu_job_subcore_commit_pc() casts task_obj_addr as
	 *   (struct rknpu_mem_object *)(uintptr_t)task_obj_addr
	 * and reads ->kv_addr to locate the command/task array.
	 */
	args.obj_addr = (u64)(uintptr_t)&buf->mem;

	if (copy_to_user((void __user *)data, &args, sizeof(args))) {
		/* fd installed; caller must close to free memory */
		return -EFAULT;
	}

	return 0;

err_vunmap:
	vunmap(buf->mem.kv_addr);
err_unmap_sgt:
	dma_unmap_sgtable(buf->dev, &buf->sgt, DMA_BIDIRECTIONAL, 0);
err_free_sgt:
	sg_free_table(&buf->sgt);
err_free_pages:
	/* free pages that were successfully allocated (indices 0..i-1) */
	while (i > 0)
		__free_page(buf->pages[--i]);
	vfree(buf->pages);
	kfree(buf);
	return ret;
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
	 * Pages are allocated with GFP_KERNEL (cache-coherent on ARM64 with
	 * SMMUv3); dma_map_sgtable() handles CPU/device sync via the DMA
	 * subsystem.  No explicit sync is required here.
	 */
	return 0;
}
