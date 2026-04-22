# RKNN NPU benchmark image for RK3588
#
# librknnrt.so is NOT bundled here — it is injected at runtime by CDI
# when the pod requests rockchip.com/npu: "1".  The rknnlite Python
# package does a dlopen() of librknnrt.so from the standard library
# search path (/usr/lib), so the CDI bind-mount is sufficient.
#
# Build (ARM64 native or via buildx):
#   docker buildx build --platform linux/arm64 \
#     -t ghcr.io/schwankner/rk3588-rknn-bench:v18 --push .
#
# Base image selection history:
#   python:3.11-slim (Trixie/glibc 2.40+): BTI/PAC exec format errors on RK3588
#   debian:12-slim (Bookworm/glibc 2.36): Debian 12 ARM64 causes exec format errors
#     on Talos 6.18.18/RK3588 (kernel ↔ Debian glibc 2.36 ABI incompatibility)
#   ubuntu:22.04 (glibc 2.35): python3.11 binary zero-truncated to 0 bytes by
#     BuildKit layer-diff bug when installing large binaries via apt in a RUN step
#   python:3.11-slim-bullseye (Debian 11/glibc 2.31): python3.11 is pre-baked in the
#     base image layers (no BuildKit diff, no truncation), glibc 2.31 works on this
#     kernel.  Confirmed working on Talos 6.18.18/RK3588.
FROM python:3.11-slim-bullseye

RUN apt-get update && apt-get install -y --no-install-recommends \
        libgomp1 strace ca-certificates file curl \
    && rm -rf /var/lib/apt/lists/*

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
