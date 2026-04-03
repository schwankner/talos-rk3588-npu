#!/usr/bin/env bash
# Build the RK3588 NPU Talos system extensions (rknpu + rknn-libs).
# Produces OCI images pushed to ${REGISTRY}.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

echo "Building rockchip-rknpu extension (rknpu ${RKNPU_VERSION}, kernel ${KERNEL_VERSION})"
# TODO: implement bldr build for rockchip-rknpu/pkg.yaml

echo "Building rockchip-rknn-libs extension (librknnrt ${RKNN_RUNTIME_VERSION})"
# TODO: implement bldr build for rockchip-rknn-libs/pkg.yaml

echo "Done. Images pushed to ${REGISTRY}"
