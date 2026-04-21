#!/usr/bin/env bash
# Shared version pins and helper variables.
# Source this file from all other scripts: source "$(dirname "$0")/common.sh"

set -euo pipefail

# Talos Linux
TALOS_VERSION="${TALOS_VERSION:-v1.12.6}"

# Linux kernel (must match the Talos release above)
# Verified on Turing RK1 worker (10.0.60.4): uname -r = 6.18.18-talos
KERNEL_VERSION="${KERNEL_VERSION:-6.18.18-talos}"

# siderolabs/pkgs commit pinned to the Talos release
# Source: https://github.com/siderolabs/talos/blob/v1.12.6/pkg/machinery/gendata/data/pkgs
PKGS_COMMIT="${PKGS_COMMIT:-a92bed5}"

# Rockchip NPU driver (vendor rknpu, not mainline rocket)
# Source: https://github.com/w568w/rknpu-module (out-of-tree port to mainline kernel)
# Version refers to the RKNN SDK ABI level, not a release tag (there are none).
RKNPU_VERSION="${RKNPU_VERSION:-0.9.9}"

# Rockchip RKNN runtime library (librknnrt.so)
# Must match RKNPU_VERSION — same SDK release
# Source: https://github.com/airockchip/rknn-toolkit2
RKNN_RUNTIME_VERSION="${RKNN_RUNTIME_VERSION:-2.3.2}"

# OCI registry for intermediate build images (local or remote)
REGISTRY="${REGISTRY:-registry.local:5000}"

# Build output directory
DIST="${DIST:-$(dirname "$(dirname "$0")")/dist}"
