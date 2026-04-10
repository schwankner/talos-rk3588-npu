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
REGISTRY="${REGISTRY:-ghcr.io/schwankner}"
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
# Shared pkgs tree setup — downloaded once, reused for both targets
# ---------------------------------------------------------------------------

# PKGS_WORK_DIR is set by setup_pkgs_tree; callers must call that first.
PKGS_WORK_DIR=""

setup_pkgs_tree() {
    if [ -n "${PKGS_WORK_DIR}" ]; then
        return  # already set up
    fi

    PKGS_WORK_DIR="$(mktemp -d)"
    # shellcheck disable=SC2064
    trap 'rm -rf "${PKGS_WORK_DIR}"' EXIT

    log "Downloading siderolabs/pkgs @ ${PKGS_COMMIT}..."
    # GitHub archive API returns a tarball for any commit SHA — avoids the
    # git fetch limitations with shallow clones and commit SHA refs.
    curl -fsSL \
        "https://github.com/siderolabs/pkgs/archive/${PKGS_COMMIT}.tar.gz" \
        -o "${PKGS_WORK_DIR}/pkgs.tar.gz"
    mkdir -p "${PKGS_WORK_DIR}/pkgs"
    tar -xzf "${PKGS_WORK_DIR}/pkgs.tar.gz" \
        --strip-components=1 \
        -C "${PKGS_WORK_DIR}/pkgs"

    # Inject both extension packages into the pkgs tree so bldr can resolve
    # stage: base and stage: kernel-build dependencies.
    cp -r "${REPO_ROOT}/rockchip-rknpu"    "${PKGS_WORK_DIR}/pkgs/rockchip-rknpu"
    cp -r "${REPO_ROOT}/rockchip-rknn-libs" "${PKGS_WORK_DIR}/pkgs/rockchip-rknn-libs"

    # Apply RK3588 NPU kernel config fragment on top of the upstream ARM64 config.
    # Each line in the fragment is "KEY=value"; we remove any existing entry
    # (commented-out or active) then append the new value.  This mirrors the
    # behaviour of scripts/kconfig/merge_config.sh without requiring the kernel
    # source tree at setup time.
    local fragment="${REPO_ROOT}/kernel/config-arm64-rk3588-npu.fragment"
    local pkgs_config="${PKGS_WORK_DIR}/pkgs/kernel/build/config-arm64"
    log "Applying kernel config fragment: $(basename "${fragment}")"
    while IFS= read -r line; do
        # Skip blank lines and comment lines
        [[ "${line}" =~ ^[[:space:]]*# ]] && continue
        [[ -z "${line// }" ]] && continue
        key="${line%%=*}"
        # Remove existing setting (active "KEY=..." or disabled "# KEY is not set").
        # Use a temp-file replacement instead of sed -i for macOS/BSD compatibility.
        local tmp
        tmp="$(mktemp)"
        grep -v "^${key}[= ]\|^# ${key} is not set$" "${pkgs_config}" > "${tmp}" || true
        mv "${tmp}" "${pkgs_config}"
        echo "${line}" >> "${pkgs_config}"
    done < "${fragment}"
    log "Kernel config fragment applied."
}

# ---------------------------------------------------------------------------
# kernel — full kernel image for the custom imager
#
# rknpu.ko must be signed with the same key embedded in the running kernel.
# The signing key is ephemeral (generated during kernel-build, never committed).
# We build our own kernel from pkgs@${PKGS_COMMIT} so that both the kernel and
# rknpu.ko share the same kernel-build cache layer — and therefore the same key.
# The custom imager embeds this kernel at /usr/install/arm64/vmlinuz.
# ---------------------------------------------------------------------------

build_kernel() {
    log "Building kernel (${KERNEL_VERSION}) for custom imager..."
    setup_pkgs_tree

    local image="${REGISTRY}/talos-rk3588-kernel:${KERNEL_VERSION}"

    # KERNEL_LOCAL_LOAD=true: load into local daemon instead of pushing to GHCR.
    # Used by build-installer.sh so it can docker-create the image locally without
    # requiring write access to the (pre-existing, unlinked) talos-rk3588-kernel
    # GHCR package.
    local output_flag="--push"
    local cache_to_args=("--cache-to" "type=registry,ref=${CACHE_REGISTRY}/kernel,mode=max")
    if [[ "${KERNEL_LOCAL_LOAD:-false}" == "true" ]]; then
        output_flag="--load"
        cache_to_args=()
        log "  (KERNEL_LOCAL_LOAD=true: loading into local daemon, skipping registry push and cache write)"
    fi

    docker buildx build \
        --builder "${BUILDER_NAME}" \
        --file "${PKGS_WORK_DIR}/pkgs/Pkgfile" \
        --target kernel \
        --platform linux/arm64 \
        --build-arg TAG="${BUILD_ARG_TAG}" \
        --build-arg PKGS="${PKGS_COMMIT}" \
        --cache-from "type=registry,ref=${CACHE_REGISTRY}/rockchip-rknpu" \
        --cache-from "type=registry,ref=${CACHE_REGISTRY}/kernel" \
        "${cache_to_args[@]}" \
        --tag "${image}" \
        "${output_flag}" \
        "${PKGS_WORK_DIR}/pkgs"

    if [[ "${KERNEL_LOCAL_LOAD:-false}" == "true" ]]; then
        log "Loaded into local daemon: ${image}"
    else
        log "Pushed: ${image}"
    fi
}

# ---------------------------------------------------------------------------
# rockchip-rknpu — kernel module, requires siderolabs/pkgs tree
# ---------------------------------------------------------------------------

build_rknpu() {
    log "Building rockchip-rknpu (driver ${RKNPU_VERSION}, kernel ${KERNEL_VERSION})..."
    setup_pkgs_tree

    local tag="${RKNPU_VERSION}-${KERNEL_VERSION}"
    local image="${REGISTRY}/rockchip-rknpu:${tag}"

    docker buildx build \
        --builder "${BUILDER_NAME}" \
        --file "${PKGS_WORK_DIR}/pkgs/Pkgfile" \
        --target rockchip-rknpu \
        --platform linux/arm64 \
        --build-arg TAG="${BUILD_ARG_TAG}" \
        --build-arg PKGS="${PKGS_COMMIT}" \
        --cache-from "type=registry,ref=${CACHE_REGISTRY}/kernel" \
        --cache-from "type=registry,ref=${CACHE_REGISTRY}/rockchip-rknpu" \
        --cache-to   "type=registry,ref=${CACHE_REGISTRY}/rockchip-rknpu,mode=max" \
        --tag "${image}" \
        --push \
        "${PKGS_WORK_DIR}/pkgs"

    log "Pushed: ${image}"
}

# ---------------------------------------------------------------------------
# rockchip-rknn-libs — librknnrt.so, built inside pkgs tree (needs base stage)
# ---------------------------------------------------------------------------

build_rknn_libs() {
    log "Building rockchip-rknn-libs (librknnrt ${RKNN_RUNTIME_VERSION}, kernel ${KERNEL_VERSION})..."
    setup_pkgs_tree

    local tag="${RKNN_RUNTIME_VERSION}-${KERNEL_VERSION}"
    local image="${REGISTRY}/rockchip-rknn-libs:${tag}"

    docker buildx build \
        --builder "${BUILDER_NAME}" \
        --file "${PKGS_WORK_DIR}/pkgs/Pkgfile" \
        --target rockchip-rknn-libs \
        --platform linux/arm64 \
        --build-arg TAG="${BUILD_ARG_TAG}" \
        --build-arg RKNN_RUNTIME_VERSION="${RKNN_RUNTIME_VERSION}" \
        --cache-from "type=registry,ref=${CACHE_REGISTRY}/rockchip-rknn-libs" \
        --cache-to   "type=registry,ref=${CACHE_REGISTRY}/rockchip-rknn-libs,mode=max" \
        --tag "${image}" \
        --push \
        "${PKGS_WORK_DIR}/pkgs"

    log "Pushed: ${image}"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

TARGET="${1:-all}"

case "${TARGET}" in
    kernel)     build_kernel ;;
    rknpu)      build_rknpu ;;
    rknn-libs)  build_rknn_libs ;;
    all)
        # build_kernel pushes a standalone talos-rk3588-kernel image that is
        # required by build-installer.sh.  It reuses the kernel-build cache
        # layer shared with build_rknpu, so no extra compile time is incurred
        # when both are run in the same workflow.
        build_kernel
        build_rknpu
        build_rknn_libs
        ;;
    *)
        echo "Usage: $0 [kernel|rknpu|rknn-libs|all]" >&2
        exit 1
        ;;
esac

log "Done. Images pushed to ${REGISTRY}"
