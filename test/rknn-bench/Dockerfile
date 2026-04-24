# RKNN NPU benchmark image for RK3588
#
# librknnrt.so is NOT bundled here — it is injected at runtime by CDI
# when the pod requests rockchip.com/npu: "1".  The rknnlite Python
# package does a dlopen() of librknnrt.so from the standard library
# search path (/usr/lib), so the CDI bind-mount is sufficient.
#
# Build (ARM64 native or via buildx):
#   docker buildx build --platform linux/arm64 \
#     -t ghcr.io/schwankner/rk3588-rknn-bench:v16 --push .
#
# Base image selection history:
#   python:3.11-slim (Trixie/glibc 2.40+): BTI/PAC exec format errors on RK3588
#   debian:12-slim (Bookworm/glibc 2.36): Debian 12 ARM64 ELF incompatible with
#     Talos 6.18.18/RK3588 kernel (exec format error on all Debian 12 containers)
#   ubuntu:22.04 (glibc 2.35): python3.11 binary zero-truncated to 0 bytes by
#     BuildKit layer-diff bug when installing large binaries via apt in a RUN step
#   python:3.11-slim-bullseye (Debian 11/glibc 2.31): python3.11 pre-baked in
#     base layers (no BuildKit diff/truncation), glibc 2.31 works on this kernel.
#
# C binary build isolation (v16+):
#   gcc compilation of bench_c, bench_c_mt, and dt_compat_shim.so was moved into
#   a dedicated c-compiler stage.  The binaries are then COPY --from'd into the
#   final stage.  This avoids the BuildKit layer-diff truncation bug that zeroed
#   the compiled binaries when the large librknnrt.so download + gcc compile +
#   apt-purge all landed in the same RUN layer of the final stage (Bug 50).

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
# Stage 2: C binary compiler
#
# Isolated from the final stage so the BuildKit layer-diff bug (which
# zero-truncates compiled binaries when a large download + gcc + apt-purge
# all land in the same RUN layer) cannot affect the output.  Binaries are
# COPY --from'd into the final stage as plain file copies.
# ---------------------------------------------------------------------------
FROM python:3.11-slim-bullseye AS c-compiler

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc libc6-dev curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY dt_compat_shim.c bench_c.c bench_c_mt.c /tmp/

# Download rknn_api.h and librknnrt.so (link-time only; runtime lib injected by CDI).
# librknnrt.so is ~190 MB — keeping it in a dedicated stage prevents the
# BuildKit layer-diff truncation bug from zeroing the gcc output binaries.
RUN curl -fsSL \
      "https://raw.githubusercontent.com/airockchip/rknn-toolkit2/v2.3.2/rknpu2/runtime/Linux/librknn_api/include/rknn_api.h" \
      -o /tmp/rknn_api.h \
 && curl -fsSL \
      "https://github.com/airockchip/rknn-toolkit2/raw/v2.3.2/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so" \
      -o /tmp/librknnrt.so \
 && gcc -shared -fPIC -O2 -o /dt_compat_shim.so /tmp/dt_compat_shim.c -ldl \
 && gcc -O2 -o /bench_c /tmp/bench_c.c \
        -I/tmp -L/tmp -lrknnrt -Wl,-rpath,/usr/lib -lpthread -lm \
 && gcc -O2 -o /bench_c_mt /tmp/bench_c_mt.c \
        -I/tmp -L/tmp -lrknnrt -Wl,-rpath,/usr/lib -lpthread -lm

# ---------------------------------------------------------------------------
# Stage 3: Final bench image (toolkit stripped, only compiled .rknn files kept)
# ---------------------------------------------------------------------------
FROM python:3.11-slim-bullseye
# v16: Move C compilation into isolated c-compiler stage; COPY --from to avoid
#      BuildKit layer-diff zero-truncation of gcc output binaries (Bug 50).
LABEL "bench.build"="v16"

RUN apt-get update && apt-get install -y --no-install-recommends \
        libgomp1 strace ca-certificates file curl \
    && rm -rf /var/lib/apt/lists/*

# C binaries copied from c-compiler stage — not subject to layer-diff truncation.
COPY --from=c-compiler /dt_compat_shim.so  /usr/lib/dt_compat_shim.so
COPY --from=c-compiler /bench_c            /bench_c
COPY --from=c-compiler /bench_c_mt         /bench_c_mt

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
