#!/usr/bin/env python3
"""
Step-by-step init_runtime decomposition for crash diagnosis.
Prints "STEP N: ..." before and after each sub-call.
The last successful STEP before the node crash identifies the culprit.
"""

import os
import sys

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
    from rknnlite.api.rknn_runtime import RKNNRuntime
except ImportError as e:
    sys.exit(f"ERROR: cannot import rknnlite: {e}")

MODEL_PATH = "/model/resnet18.rknn"


def step(n, msg):
    line = f"STEP {n}: {msg}"
    print(line, flush=True)
    print(line, file=sys.stderr, flush=True)


step(0, "script started")

step(1, "RKNNLite(verbose=False)")
rknn = RKNNLite(verbose=False)
step(1, "done")

step(2, f"load_rknn({MODEL_PATH})")
ret = rknn.load_rknn(MODEL_PATH)
step(2, f"done ret={ret}")
if ret != 0:
    sys.exit(f"load_rknn failed: ret={ret}")

step(3, "list /dev/dri")
drm_nodes = [f"/dev/dri/{e}" for e in os.listdir("/dev/dri")] if os.path.exists("/dev/dri") else []
step(3, f"done: {drm_nodes}")

step(4, "RKNNRuntime() constructor (opens DRM device)")
rt = RKNNRuntime(root_dir=rknn.root_dir, target=None, device_id=None,
                 async_mode=False, core_mask=RKNNLite.NPU_CORE_AUTO)
step(4, "done — DRM device opened, librknnrt.so loaded")

step(5, "build_graph() — sends model to NPU, loads firmware")
rt.build_graph(rknn.rknn_data, False)
step(5, "done")

step(6, "set_core_mask(NPU_CORE_AUTO)")
ret = rt.set_core_mask(RKNNLite.NPU_CORE_AUTO)
step(6, f"done ret={ret}")

step(7, "check_rt_version()")
rt.check_rt_version()
step(7, "done")

step(8, "extend_rt_lib()")
rt.extend_rt_lib()
step(8, "done")

step(9, "ALL init_runtime steps completed without crash!")
rt.release()
