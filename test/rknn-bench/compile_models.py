#!/usr/bin/env python3
"""
Compile ResNet18, ResNet50, and YOLOv5s ONNX → RK3588 RKNN.

  ResNet18  — INT8 quantized  (target: ~10× speedup over CPU)
  ResNet50  — fp16 (no quant) (comparison baseline from v11)
  YOLOv5s   — INT8 quantized  (ReLU variant from rknn_model_zoo)

Calibration uses 30 synthetic random images — sufficient for
throughput/latency measurements (accuracy not evaluated here).

Run inside the Docker builder stage after rknn-toolkit2 is installed.
"""
import os
import sys

import numpy as np
from PIL import Image
from rknn.api import RKNN

MODEL_DIR = '/model'

RESNET18_ONNX  = '/tmp/resnet18.onnx'
RESNET50_ONNX  = '/tmp/resnet50.onnx'
YOLOV5S_ONNX   = '/tmp/yolov5s.onnx'

RESNET18_RKNN  = f'{MODEL_DIR}/resnet18_int8_for_rk3588.rknn'
RESNET50_RKNN  = f'{MODEL_DIR}/resnet50_for_rk3588.rknn'
YOLOV5S_RKNN   = f'{MODEL_DIR}/yolov5s_int8_for_rk3588.rknn'


# ---------------------------------------------------------------------------
# Calibration helpers
# ---------------------------------------------------------------------------

def make_calib_dataset(out_dir: str, n: int, hw: tuple) -> str:
    """
    Generate n synthetic RGB images at (h, w) = hw and write a calib.txt
    dataset file listing their paths.  Returns the path to calib.txt.
    """
    os.makedirs(out_dir, exist_ok=True)
    rng = np.random.default_rng(seed=42)
    txt_path = out_dir + '/calib.txt'
    with open(txt_path, 'w') as fh:
        for i in range(n):
            arr = rng.integers(0, 256, (*hw, 3), dtype=np.uint8)
            p   = f'{out_dir}/img_{i:03d}.jpg'
            Image.fromarray(arr).save(p)
            fh.write(p + '\n')
    print(f"  calib dataset: {n} synthetic images ({hw[0]}×{hw[1]}) → {txt_path}")
    return txt_path


# ---------------------------------------------------------------------------
# Compile ResNet18 — INT8
# ---------------------------------------------------------------------------

def compile_resnet18() -> None:
    print(f"\n=== ResNet18 INT8: {RESNET18_ONNX} → {RESNET18_RKNN} ===")

    calib_txt = make_calib_dataset('/tmp/calib_224', 30, (224, 224))

    rknn = RKNN(verbose=False)
    rknn.config(
        target_platform='rk3588',
        optimization_level=3,
        # ImageNet normalisation — runtime converts uint8 → float automatically
        mean_values=[[123.675, 116.28,  103.53]],
        std_values=[[58.395,  57.12,   57.375]],
    )

    # ResNet18-v2-7.onnx has a dynamic batch dim ('N', 3, 224, 224)
    ret = rknn.load_onnx(model=RESNET18_ONNX,
                         inputs=['data'],
                         input_size_list=[[1, 3, 224, 224]])
    if ret != 0:
        sys.exit(f'load_onnx failed: {ret}')
    print('  load_onnx OK')

    ret = rknn.build(do_quantization=True, dataset=calib_txt)
    if ret != 0:
        sys.exit(f'build failed: {ret}')
    print('  build OK (INT8)')

    ret = rknn.export_rknn(RESNET18_RKNN)
    if ret != 0:
        sys.exit(f'export_rknn failed: {ret}')
    print(f'  exported → {RESNET18_RKNN}')
    rknn.release()


# ---------------------------------------------------------------------------
# Compile ResNet50 — fp16 (no quantization, kept as comparison baseline)
# ---------------------------------------------------------------------------

def compile_resnet50() -> None:
    print(f"\n=== ResNet50 fp16: {RESNET50_ONNX} → {RESNET50_RKNN} ===")

    rknn = RKNN(verbose=False)
    rknn.config(
        target_platform='rk3588',
        optimization_level=3,
        mean_values=[[123.675, 116.28,  103.53]],
        std_values=[[58.395,  57.12,   57.375]],
    )

    ret = rknn.load_onnx(model=RESNET50_ONNX,
                         inputs=['data'],
                         input_size_list=[[1, 3, 224, 224]])
    if ret != 0:
        sys.exit(f'load_onnx failed: {ret}')
    print('  load_onnx OK')

    ret = rknn.build(do_quantization=False)
    if ret != 0:
        sys.exit(f'build failed: {ret}')
    print('  build OK (fp16)')

    ret = rknn.export_rknn(RESNET50_RKNN)
    if ret != 0:
        sys.exit(f'export_rknn failed: {ret}')
    print(f'  exported → {RESNET50_RKNN}')
    rknn.release()


# ---------------------------------------------------------------------------
# Compile YOLOv5s — INT8
# ---------------------------------------------------------------------------

def compile_yolov5s() -> None:
    print(f"\n=== YOLOv5s INT8: {YOLOV5S_ONNX} → {YOLOV5S_RKNN} ===")

    calib_txt = make_calib_dataset('/tmp/calib_640', 30, (640, 640))

    rknn = RKNN(verbose=False)
    rknn.config(
        target_platform='rk3588',
        optimization_level=3,
        # YOLOv5 normalises [0,255] → [0,1] by dividing by 255;
        # express that as mean=0 / std=255 so the runtime handles uint8 input.
        mean_values=[[0, 0, 0]],
        std_values=[[255, 255, 255]],
    )

    ret = rknn.load_onnx(model=YOLOV5S_ONNX,
                         inputs=['images'],
                         input_size_list=[[1, 3, 640, 640]])
    if ret != 0:
        sys.exit(f'load_onnx failed: {ret}')
    print('  load_onnx OK')

    ret = rknn.build(do_quantization=True, dataset=calib_txt)
    if ret != 0:
        sys.exit(f'build failed: {ret}')
    print('  build OK (INT8)')

    ret = rknn.export_rknn(YOLOV5S_RKNN)
    if ret != 0:
        sys.exit(f'export_rknn failed: {ret}')
    print(f'  exported → {YOLOV5S_RKNN}')
    rknn.release()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if __name__ == '__main__':
    os.makedirs(MODEL_DIR, exist_ok=True)
    compile_resnet18()
    compile_resnet50()
    compile_yolov5s()
    print('\nDone.')
