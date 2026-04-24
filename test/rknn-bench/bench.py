#!/usr/bin/env python3
"""
RKNN NPU vs CPU benchmark for RK3588.

Supports multiple models to demonstrate NPU speedup at different compute levels:

  resnet18  - 224x224, ~1.8 GFLOPS  (light; NPU ~= CPU at batch-1)
  yolov5s   - 640x640, ~16 GFLOPS   (heavy; NPU clearly faster)

Usage:
  bench.py --mode npu --model yolov5s --iterations 200
  bench.py --mode cpu --model yolov5s --iterations 50
"""

import argparse
import errno
import os
import sys
import time

import numpy as np

# rknnlite reads /proc/device-tree/compatible to detect the SoC.
# The dt_compat_shim.so LD_PRELOAD intercepts the C-level open(); this
# Python-level patch covers the pure-Python fallback path.
import builtins as _builtins
import io as _io
_real_open = _builtins.open
_DT_COMPAT = b'turing,rk1\x00rockchip,rk3588\x00'
def _patched_open(file, mode='r', *args, **kwargs):
    if str(file) == '/proc/device-tree/compatible':
        if isinstance(mode, str) and 'b' in mode:
            return _io.BytesIO(_DT_COMPAT)
        return _io.StringIO(_DT_COMPAT.decode('utf-8', errors='ignore'))
    return _real_open(file, mode, *args, **kwargs)
_builtins.open = _patched_open

try:
    from rknnlite.api import RKNNLite
except ImportError as e:
    sys.exit(f"ERROR: cannot import rknnlite: {e}\n"
             "  Is librknnrt.so bind-mounted at /usr/lib/librknnrt.so?")

# ---------------------------------------------------------------------------
# Model registry
# ---------------------------------------------------------------------------
# input_shape: (N, C, H, W) — all models compiled with NCHW layout
# dtype:       numpy dtype for the input tensor
MODEL_CONFIGS = {
    'resnet18': {
        'path':        '/model/resnet18.rknn',
        'input_shape': (1, 3, 224, 224),
        'dtype':       np.uint8,
        'gflops':      1.8,
    },
    'yolov5s': {
        'path':        '/model/yolov5s.rknn',
        'input_shape': (1, 3, 640, 640),
        'dtype':       np.uint8,
        'gflops':      16.0,
    },
}


def probe_device(path: str) -> str:
    try:
        fd = os.open(path, os.O_RDWR)
        os.close(fd)
        return "ok"
    except OSError as e:
        return f"FAIL({e.errno} {errno.errorcode.get(e.errno, '?')} {e.strerror})"


def run_bench(mode: str, model_name: str, iterations: int) -> None:
    cfg = MODEL_CONFIGS[model_name]

    print(f"=== RKNN Benchmark  mode={mode}  model={model_name}"
          f"  gflops={cfg['gflops']}  iterations={iterations} ===", flush=True)

    rknpu_dev = "/dev/rknpu"
    rknpu_ok  = os.path.exists(rknpu_dev)
    lib_ok    = os.path.exists("/usr/lib/librknnrt.so")
    dma_heap  = "/dev/dma_heap/system"

    # Read /proc/device-tree/compatible (patched open above handles it)
    try:
        compat_val = open('/proc/device-tree/compatible', 'rb').read().replace(b'\x00', b' ').strip().decode()
    except OSError as e:
        compat_val = f"MISSING ({e.strerror})"

    print(f"  /dev/rknpu      : {'present' if rknpu_ok else 'NOT FOUND'}", end="")
    if rknpu_ok:
        print(f"  open()={probe_device(rknpu_dev)}", end="")
    print()
    print(f"  dma_heap/system : {'present' if os.path.exists(dma_heap) else 'MISSING'}", end="")
    if os.path.exists(dma_heap):
        print(f"  open()={probe_device(dma_heap)}", end="")
    print()
    print(f"  librknnrt.so    : {'present' if lib_ok else 'MISSING'}")
    print(f"  /proc/dt/compat : {compat_val}")

    if not lib_ok:
        sys.exit("ERROR: /usr/lib/librknnrt.so not found.")
    if mode == "npu" and not rknpu_ok:
        sys.exit("ERROR: NPU mode but /dev/rknpu not found.\n"
                 "  Add 'rockchip.com/npu: 1' to resources.limits.")

    rknn = RKNNLite(verbose=False)

    ret = rknn.load_rknn(cfg['path'])
    if ret != 0:
        sys.exit(f"ERROR: load_rknn({cfg['path']}) failed ret={ret}")
    print(f"  Model           : {cfg['path']}  (~{cfg['gflops']} GFLOPS)", flush=True)

    if mode == "npu":
        ret = rknn.init_runtime(core_mask=RKNNLite.NPU_CORE_AUTO)
        runtime_label = "NPU (RK3588)"
    else:
        ret = rknn.init_runtime()
        runtime_label = "CPU (ARM fallback)"
    if ret != 0:
        sys.exit(f"ERROR: init_runtime failed ret={ret}")
    print(f"  Runtime         : {runtime_label}", flush=True)

    # Random uint8 input — sufficient for latency measurement
    inp = np.random.randint(0, 256, cfg['input_shape'], dtype=cfg['dtype'])
    print(f"  Input shape     : {inp.shape}  dtype={inp.dtype}")

    print(f"  Warmup (10 iterations)...", flush=True)
    for _ in range(10):
        rknn.inference(inputs=[inp])

    print(f"  Running {iterations} inferences...", flush=True)
    t0 = time.perf_counter()
    for _ in range(iterations):
        rknn.inference(inputs=[inp])
    elapsed = time.perf_counter() - t0

    fps    = iterations / elapsed
    ms_per = elapsed / iterations * 1000

    rknn.release()

    print()
    print(f"  Total time  : {elapsed:.3f} s")
    print(f"  Throughput  : {fps:.1f} fps")
    print(f"  Latency     : {ms_per:.2f} ms / inference")
    print()
    print(f"RESULT mode={mode} model={model_name} runtime={runtime_label!r} "
          f"fps={fps:.1f} latency_ms={ms_per:.2f}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode",       choices=["npu", "cpu"],
                        default="npu")
    parser.add_argument("--model",      choices=list(MODEL_CONFIGS),
                        default="yolov5s")
    parser.add_argument("--iterations", type=int, default=200)
    args = parser.parse_args()
    run_bench(args.mode, args.model, args.iterations)
