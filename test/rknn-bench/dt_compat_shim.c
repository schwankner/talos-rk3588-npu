/*
 * dt_compat_shim.c — LD_PRELOAD shim for rknnlite SoC detection.
 *
 * rknnlite's Cython init_runtime() does a C-level open() of
 * /proc/device-tree/compatible to identify the SoC.  In a Kubernetes
 * pod this path is masked unless procMount:Unmasked is set, which in
 * turn requires hostUsers:false (user namespaces).  User namespaces
 * break both IDMAP mounts for hostPath volumes AND kubelet log
 * collection.
 *
 * This shim intercepts the C-level open() for the one path rknnlite
 * needs and returns an anonymous memfd containing the known-good RK3588
 * compatible string.  All other open() calls are forwarded to libc.
 * With this shim active via LD_PRELOAD, neither procMount:Unmasked nor
 * hostUsers:false are required.
 *
 * Compiled during image build; activated at runtime via ENV LD_PRELOAD.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Compatible string expected by rknnlite for Turing RK1 / any RK3588 */
static const char DT_COMPAT[] = "turing,rk1\0rockchip,rk3588\0";
#define DT_COMPAT_LEN (sizeof(DT_COMPAT))

#define DT_PATH "/proc/device-tree/compatible"

typedef int (*open_fn_t)(const char *, int, ...);

static open_fn_t real_open(void)
{
    static open_fn_t fn = NULL;
    if (!fn)
        fn = (open_fn_t)dlsym(RTLD_NEXT, "open");
    return fn;
}

static int make_compat_fd(void)
{
    int fd = (int)syscall(SYS_memfd_create, "dt_compat", 0);
    if (fd < 0)
        return fd;
    write(fd, DT_COMPAT, DT_COMPAT_LEN);
    lseek(fd, 0, SEEK_SET);
    return fd;
}

int open(const char *pathname, int flags, ...)
{
    if (pathname && strcmp(pathname, DT_PATH) == 0)
        return make_compat_fd();

    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);
    return real_open()(pathname, flags, mode);
}

/* glibc also exports open64 on 32-bit; intercept it for completeness */
int open64(const char *pathname, int flags, ...)
{
    if (pathname && strcmp(pathname, DT_PATH) == 0)
        return make_compat_fd();

    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);

    typedef int (*open64_fn_t)(const char *, int, ...);
    static open64_fn_t fn64 = NULL;
    if (!fn64)
        fn64 = (open64_fn_t)dlsym(RTLD_NEXT, "open64");
    return fn64(pathname, flags, mode);
}
