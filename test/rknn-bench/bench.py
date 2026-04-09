#!/usr/bin/env python3
"""
RKNN NPU vs CPU benchmark using ResNet18 / RK3588.

Loads the pre-converted resnet18_for_rk3588.rknn model and runs N inferences,
reporting latency and throughput.  Designed to run inside a Kubernetes pod:

  NPU mode:  pod requests rockchip.com/npu=1
             → CDI injects /dev/dri/renderD128 + /usr/lib/librknnrt.so
             → init_runtime(core_mask=NPU_CORE_AUTO)

  CPU mode:  no resource request (or fallback when DRM device absent)
             → init_runtime() falls back to ARM CPU via librknnrt.so

Usage:
  bench.py --mode npu --iterations 200
  bench.py --mode cpu --iterations 50
"""

import argparse
import errno
import os
import sys
import time

import numpy as np
from PIL import Image

# rknnlite reads /proc/device-tree/compatible to detect the SoC.
# In Kubernetes pods, /proc/device-tree is a symlink to /sys/firmware/devicetree/base,
# but /sys/firmware is masked by a read-only empty overlay — so the symlink target
# doesn't exist.  Patch builtins.open to intercept exactly that path and return
# the known content, allowing rknnlite's Cython _get_target_soc to succeed and
# properly set its internal C-struct target_soc field.
_DT_BASE = '/sys/firmware/devicetree/base'
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
             f"  Is librknnrt.so bind-mounted at /usr/lib/librknnrt.so?")

MODEL_PATH  = "/model/resnet18.rknn"
IMAGE_PATH  = "/model/space_shuttle_224.jpg"
INPUT_SIZE  = 224   # ResNet18 input: 224×224 RGB


def load_input() -> np.ndarray:
    """Return a 1×3×224×224 uint8 array (NCHW) from the reference image."""
    img = Image.open(IMAGE_PATH).convert("RGB").resize((INPUT_SIZE, INPUT_SIZE))
    arr = np.array(img, dtype=np.uint8)          # HWC
    arr = np.transpose(arr, (2, 0, 1))           # CHW
    return np.expand_dims(arr, 0)                # NCHW


def probe_device(path: str) -> str:
    """Try raw open() on a device node; return 'ok' or the errno string."""
    try:
        fd = os.open(path, os.O_RDWR)
        os.close(fd)
        return "ok"
    except OSError as e:
        return f"FAIL({e.errno} {errno.errorcode.get(e.errno, '?')} {e.strerror})"


def run_bench(mode: str, iterations: int) -> None:
    print(f"=== RKNN Benchmark  mode={mode}  iterations={iterations} ===", flush=True)
    print(f"[DBG] run_bench entered", file=sys.stderr, flush=True)

    drm_nodes = [p for p in ["/dev/dri/renderD128", "/dev/dri/renderD129"]
                 if os.path.exists(p)]
    lib_ok = os.path.exists("/usr/lib/librknnrt.so")

    proc_dt = "/proc/device-tree"
    try:
        dt_link = os.readlink(proc_dt)
    except OSError as _e:
        dt_link = f"not a symlink ({_e.strerror})"
    sysfw_compat = os.path.join(_DT_BASE, 'compatible')
    compat_path = "/proc/device-tree/compatible"
    try:
        compat_val = open(compat_path, 'rb').read().replace(b'\x00', b' ').strip().decode()
    except OSError as _e:
        try:
            direct = open(sysfw_compat, 'rb').read().replace(b'\x00', b' ').strip().decode()
        except OSError as _e2:
            direct = f"MISSING ({_e2.strerror})"
        compat_val = f"MISSING ({_e.strerror})  /sys/fw={direct}  /proc/dt→{dt_link}"
    print(f"  DRM render node : {drm_nodes[0] if drm_nodes else 'NOT FOUND'}")
    if drm_nodes:
        print(f"  DRM open()      : {probe_device(drm_nodes[0])}")
    dma_heap = "/dev/dma_heap/system"
    print(f"  dma_heap/system : {'present' if os.path.exists(dma_heap) else 'MISSING'}", end="")
    if os.path.exists(dma_heap):
        print(f" open()={probe_device(dma_heap)}", end="")
    print()
    print(f"  librknnrt.so    : {'present' if lib_ok else 'MISSING'}")
    print(f"  /proc/dt/compat : {compat_val}")

    if not lib_ok:
        sys.exit("ERROR: /usr/lib/librknnrt.so not found.\n"
                 "  In NPU pods: CDI injects it automatically.\n"
                 "  In CPU pods: add a hostPath volume for /usr/lib/librknnrt.so.")

    if mode == "npu" and not drm_nodes:
        sys.exit("ERROR: NPU mode but no /dev/dri/renderD* found.\n"
                 "  Add 'rockchip.com/npu: 1' to resources.limits.")

    print(f"[DBG] pre-RKNNLite", file=sys.stderr, flush=True)
    rknn = RKNNLite(verbose=False)
    print(f"[DBG] post-RKNNLite", file=sys.stderr, flush=True)

    print(f"[DBG] calling load_rknn", file=sys.stderr, flush=True)
    ret = rknn.load_rknn(MODEL_PATH)
    print(f"[DBG] load_rknn ret={ret}", file=sys.stderr, flush=True)
    if ret != 0:
        sys.exit(f"ERROR: load_rknn failed (ret={ret})")
    print(f"  Model           : {MODEL_PATH}", flush=True)

    print(f"[DBG] calling init_runtime mode={mode}", file=sys.stderr, flush=True)
    if mode == "npu":
        ret = rknn.init_runtime(core_mask=RKNNLite.NPU_CORE_AUTO)
        runtime_label = "NPU (RK3588)"
    else:
        ret = rknn.init_runtime()
        runtime_label = "CPU (ARM fallback)"
    print(f"[DBG] init_runtime ret={ret}", file=sys.stderr, flush=True)
    if ret != 0:
        sys.exit(f"ERROR: init_runtime failed (ret={ret})")
    print(f"  Runtime         : {runtime_label}", flush=True)

    inp = load_input()
    print(f"  Input shape     : {inp.shape}  dtype={inp.dtype}")

    # Warmup — not counted in timing
    print("  Warmup (10 iterations)...", flush=True)
    for _ in range(10):
        rknn.inference(inputs=[inp])

    # Benchmark
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
    # Machine-readable summary line for easy grep
    print(f"RESULT mode={mode} runtime={runtime_label!r} "
          f"fps={fps:.1f} latency_ms={ms_per:.2f}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["npu", "cpu"], default="npu")
    parser.add_argument("--iterations", type=int, default=200)
    args = parser.parse_args()
    run_bench(args.mode, args.iterations)
