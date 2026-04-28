// SPDX-License-Identifier: GPL-2.0
/*
 * rknpu_mem.c — NPU memory allocation: small buffers via scatter-gather,
 *               large buffers via direct iommu_map() at a manually-managed
 *               IOVA cursor.
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
 * kernel 6.18.22 / Rockchip IOMMU for the rknpu device even with abundant
 * free RAM.  Root cause: iommu_dma_ops.alloc_noncontiguous calls
 * iommu_dma_alloc_pages() which uses dma_alloc_pages() internally; under the
 * Rockchip IOMMU translated-mode domain for fdab0000.rknpu, iova_alloc()
 * fails to find a contiguous 2.4 GB window in the device's IOVA space (the
 * device DMA mask limits the IOVA range to 4 GB, and early boot allocations
 * fragment it).
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
 *      IOMMU raises a fault.  Additionally, with the default 32-bit DMA mask
 *      (4 GB IOVA), allocating a 1.2 GB contiguous IOVA window alongside other
 *      active mappings fails with -ENOMEM.
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
 * Fix (rev 4): call dma_set_mask_and_coherent() instead of dma_set_mask().
 *
 * Bug 55 (fix, rev 5): dma_alloc_noncontiguous() still fails even with both
 * masks at 40-bit.  Root cause: the IOVA domain is not resized dynamically
 * when the DMA mask is changed at runtime.  The IOVA domain (iova_domain
 * inside the iommu_dma_cookie) is initialized on the FIRST DMA operation
 * (iommu_dma_init_domain → init_iova_domain), with end_pfn derived from
 * the DMA mask in effect AT THAT MOMENT.  Revs 3 and 4 widened the mask only
 * in the large-buffer branch, which runs AFTER the small buffer allocations
 * (19 MiB and 83 MiB).  Those small allocations happened first with a 32-bit
 * mask, locking the IOVA domain to [0, 4 GB).  Widening the mask later has
 * no effect because the domain is already initialized.
 *
 * Fix (rev 5): widen the DMA mask to 40-bit at the TOP of
 * rknpu_mem_create_ioctl(), unconditionally, before any size check or DMA
 * operation.  The first call now initialises the IOVA domain with a 40-bit
 * limit.
 *
 * Bug 55 (fix, rev 6): dma_alloc_noncontiguous() STILL fails despite Rev 5's
 * ordering fix.  Root cause (finally confirmed): the rknpu device uses the
 * Rockchip IOMMU (rk3568-iommu, fdab9000.rknpu_mmu) which has a HARDWARE
 * LIMIT of 32-bit IOVA (4 GB).  Widening the DMA mask to 40-bit has no effect
 * on the Rockchip IOMMU's page-table address space.
 *
 * Additionally, dma_alloc_noncontiguous() uses size_aligned=true in
 * alloc_iova_fast(), which forces the 2.4 GB buffer to be aligned to
 *   2^fls_long(638208-1) = 2^20 PFN = 4 GB.
 * Inside a 4 GB IOVA domain the only 4 GB-aligned address is 0x0, which is
 * reserved (start_pfn=1).  The allocation fails regardless of DMA mask.
 *
 * Fix (rev 6): bypass the DMA IOVA allocator entirely for large allocations.
 *
 *   1. Allocate physical pages individually with alloc_page(GFP_KERNEL).
 *
 *   2. Assign an IOVA from a module-level cursor (rknpu_iova_cursor) that
 *      starts at RKNPU_IOVA_BASE (1 MB) and grows upward in steps of the
 *      allocation size (rounded up to RKNPU_IOVA_ALIGN, 1 GB).  The DMA IOVA
 *      allocator works top-down from near 4 GB; our cursor grows bottom-up;
 *      they do not overlap for the typical use-case of a few large buffers.
 *
 *   3. Map each page at cursor_iova + i*PAGE_SIZE via iommu_map().  Unlike
 *      dma_alloc_noncontiguous(), iommu_map() writes page-table entries
 *      directly with no alignment constraint beyond PAGE_SIZE.  The NPU
 *      hardware sees a flat contiguous IOVA window.
 *
 *   4. vmap() the physical pages to provide a contiguous kernel virtual
 *      address (kv_addr) for rknpu_job.c.
 *
 * struct rknpu_mem_buf wraps rknpu_mem_object with the fields needed for
 * teardown.  rknpu_mem_object MUST remain the first member so that
 * (struct rknpu_mem_object *)obj_addr is the same address as
 * (struct rknpu_mem_buf *)obj_addr.
 */

#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "include/rknpu_drv.h"
#include "include/rknpu_mem.h"

/*
 * Allocations above this threshold use the direct iommu_map() path to obtain
 * a contiguous IOVA range without the size-alignment constraint imposed by
 * the DMA IOVA allocator (Bug 55 rev 6).  Below the threshold the
 * alloc_page + dma_map_sgtable path produces nents=1 and works fine.
 * 256 MiB sits well above the largest observed small allocation (~80 MiB) and
 * well below the failing 2.4 GiB model-weight buffer.
 */
#define RKNPU_MEM_LARGE_THR  (256UL << 20)

/*
 * Manual IOVA allocator for large buffers (Bug 55 rev 6).
 *
 * RKNPU_IOVA_BASE: starting IOVA for large allocations.  1 MB keeps us well
 * above the IOMMU's reserved zero page while leaving room below the DMA
 * allocator's top-down allocations (first small alloc lands near 0xfe000000).
 *
 * RKNPU_IOVA_ALIGN: stride between consecutive large allocations.  Rounding
 * each allocation up to 1 GB boundaries avoids fragmentation in the IOVA
 * space and keeps the cursor advancing in predictable steps.
 *
 * With 3 GB of usable IOVA below the first small allocation (~0xf8000000),
 * a single 2.4 GB allocation fits comfortably starting at 1 MB.
 */
#define RKNPU_IOVA_BASE   (1UL << 20)            /* 1 MB */
#define RKNPU_IOVA_ALIGN  (1UL << 30)            /* 1 GB stride */

static atomic64_t rknpu_iova_cursor = ATOMIC64_INIT(RKNPU_IOVA_BASE);

/*
 * rknpu_mem_buf — private per-allocation state.
 *
 * mem MUST be the first member: the submit path in rknpu_job.c does
 *   task_obj = (struct rknpu_mem_object *)(uintptr_t)task_obj_addr
 * where task_obj_addr == obj_addr returned by rknpu_mem_create_ioctl.
 * Placing mem first makes (struct rknpu_mem_object *)obj_addr == &buf->mem.
 *
 * use_iommu_map selects the allocation path for large buffers:
 *   true  — iommu_map() path (large allocations, Bug 55 rev 6).
 *            Teardown: iommu_unmap() + vunmap() + __free_page() × N.
 *            iommu_iova holds the manually-assigned IOVA base.
 *   false — alloc_page() per page + dma_map_sgtable() path (small buffers).
 *           Teardown: vunmap() + dma_unmap_sgtable() + sg_free_table() +
 *           __free_page() × N.
 *
 * In both paths, pages[] and nr_pages track the physical pages.
 */
struct rknpu_mem_buf {
	struct rknpu_mem_object  mem;		/* MUST be first — cast target in submit */
	struct device		*dev;		/* device for DMA ops */
	bool			 use_iommu_map;	/* true: direct iommu_map large path */
	unsigned long		 iommu_iova;	/* IOVA base (iommu_map path only) */
	struct sg_table		 sgt;		/* scatter-gather (small path only) */
	struct page		**pages;	/* physical pages (both paths) */
	unsigned long		 nr_pages;	/* number of pages (both paths) */
};

static int rknpu_mem_obj_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct rknpu_mem_buf *buf = filp->private_data;
	struct page **pages = buf->pages;
	unsigned long addr = vma->vm_start;
	unsigned long i;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	for (i = 0; i < buf->nr_pages && addr < vma->vm_end; i++) {
		if (remap_pfn_range(vma, addr, page_to_pfn(pages[i]),
				    PAGE_SIZE, vma->vm_page_prot))
			return -EAGAIN;
		addr += PAGE_SIZE;
	}
	return 0;
}

static int rknpu_mem_obj_release(struct inode *inode, struct file *filp)
{
	struct rknpu_mem_buf *buf = filp->private_data;
	unsigned long i;

	if (buf->use_iommu_map) {
		struct iommu_domain *domain = iommu_get_domain_for_dev(buf->dev);

		vunmap(buf->mem.kv_addr);
		if (domain)
			iommu_unmap(domain, buf->iommu_iova, buf->mem.size);
	} else {
		vunmap(buf->mem.kv_addr);
		dma_unmap_sgtable(buf->dev, &buf->sgt, DMA_BIDIRECTIONAL, 0);
		sg_free_table(&buf->sgt);
	}

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
	unsigned long i = 0;
	int ret, fd;

	if (copy_from_user(&args, (void __user *)data, sizeof(args)))
		return -EFAULT;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf->dev      = rknpu_dev->dev;
	buf->mem.flags = args.flags;
	buf->mem.size  = PAGE_ALIGN(args.size);
	buf->nr_pages  = buf->mem.size >> PAGE_SHIFT;

	/* Allocate physical pages (both paths). */
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

	if (buf->mem.size > RKNPU_MEM_LARGE_THR) {
		/*
		 * Bug 55 rev 6: large allocation path.
		 *
		 * The Rockchip IOMMU has a 32-bit (4 GB) IOVA space.
		 * dma_alloc_noncontiguous() requires 4 GB alignment for a
		 * 2.4 GB buffer (size_aligned=true constraint), which is
		 * impossible in a 4 GB domain (only valid 4 GB-aligned address
		 * is 0x0 = reserved).
		 *
		 * Instead: assign a manually-managed IOVA from the bottom of
		 * the address space (rknpu_iova_cursor, starting at 1 MB) and
		 * map each physical page directly with iommu_map().  iommu_map()
		 * has no alignment constraint beyond PAGE_SIZE.
		 *
		 * The DMA IOVA allocator works top-down (small allocs land near
		 * 0xfe000000); our cursor grows bottom-up from 1 MB.  They do
		 * not overlap for the typical use-case of one or two large
		 * model-weight buffers.
		 */
		struct iommu_domain *domain;
		unsigned long iova, mapped = 0;
		unsigned long stride;

		domain = iommu_get_domain_for_dev(buf->dev);
		if (!domain) {
			dev_err(buf->dev,
				"rknpu_mem: iommu_get_domain_for_dev failed\n");
			ret = -ENODEV;
			goto err_free_pages;
		}

		stride = ALIGN(buf->mem.size, RKNPU_IOVA_ALIGN);
		iova = (unsigned long)atomic64_fetch_add((s64)stride,
							 &rknpu_iova_cursor);
		buf->iommu_iova   = iova;
		buf->use_iommu_map = true;

		dev_info(buf->dev,
			 "rknpu_mem: iommu_map alloc %zu bytes iova=0x%lx (%lu pages)\n",
			 buf->mem.size, iova, buf->nr_pages);

		/* Map pages into the IOMMU page table, coalescing contiguous
		 * physical runs to reduce iommu_map() call count. */
		while (mapped < buf->nr_pages) {
			unsigned long run = mapped;
			phys_addr_t base = page_to_phys(buf->pages[mapped]);
			size_t run_size;

			/* Extend run while physically contiguous. */
			while (mapped + 1 < buf->nr_pages &&
			       page_to_phys(buf->pages[mapped + 1]) ==
			       page_to_phys(buf->pages[mapped]) + PAGE_SIZE)
				mapped++;

			run_size = (mapped - run + 1) * PAGE_SIZE;
			ret = iommu_map(domain,
					iova + (run << PAGE_SHIFT),
					base, run_size,
					IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE,
					GFP_KERNEL);
			if (ret) {
				dev_err(buf->dev,
					"rknpu_mem: iommu_map failed at iova=0x%lx size=%zu: %d\n",
					iova + (run << PAGE_SHIFT), run_size,
					ret);
				/* Unmap what was already mapped. */
				if (run > 0)
					iommu_unmap(domain, iova,
						    run << PAGE_SHIFT);
				goto err_free_pages;
			}
			mapped++;
		}

		dev_info(buf->dev,
			 "rknpu_mem: iommu_map OK iova=0x%lx size=%zu\n",
			 iova, buf->mem.size);

		buf->mem.dma_addr = (dma_addr_t)iova;

		buf->mem.kv_addr = vmap(buf->pages, buf->nr_pages,
					VM_MAP, PAGE_KERNEL);
		if (!buf->mem.kv_addr) {
			dev_err(buf->dev,
				"rknpu_mem: vmap failed (nr_pages=%lu)\n",
				buf->nr_pages);
			iommu_unmap(domain, iova, buf->mem.size);
			ret = -ENOMEM;
			goto err_free_pages;
		}

	} else {
		/*
		 * Small allocation path (≤ 256 MiB): build sg_table from pages
		 * and map through the DMA subsystem.  For small buffers the
		 * buddy allocator typically returns physically-contiguous pages
		 * (nents=1), so the IOVA mapping is a single window and the NPU
		 * hardware accesses the buffer correctly.
		 */
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

		ret = dma_map_sgtable(buf->dev, &buf->sgt, DMA_BIDIRECTIONAL,
				      0);
		if (ret) {
			dev_err(buf->dev,
				"rknpu_mem: dma_map_sgtable failed: %d (size=%zu nents=%u)\n",
				ret, buf->mem.size, buf->sgt.nents);
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
	/*
	 * Always report iommu_domain_id = 0 (the device's initial default
	 * domain, registered in rknpu_iommu_init as iommu_domains[0] =
	 * iommu_get_domain_for_dev()).
	 *
	 * Our iommu_map() / dma_map_sgtable() calls operate on the device's
	 * current hardware domain, which is domain 0 for the entire lifetime
	 * of the module.  When librknnrt.so submits a job with domain_id = 0,
	 * rknpu_iommu_domain_get_and_switch() finds 0 == rknpu_dev->
	 * iommu_domain_id and returns immediately (no switch), so the NPU
	 * accesses our mappings in the existing page table.
	 *
	 * Without this, copy_to_user would echo back whatever value
	 * librknnrt sent in the request.  librknnrt v2.3.x passes a
	 * monotonically increasing domain counter (e.g. 10) which triggers
	 * rknpu_iommu_switch_domain() → iommu_detach_device() + attach of a
	 * freshly allocated empty domain → all IOVA mappings disappear →
	 * task counter: 0.
	 */
	args.iommu_domain_id = 0;

	if (copy_to_user((void __user *)data, &args, sizeof(args))) {
		/* fd installed; caller must close to free memory */
		return -EFAULT;
	}

	return 0;

err_vunmap:
	vunmap(buf->mem.kv_addr);
	if (buf->use_iommu_map) {
		struct iommu_domain *domain = iommu_get_domain_for_dev(buf->dev);

		if (domain)
			iommu_unmap(domain, buf->iommu_iova, buf->mem.size);
		goto err_free_pages;
	}
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
	struct rknpu_mem_sync args;
	struct rknpu_mem_buf *buf;
	unsigned long i;

	if (copy_from_user(&args, (void __user *)data, sizeof(args)))
		return -EFAULT;

	if (!args.obj_addr)
		return -EINVAL;

	/*
	 * obj_addr is &buf->mem (filled by rknpu_mem_create_ioctl).
	 * mem is the first member of rknpu_mem_buf so the two pointers
	 * are identical — no container_of offset arithmetic needed.
	 */
	buf = (struct rknpu_mem_buf *)(uintptr_t)args.obj_addr;

	if (buf->use_iommu_map) {
		/*
		 * Large-path: pages were mapped via iommu_map(), bypassing the
		 * DMA subsystem.  The Rockchip IOMMU does not participate in
		 * the ARM64 cache-coherency domain, so cache maintenance must
		 * be done explicitly.
		 *
		 * The mmap path uses pgprot_writecombine, which bypasses the
		 * CPU cache on writes (data goes directly to DRAM).  However
		 * the kernel's vmap alias (PAGE_KERNEL, cacheable) may hold
		 * stale lines populated during __GFP_ZERO at allocation time.
		 * flush_dcache_page() issues a clean+invalidate for each page,
		 * ensuring that subsequent cacheable reads through kv_addr (in
		 * rknpu_job.c) fetch the model data written by librknnrt.so
		 * rather than the stale zero lines.
		 *
		 * Flush all pages regardless of direction: both TO_DEVICE and
		 * FROM_DEVICE require coherent visibility of the full buffer.
		 */
		for (i = 0; i < buf->nr_pages; i++)
			flush_dcache_page(buf->pages[i]);
	} else {
		/*
		 * Small-path: buffer was mapped via dma_map_sgtable().  Use
		 * the DMA layer for cache maintenance so that the Rockchip
		 * IOMMU driver can do any platform-specific work it needs.
		 */
		if (args.flags & RKNPU_MEM_SYNC_TO_DEVICE)
			dma_sync_sg_for_device(buf->dev, buf->sgt.sgl,
					       buf->sgt.nents, DMA_TO_DEVICE);
		if (args.flags & RKNPU_MEM_SYNC_FROM_DEVICE)
			dma_sync_sg_for_cpu(buf->dev, buf->sgt.sgl,
					    buf->sgt.nents, DMA_FROM_DEVICE);
	}

	return 0;
}
