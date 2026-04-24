#!/usr/bin/env python3
"""
Compile ResNet50 ONNX → RK3588 RKNN (float16, no quantization).

Run inside the Docker builder stage after rknn-toolkit2 is installed.
Output: /model/resnet50_for_rk3588.rknn
"""
import sys

from rknn.api import RKNN

ONNX_PATH  = '/tmp/resnet50.onnx'
RKNN_PATH  = '/model/resnet50_for_rk3588.rknn'

print(f"Compiling {ONNX_PATH} → {RKNN_PATH} (target=rk3588, fp16)")

rknn = RKNN(verbose=False)

# mean/std match ImageNet normalization baked into many ResNet ONNX models;
# set here so the runtime handles the uint8→float conversion automatically,
# allowing the bench to feed raw uint8 input (same as ResNet18).
rknn.config(
    target_platform='rk3588',
    optimization_level=3,
    mean_values=[[123.675, 116.28,  103.53]],
    std_values=[[58.395,  57.12,   57.375]],
)

# ResNet50-v2-7.onnx has a dynamic batch dimension ('N', 3, 224, 224).
# RKNN requires static shapes; fix batch=1 via input_size_list.
ret = rknn.load_onnx(model=ONNX_PATH, inputs=['data'], input_size_list=[[1, 3, 224, 224]])
if ret != 0:
    sys.exit(f"load_onnx failed: {ret}")
print("  load_onnx OK")

# do_quantization=False → float16; no calibration data required.
ret = rknn.build(do_quantization=False)
if ret != 0:
    sys.exit(f"build failed: {ret}")
print("  build OK")

ret = rknn.export_rknn(RKNN_PATH)
if ret != 0:
    sys.exit(f"export_rknn failed: {ret}")
print(f"  exported → {RKNN_PATH}")

rknn.release()
print("Done.")
