#!/usr/bin/env bash
# Assemble a Talos UKI image for a given board, embedding the NPU extensions.
# Usage: BOARD=turing-rk1 ./build-uki.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

BOARD="${BOARD:-turing-rk1}"

echo "Building UKI for board=${BOARD} talos=${TALOS_VERSION} kernel=${KERNEL_VERSION}"
mkdir -p "${DIST}"

# TODO: assemble UKI via siderolabs/imager with:
#   - rockchip-rknpu extension
#   - rockchip-rknn-libs extension
#   - board DTB from boards/${BOARD}/
