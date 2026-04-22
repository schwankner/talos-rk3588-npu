# RKNN NPU benchmark image for RK3588
#
# librknnrt.so is NOT bundled here — it is injected at runtime by CDI
# when the pod requests rockchip.com/npu: "1".  The rknnlite Python
# package does a dlopen() of librknnrt.so from the standard library
# search path (/usr/lib), so the CDI bind-mount is sufficient.
#
# Build (ARM64 native or via buildx):
#   docker buildx build --platform linux/arm64 \
#     -t ghcr.io/schwankner/rk3588-rknn-bench:v19 --push .
#
# Base image selection history:
#   python:3.11-slim (Trixie/glibc 2.40+): BTI/PAC exec format errors on RK3588
#   debian:12-slim (Bookworm/glibc 2.36): Debian 12 ARM64 ELF incompatible with
#     Talos 6.18.18/RK3588 kernel (exec format error on all Debian 12 containers)
#   ubuntu:22.04 (glibc 2.35): python3.11 binary zero-truncated to 0 bytes by
#     BuildKit layer-diff bug when installing large binaries via apt in a RUN step
#   python:3.11-slim-bullseye (Debian 11/glibc 2.31): python3.11 pre-baked in
#     base layers (no BuildKit diff/truncation), glibc 2.31 works on this kernel.
FROM python:3.11-slim-bullseye

# gcc is only needed to build the LD_PRELOAD shim; purged afterward.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgomp1 strace ca-certificates file curl gcc \
    && rm -rf /var/lib/apt/lists/*

# dt_compat_shim: intercepts the C-level open("/proc/device-tree/compatible")
# that rknnlite's Cython init_runtime() uses for SoC detection.  Without this
# shim, pods would need procMount:Unmasked + hostUsers:false (user namespaces),
# which breaks IDMAP mounts for hostPath volumes and kubelet log collection.
COPY dt_compat_shim.c /tmp/
RUN gcc -shared -fPIC -O2 -o /usr/lib/dt_compat_shim.so /tmp/dt_compat_shim.c -ldl \
    && rm /tmp/dt_compat_shim.c \
    && apt-get purge -y gcc && apt-get autoremove -y && rm -rf /var/lib/apt/lists/*

# Activate the shim for all processes in the container (build-time ENV is
# only set in the final image, not during earlier RUN steps).
ENV LD_PRELOAD=/usr/lib/dt_compat_shim.so

# rknn-toolkit-lite2 wheel (Python wrapper that dlopen()s librknnrt.so)
# Pre-downloaded from github.com/airockchip/rknn-toolkit2 v2.3.2 packages/
COPY rknn_toolkit_lite2-2.3.2-cp311-cp311-manylinux_2_17_aarch64.manylinux2014_aarch64.whl /tmp/
RUN pip install --no-cache-dir \
    /tmp/rknn_toolkit_lite2-2.3.2-cp311-cp311-manylinux_2_17_aarch64.manylinux2014_aarch64.whl \
    numpy Pillow \
    && rm /tmp/rknn_toolkit_lite2-2.3.2-cp311-cp311-manylinux_2_17_aarch64.manylinux2014_aarch64.whl

# Pre-converted ResNet18 model for RK3588 (from rknn-toolkit2 examples)
COPY resnet18.rknn /model/resnet18.rknn

# Reference image for the real-input test
COPY space_shuttle_224.jpg /model/space_shuttle_224.jpg

COPY bench.py /bench.py
COPY bench_v9.py /bench_v9.py

ENTRYPOINT ["python3", "/bench.py"]
CMD ["--mode", "npu", "--iterations", "200"]
