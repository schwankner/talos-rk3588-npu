/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rknpu_build_config.h — force DMA_HEAP compilation mode.
 *
 * Forced-included via ccflags-y in Kbuild:
 *   ccflags-y += -include $(src)/src/include/compat/rknpu_build_config.h
 *
 * GCC processes all -D/-U command-line flags before any -include files.
 * This means a bare -UCONFIG_ROCKCHIP_RKNPU_DRM_GEM in ccflags-y is
 * overridden by a subsequent #define inside -include linux/kconfig.h
 * (which reads autoconf.h).  This header is a second -include file that
 * appears after linux/kconfig.h in the compiler command (ccflags-y flags
 * follow KBUILD_CFLAGS in the GCC invocation), so its #undef executes
 * after autoconf.h and wins unconditionally.
 *
 * DMA_HEAP mode: misc_register() -> /dev/rknpu (required by librknnrt.so).
 * DRM_GEM mode:  drm_dev_register() -> /dev/dri/renderD* (unusable by librknnrt).
 */
#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
#undef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
#endif

#ifndef CONFIG_ROCKCHIP_RKNPU_DMA_HEAP
#define CONFIG_ROCKCHIP_RKNPU_DMA_HEAP 1
#endif
