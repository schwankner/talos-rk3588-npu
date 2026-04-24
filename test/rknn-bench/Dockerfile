# RKNN NPU benchmark image for RK3588
#
# librknnrt.so is NOT bundled here — it is injected at runtime by CDI
# when the pod requests rockchip.com/npu: "1".  The rknnlite Python
# package does a dlopen() of librknnrt.so from the standard library
# search path (/usr/lib), so the CDI bind-mount is sufficient.
#
# Build (ARM64 native or via buildx):
#   docker buildx build --platform linux/arm64 \
#     -t ghcr.io/schwankner/rk3588-rknn-bench:v23 --push .
#
# Base image selection history:
#   python:3.11-slim (Trixie/glibc 2.40+): BTI/PAC exec format errors on RK3588
#   debian:12-slim (Bookworm/glibc 2.36): Debian 12 ARM64 ELF incompatible with
#     Talos 6.18.18/RK3588 kernel (exec format error on all Debian 12 containers)
#   ubuntu:22.04 (glibc 2.35): python3.11 binary zero-truncated to 0 bytes by
#     BuildKit layer-diff bug when installing large binaries via apt in a RUN step
#   python:3.11-slim-bullseye (Debian 11/glibc 2.31): python3.11 pre-baked in
#     base layers (no BuildKit diff/truncation), glibc 2.31 works on this kernel.

# ---------------------------------------------------------------------------
# Stage 1: Compile ResNet50 ONNX → RK3588 RKNN (toolkit 2.3.2, ARM64)
#
# rknn-toolkit2 (compilation toolkit) is separate from rknn-toolkit-lite2
# (inference runtime).  The full toolkit runs entirely offline; it takes an
# ONNX model, converts ops to RKNN format, and exports a .rknn file.
#
# ONNX→RKNN compilation does not require torch at runtime — torch is listed
# in the requirements only for direct PyTorch (.pt) model loading.  We
# install with --no-deps and supply only the ONNX pipeline deps manually.
# ---------------------------------------------------------------------------
FROM python:3.11-slim-bullseye AS model-compiler

RUN apt-get update && apt-get install -y --no-install-recommends \
        curl ca-certificates libgomp1 libglib2.0-0 \
    && rm -rf /var/lib/apt/lists/*

# Install rknn-toolkit2 2.3.2 ARM64 wheel (no torch dependency needed for ONNX)
RUN curl -fsSL \
    "https://raw.githubusercontent.com/airockchip/rknn-toolkit2/master/rknn-toolkit2/packages/arm64/rknn_toolkit2-2.3.2-cp311-cp311-manylinux_2_17_aarch64.manylinux2014_aarch64.whl" \
    -o /tmp/rknn_toolkit2.whl \
 && pip install --no-cache-dir \
        "protobuf==3.20.3" "numpy<=1.26.4" "Pillow>=10.0.1" \
        "onnx==1.16.1" "onnxoptimizer==0.3.8" "onnxruntime>=1.16.0" \
        "scipy>=1.9.3" "psutil>=5.9.0" "ruamel.yaml>=0.17.21" \
        "tqdm>=4.64.1" "fast-histogram>=0.11" "opencv-python-headless>=4.5.5" \
 && pip install --no-cache-dir --no-deps /tmp/rknn_toolkit2.whl \
 && rm /tmp/rknn_toolkit2.whl

# Download ResNet50-v2 ONNX (~97 MB) from the ONNX model zoo (git-lfs CDN)
RUN curl -fsSL \
    "https://media.githubusercontent.com/media/onnx/models/main/validated/vision/classification/resnet/model/resnet50-v2-7.onnx" \
    -o /tmp/resnet50.onnx

RUN mkdir -p /model
COPY compile_models.py /tmp/compile_models.py
RUN python3 /tmp/compile_models.py

# ---------------------------------------------------------------------------
# Stage 2: Final bench image (toolkit stripped, only the compiled .rknn kept)
# ---------------------------------------------------------------------------
FROM python:3.11-slim-bullseye
# v23: ResNet50 compiled with rknn-toolkit2 2.3.2 (same version as librknnrt.so)
#      for a fair NPU vs CPU comparison. YOLOv5s kept for reference.
LABEL "bench.build"="v23"

# gcc + libc6-dev are only needed to build the LD_PRELOAD shim; purged afterward.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgomp1 strace ca-certificates file curl gcc libc6-dev \
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

# Models
# resnet18: pre-converted, ~1.8 GFLOPS (light; reference)
# resnet50: compiled in stage 1 with toolkit 2.3.2, ~8 GFLOPS fp16 (clear NPU speedup)
# yolov5s:  pre-compiled with toolkit 1.6.2, ~16 GFLOPS (reference; limited speedup due to version mismatch)
COPY resnet18.rknn /model/resnet18.rknn
COPY --from=model-compiler /model/resnet50_for_rk3588.rknn /model/resnet50.rknn
RUN curl -fsSL \
    "https://raw.githubusercontent.com/airockchip/rknn-toolkit2/master/rknpu2/examples/rknn_yolov5_demo/model/RK3588/yolov5s-640-640.rknn" \
    -o /model/yolov5s.rknn

COPY bench.py /bench.py
COPY bench_v9.py /bench_v9.py

ENTRYPOINT ["python3", "/bench.py"]
CMD ["--mode", "npu", "--model", "resnet50", "--iterations", "200"]
