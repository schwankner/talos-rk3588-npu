// SPDX-License-Identifier: GPL-2.0
/*
 * rknpu_mem.c — NPU memory allocation: small buffers via scatter-gather,
 *               large buffers via dma_alloc_noncontiguous + DMA mask widening.
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
 * Bug 55 (fix, rev 3): dma_map_sgtable() returns -ENOMEM (-12) for the
 * 2.4 GB model-weight buffer despite Rev 2's per-page allocation.  Root cause
 * (two interacting problems):
 *
 *   1. sg_alloc_table_from_pages() coalesces physically-contiguous pages.
 *      When alloc_page(GFP_KERNEL) is called 638,208 times on a freshly-booted
 *      system, the buddy allocator hands out pages in large physically-
 *      contiguous runs.  The resulting sg_table has nents=2 (two ~1.2 GB
 *      chunks) rather than 638,208 individual 4 KB entries.
 *
 *   2. dma_map_sgtable() maps each sg entry to its OWN, independent IOVA
 *      region.  For nents=2, the two regions are NOT adjacent in IOVA space.
 *      The NPU hardware accesses model weights using base_iova + sequential
 *      byte offsets; beyond the first sg entry it reads unmapped IOVA and the
 *      SMMU raises a fault (or returns zeros).  Additionally, with the default
 *      32-bit DMA mask (4 GB IOVA), allocating a 1.2 GB contiguous IOVA window
 *      alongside other active mappings fails with -ENOMEM.
 *
 * Fix (rev 3): two-step solution for large allocations (> RKNPU_MEM_LARGE_THR):
 *
 *   Step 1 — widen the DMA mask to 40-bit before the allocation.  This gives
 *   the IOVA allocator a 1 TB address space; a 2.4 GB contiguous window is
 *   trivially available.  The RK3588 SoC and its ARM SMMU-500 support 40-bit
 *   IOVAs (the physical address space is 40-bit).
 *
 *   Step 2 — use dma_alloc_noncontiguous().  Unlike raw dma_map_sgtable(),
 *   dma_alloc_noncontiguous() maps physically-scattered pages into a SINGLE
 *   contiguous IOVA range through the IOMMU page table.  The NPU hardware sees
 *   a flat [iova, iova + size) window regardless of physical layout.
 *   dma_vmap_noncontiguous() provides the contiguous kernel virtual address
 *   (kv_addr) for rknpu_job.c.
 *
 * Bug 55 (fix, rev 4): dma_alloc_noncontiguous() fails even after widening
 * dma_mask to 40-bit.  Root cause: dma_alloc_noncontiguous() uses
 * dev->coherent_dma_mask (not dma_mask) to compute the IOVA upper limit.
 * Rev 3 called dma_set_mask() which only updates *dev->dma_mask, leaving
 * coherent_dma_mask at 32-bit (0xffffffff).
 *
 * With coherent_dma_mask = 32-bit, __alloc_and_insert_iova_range() imposes a
 * size-alignment constraint on the IOVA start address:
 *   align = 2 ^ fls_long(size_pfn - 1)
 * For size = 638,208 PFN (2.4 GB), fls_long = 20, so the IOVA must be
 * 2^20-PFN = 4 GB aligned.  The only 4 GB-aligned address in a 4 GB IOVA
 * space is 0, which is reserved (start_pfn = 1).  The allocation fails with
 * -ENOMEM regardless of how much IOVA space is available.
 *
 * Fix (rev 4): call dma_set_mask_and_coherent() instead of dma_set_mask().
 * This also raises coherent_dma_mask to 40-bit, so the IOVA limit becomes
 * 1 TB.  Valid 4 GB-aligned IOVA slots appear at 4 GB, 8 GB, 12 GB … all
 * beyond the existing sub-4 GB mappings and freely available.  The
 * dma_alloc_noncontiguous() call then succeeds.
 *
 * Small allocations (≤ RKNPU_MEM_LARGE_THR) continue to use the Rev 2
 * alloc_page + dma_map_sgtable path; they produce nents=1 (one physically-
 * contiguous block) and work correctly.
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
 * Allocations above this threshold use dma_alloc_noncontiguous() to obtain a
 * contiguous IOVA range through the IOMMU (Bug 55 rev 3).  Below the threshold
 * the alloc_page + dma_map_sgtable path produces nents=1 and works fine.
 * 256 MiB sits well above the largest observed small allocation (~80 MiB) and
 * well below the failing 2.4 GiB model-weight buffer.
 */
#define RKNPU_MEM_LARGE_THR  (256UL << 20)

/*
 * rknpu_mem_buf — private per-allocation state.
 *
 * mem MUST be the first member: the submit path in rknpu_job.c does
 *   task_obj = (struct rknpu_mem_object *)(uintptr_t)task_obj_addr
 * where task_obj_addr == obj_addr returned by rknpu_mem_create_ioctl.
 * Placing mem first makes (struct rknpu_mem_object *)obj_addr == &buf->mem.
 *
 * use_noncontig selects the allocation path:
 *   true  — dma_alloc_noncontiguous(); teardown via dma_vunmap_noncontiguous()
 *            + dma_free_noncontiguous().  nc_sgt holds the sg_table pointer.
 *   false — alloc_page() per page + dma_map_sgtable(); teardown via vunmap()
 *           + dma_unmap_sgtable() + sg_free_table() + __free_page() × N.
 *           sgt, pages, nr_pages hold the scatter-gather state.
 */
struct rknpu_mem_buf {
	struct rknpu_mem_object  mem;		/* MUST be first — cast target in submit */
	struct device		*dev;		/* device for DMA ops */
	bool			 use_noncontig;	/* true: dma_alloc_noncontiguous path */
	/* noncontiguous path (large allocations) */
	struct sg_table		*nc_sgt;
	/* scatter-gather path (small allocations) */
	struct sg_table		 sgt;
	struct page		**pages;
	unsigned long		 nr_pages;
};

static int rknpu_mem_obj_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct rknpu_mem_buf *buf = filp->private_data;
	struct scatterlist *sg;
	unsigned long addr = vma->vm_start;
	int i;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	if (buf->use_noncontig) {
		/*
		 * Iterate the physical scatter entries (orig_nents) to remap
		 * physical pages into the VMA.  The DMA entries (nents) reflect
		 * the contiguous IOVA mapping and are not useful here.
		 */
		for_each_sg(buf->nc_sgt->sgl, sg, buf->nc_sgt->orig_nents, i) {
			unsigned long len = sg->length;

			if (remap_pfn_range(vma, addr,
					    page_to_pfn(sg_page(sg)),
					    len, vma->vm_page_prot))
				return -EAGAIN;
			addr += len;
			if (addr >= vma->vm_end)
				break;
		}
	} else {
		for_each_sg(buf->sgt.sgl, sg, buf->sgt.nents, i) {
			unsigned long len = sg->length;

			if (remap_pfn_range(vma, addr,
					    page_to_pfn(sg_page(sg)),
					    len, vma->vm_page_prot))
				return -EAGAIN;
			addr += len;
			if (addr >= vma->vm_end)
				break;
		}
	}
	return 0;
}

static int rknpu_mem_obj_release(struct inode *inode, struct file *filp)
{
	struct rknpu_mem_buf *buf = filp->private_data;
	unsigned long i;

	if (buf->use_noncontig) {
		dma_vunmap_noncontiguous(buf->dev, buf->mem.kv_addr);
		dma_free_noncontiguous(buf->dev, buf->mem.size,
				       buf->nc_sgt, DMA_BIDIRECTIONAL);
	} else {
		vunmap(buf->mem.kv_addr);
		dma_unmap_sgtable(buf->dev, &buf->sgt, DMA_BIDIRECTIONAL, 0);
		sg_free_table(&buf->sgt);
		for (i = 0; i < buf->nr_pages; i++)
			__free_page(buf->pages[i]);
		vfree(buf->pages);
	}
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
	unsigned long i = 0;
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

	if (buf->mem.size > RKNPU_MEM_LARGE_THR) {
		/*
		 * Bug 55 rev 4: large allocation path.
		 *
		 * Widen BOTH dma_mask and coherent_dma_mask to 40-bit using
		 * dma_set_mask_and_coherent().  dma_alloc_noncontiguous() uses
		 * coherent_dma_mask for the IOVA upper limit; the earlier
		 * dma_set_mask()-only approach (rev 3) left coherent_dma_mask at
		 * 32-bit, which imposed a 4 GB IOVA alignment requirement that
		 * has no valid slot in the 4 GB IOVA space.
		 *
		 * With both masks at 40-bit the IOVA limit is 1 TB; valid
		 * 4 GB-aligned slots appear at IOVA 4 GB, 8 GB, … well clear of
		 * existing sub-4 GB mappings.
		 */
		if (buf->dev->coherent_dma_mask < DMA_BIT_MASK(40)) {
			if (!dma_set_mask_and_coherent(buf->dev, DMA_BIT_MASK(40)))
				dev_info(buf->dev,
					 "rknpu_mem: DMA mask widened to 40-bit\n");
		}

		/*
		 * Step 2: allocate scattered physical pages and map them into a
		 * single contiguous IOVA range through the IOMMU page table.
		 * sg_dma_address(nc_sgt->sgl) is the contiguous IOVA base; the
		 * NPU hardware can access all model weights at [iova, iova+size).
		 */
		buf->use_noncontig = true;
		dev_info(buf->dev,
			 "rknpu_mem: noncontig alloc %zu bytes dma_mask=0x%llx\n",
			 buf->mem.size,
			 buf->dev->dma_mask ? *buf->dev->dma_mask : 0ULL);

		buf->nc_sgt = dma_alloc_noncontiguous(buf->dev, buf->mem.size,
						      DMA_BIDIRECTIONAL,
						      GFP_KERNEL, 0);
		if (!buf->nc_sgt) {
			dev_err(buf->dev,
				"rknpu_mem: dma_alloc_noncontiguous failed (size=%zu dma_mask=0x%llx)\n",
				buf->mem.size,
				buf->dev->dma_mask ? *buf->dev->dma_mask : 0ULL);
			kfree(buf);
			return -ENOMEM;
		}

		buf->mem.dma_addr = sg_dma_address(buf->nc_sgt->sgl);
		dev_info(buf->dev,
			 "rknpu_mem: noncontig mapped OK dma_addr=0x%llx size=%zu\n",
			 (u64)buf->mem.dma_addr, buf->mem.size);

		buf->mem.kv_addr = dma_vmap_noncontiguous(buf->dev,
							  buf->mem.size,
							  buf->nc_sgt);
		if (!buf->mem.kv_addr) {
			dev_err(buf->dev,
				"rknpu_mem: dma_vmap_noncontiguous failed (size=%zu)\n",
				buf->mem.size);
			dma_free_noncontiguous(buf->dev, buf->mem.size,
					       buf->nc_sgt, DMA_BIDIRECTIONAL);
			kfree(buf);
			return -ENOMEM;
		}

	} else {
		/*
		 * Small allocation path (≤ 256 MiB): allocate pages individually
		 * and map them through the IOMMU with dma_map_sgtable().  For
		 * small buffers the pages are typically physically contiguous
		 * (nents=1) so the IOVA mapping is a single window and the NPU
		 * hardware accesses the buffer correctly.
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

		ret = sg_alloc_table_from_pages(&buf->sgt, buf->pages,
						buf->nr_pages,
						0, buf->mem.size, GFP_KERNEL);
		if (ret) {
			dev_err(buf->dev,
				"rknpu_mem: sg_alloc_table_from_pages failed: %d\n",
				ret);
			goto err_free_pages;
		}

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

		buf->mem.kv_addr = vmap(buf->pages, buf->nr_pages,
					VM_MAP, PAGE_KERNEL);
		if (!buf->mem.kv_addr) {
			dev_err(buf->dev,
				"rknpu_mem: vmap failed (nr_pages=%lu)\n",
				buf->nr_pages);
			ret = -ENOMEM;
			goto err_unmap_sgt;
		}
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
	if (buf->use_noncontig) {
		dma_vunmap_noncontiguous(buf->dev, buf->mem.kv_addr);
		dma_free_noncontiguous(buf->dev, buf->mem.size,
				       buf->nc_sgt, DMA_BIDIRECTIONAL);
		kfree(buf);
		return ret;
	}
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
	 * SMMUv3); dma_map_sgtable() / dma_alloc_noncontiguous() handle
	 * CPU/device sync via the DMA subsystem.  No explicit sync required.
	 */
	return 0;
}
