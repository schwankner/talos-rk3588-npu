# RKNN NPU benchmark image for RK3588
#
# librknnrt.so is NOT bundled here — it is injected at runtime by CDI
# when the pod requests rockchip.com/npu: "1".  The rknnlite Python
# package does a dlopen() of librknnrt.so from the standard library
# search path (/usr/lib), so the CDI bind-mount is sufficient.
#
# Build (ARM64 native or via buildx):
#   docker buildx build --platform linux/arm64 \
#     -t ghcr.io/schwankner/rk3588-rknn-bench:v12 --push .
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
# Stage 1: Compile models ONNX → RK3588 RKNN (toolkit 2.3.2, ARM64)
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

# Install rknn-toolkit2 2.3.2 ARM64 wheel (no torch dependency needed for ONNX→RKNN).
# onnxoptimizer is omitted: it requires cmake ≥ 3.19 but bullseye only has 3.18.
# The wheel must be saved with its full platform-tagged filename so pip can verify
# Python/ABI/platform compatibility before installing.
ENV RKNN2_WHL=rknn_toolkit2-2.3.2-cp311-cp311-manylinux_2_17_aarch64.manylinux2014_aarch64.whl
RUN curl -fsSL \
    "https://raw.githubusercontent.com/airockchip/rknn-toolkit2/master/rknn-toolkit2/packages/arm64/${RKNN2_WHL}" \
    -o /tmp/${RKNN2_WHL} \
 && pip install --no-cache-dir \
        "protobuf==3.20.3" "numpy<=1.26.4" "Pillow>=10.0.1" \
        "onnx==1.16.1" "onnxruntime>=1.16.0" \
        "scipy>=1.9.3" "psutil>=5.9.0" "ruamel.yaml>=0.17.21" \
        "tqdm>=4.64.1" "fast-histogram>=0.11" "opencv-python-headless>=4.5.5" \
        "torch==2.2.0" \
 && pip install --no-cache-dir --no-deps /tmp/${RKNN2_WHL} \
 && rm /tmp/${RKNN2_WHL}

# Download ONNX models
# ResNet18-v2-7 (~46 MB) — ImageNet classification, ~1.8 GFLOPS
RUN curl -fsSL \
    "https://media.githubusercontent.com/media/onnx/models/main/validated/vision/classification/resnet/model/resnet18-v2-7.onnx" \
    -o /tmp/resnet18.onnx

# ResNet50-v2-7 (~97 MB) — ImageNet classification, ~8.2 GFLOPS
RUN curl -fsSL \
    "https://media.githubusercontent.com/media/onnx/models/main/validated/vision/classification/resnet/model/resnet50-v2-7.onnx" \
    -o /tmp/resnet50.onnx

# YOLOv5s ReLU variant (~27 MB) — COCO object detection, ~16 GFLOPS
# ReLU activation instead of SiLU for full NPU mapping (no CPU fallback ops).
# Source: airockchip/rknn_model_zoo CDN (standard RKNN benchmark model)
RUN curl -fsSL \
    "https://ftrg.zbox.filez.com/v2/delivery/data/95f00b0fc900458ba134f8b180b3f7a1/examples/yolov5/yolov5s_relu.onnx" \
    -o /tmp/yolov5s.onnx

RUN mkdir -p /model
COPY compile_models.py /tmp/compile_models.py
RUN python3 /tmp/compile_models.py

# ---------------------------------------------------------------------------
# Stage 2: Final bench image (toolkit stripped, only compiled .rknn files kept)
# ---------------------------------------------------------------------------
FROM python:3.11-slim-bullseye
# v13: Add native C API benchmark (bench_c) that bypasses Python/rknnlite overhead
#      to show true NPU throughput (~500-1000 fps ResNet18 INT8, ~60-100 fps YOLOv5s).
#      Add rknn.eval_perf() hardware profiling output to bench.py NPU runs.
LABEL "bench.build"="v13"

# gcc + libc6-dev are only needed to build the LD_PRELOAD shim and bench_c binary;
# purged afterward to keep the image small.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgomp1 strace ca-certificates file curl gcc libc6-dev \
    && rm -rf /var/lib/apt/lists/*

# dt_compat_shim: intercepts the C-level open("/proc/device-tree/compatible")
# that rknnlite's Cython init_runtime() uses for SoC detection.  Without this
# shim, pods would need procMount:Unmasked + hostUsers:false (user namespaces),
# which breaks IDMAP mounts for hostPath volumes and kubelet log collection.
COPY dt_compat_shim.c /tmp/
COPY bench_c.c /tmp/

# Build dt_compat_shim.so and bench_c in the same layer so gcc is purged once.
# bench_c links against librknnrt.so for compilation only (-L/tmp); the actual
# library is injected at runtime by CDI at /usr/lib/librknnrt.so (-rpath).
RUN curl -fsSL \
      "https://raw.githubusercontent.com/airockchip/rknn-toolkit2/v2.3.2/rknpu2/runtime/Linux/librknn_api/include/rknn_api.h" \
      -o /tmp/rknn_api.h \
 && curl -fsSL \
      "https://github.com/airockchip/rknn-toolkit2/raw/v2.3.2/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so" \
      -o /tmp/librknnrt.so \
 && gcc -shared -fPIC -O2 -o /usr/lib/dt_compat_shim.so /tmp/dt_compat_shim.c -ldl \
 && gcc -O2 -o /bench_c /tmp/bench_c.c \
        -I/tmp -L/tmp -lrknnrt -Wl,-rpath,/usr/lib -lpthread -lm \
 && rm /tmp/dt_compat_shim.c /tmp/bench_c.c /tmp/rknn_api.h /tmp/librknnrt.so \
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

# Models — all compiled with rknn-toolkit2 2.3.2 in stage 1
#   resnet18: INT8,  224×224, ~1.8 GFLOPS  → expected ~10× NPU speedup
#   resnet50: fp16,  224×224, ~8.2 GFLOPS  → baseline FP16 comparison (~1.2×)
#   yolov5s:  INT8,  640×640, ~16 GFLOPS   → expected ~5-10× NPU speedup
COPY --from=model-compiler /model/resnet18_int8_for_rk3588.rknn /model/resnet18.rknn
COPY --from=model-compiler /model/resnet50_for_rk3588.rknn      /model/resnet50.rknn
COPY --from=model-compiler /model/yolov5s_int8_for_rk3588.rknn  /model/yolov5s.rknn

COPY bench.py /bench.py
COPY bench_v9.py /bench_v9.py

ENTRYPOINT ["python3", "/bench.py"]
CMD ["--mode", "npu", "--model", "resnet18", "--iterations", "200"]
