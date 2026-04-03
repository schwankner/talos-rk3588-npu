#!/usr/bin/env bash
# Build RK3588 NPU Talos system extensions and push to REGISTRY.
#
# rockchip-rknpu  — rknpu.ko OOT kernel module (needs siderolabs/pkgs tree)
# rockchip-rknn-libs — librknnrt.so runtime library (standalone bldr build)
#
# Usage:
#   REGISTRY=ghcr.io/mrmoor ./scripts/build-extensions.sh
#   REGISTRY=ghcr.io/mrmoor BUILD_ARG_TAG=6.18.18-talos ./scripts/build-extensions.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=scripts/common.sh
source "${SCRIPT_DIR}/common.sh"

REPO_ROOT="$(dirname "${SCRIPT_DIR}")"
REGISTRY="${REGISTRY:-ghcr.io/mrmoor}"
BUILD_ARG_TAG="${BUILD_ARG_TAG:-${KERNEL_VERSION}}"
CACHE_REGISTRY="${CACHE_REGISTRY:-${REGISTRY}/build-cache}"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

log() { echo "[build-extensions] $*"; }

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "ERROR: $1 is not installed or not in PATH" >&2
        exit 1
    fi
}

require_cmd docker
require_cmd curl

# ---------------------------------------------------------------------------
# Shared buildx builder
# ---------------------------------------------------------------------------

BUILDER_NAME="talos-rk3588-builder"
if ! docker buildx inspect "${BUILDER_NAME}" >/dev/null 2>&1; then
    log "Creating buildx builder ${BUILDER_NAME}..."
    docker buildx create --name "${BUILDER_NAME}" --driver docker-container --use
else
    docker buildx use "${BUILDER_NAME}"
fi

# ---------------------------------------------------------------------------
# rockchip-rknpu — kernel module, requires siderolabs/pkgs tree
# ---------------------------------------------------------------------------

build_rknpu() {
    log "Building rockchip-rknpu (driver ${RKNPU_VERSION}, kernel ${KERNEL_VERSION})..."

    WORK_DIR="$(mktemp -d)"
    trap 'rm -rf "${WORK_DIR}"' RETURN

    log "Downloading siderolabs/pkgs @ ${PKGS_COMMIT}..."
    # GitHub archive API returns a tarball for any commit SHA — avoids the
    # git fetch limitations with shallow clones and commit SHA refs.
    curl -fsSL \
        "https://github.com/siderolabs/pkgs/archive/${PKGS_COMMIT}.tar.gz" \
        -o "${WORK_DIR}/pkgs.tar.gz"
    mkdir -p "${WORK_DIR}/pkgs"
    tar -xzf "${WORK_DIR}/pkgs.tar.gz" \
        --strip-components=1 \
        -C "${WORK_DIR}/pkgs"

    # Copy our extension into the pkgs tree so bldr can resolve stage: kernel-build
    # LLVM_IMAGE / LLVM_REV are defined in rockchip-rknpu/vars.yaml, not injected here.
    cp -r "${REPO_ROOT}/rockchip-rknpu" "${WORK_DIR}/pkgs/rockchip-rknpu"

    local tag="${RKNPU_VERSION}-${KERNEL_VERSION}"
    local image="${REGISTRY}/rockchip-rknpu:${tag}"

    docker buildx build \
        --builder "${BUILDER_NAME}" \
        --file "${WORK_DIR}/pkgs/Pkgfile" \
        --target rockchip-rknpu \
        --platform linux/arm64 \
        --build-arg TAG="${BUILD_ARG_TAG}" \
        --build-arg PKGS="${PKGS_COMMIT}" \
        --cache-from "type=registry,ref=${CACHE_REGISTRY}/rockchip-rknpu" \
        --cache-to   "type=registry,ref=${CACHE_REGISTRY}/rockchip-rknpu,mode=max" \
        --tag "${image}" \
        --push \
        "${WORK_DIR}/pkgs"

    log "Pushed: ${image}"
}

# ---------------------------------------------------------------------------
# rockchip-rknn-libs — librknnrt.so, standalone bldr build
# ---------------------------------------------------------------------------

build_rknn_libs() {
    log "Building rockchip-rknn-libs (librknnrt ${RKNN_RUNTIME_VERSION}, kernel ${KERNEL_VERSION})..."

    local tag="${RKNN_RUNTIME_VERSION}-${KERNEL_VERSION}"
    local image="${REGISTRY}/rockchip-rknn-libs:${tag}"

    docker buildx build \
        --builder "${BUILDER_NAME}" \
        --file "${REPO_ROOT}/Pkgfile" \
        --target rockchip-rknn-libs \
        --platform linux/arm64 \
        --build-arg TAG="${BUILD_ARG_TAG}" \
        --build-arg RKNN_RUNTIME_VERSION="${RKNN_RUNTIME_VERSION}" \
        --cache-from "type=registry,ref=${CACHE_REGISTRY}/rockchip-rknn-libs" \
        --cache-to   "type=registry,ref=${CACHE_REGISTRY}/rockchip-rknn-libs,mode=max" \
        --tag "${image}" \
        --push \
        "${REPO_ROOT}"

    log "Pushed: ${image}"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

TARGET="${1:-all}"

case "${TARGET}" in
    rknpu)      build_rknpu ;;
    rknn-libs)  build_rknn_libs ;;
    all)
        build_rknpu
        build_rknn_libs
        ;;
    *)
        echo "Usage: $0 [rknpu|rknn-libs|all]" >&2
        exit 1
        ;;
esac

log "Done. Images pushed to ${REGISTRY}"
