#!/usr/bin/env bash
# Build a custom Talos installer image for RK3588 NPU.
#
# Problem: Talos enforces CONFIG_MODULE_SIG_FORCE=y — all kernel modules must be
# signed with the key that was used to build the running kernel.  The signing
# key generated during our bldr rebuild does NOT match the key embedded in the
# officially distributed siderolabs kernel binary.
#
# Solution:
#   1. Export the kernel binary from our bldr build (same PKGS_COMMIT → same
#      signing key that was used to sign rknpu.ko).
#   2. Create a custom installer-base image that swaps the siderolabs vmlinuz
#      with our own (both built from identical source; only the signing key differs).
#   3. Build the final installer via the Talos imager using our custom base.
#
# Usage:
#   REGISTRY=ghcr.io/schwankner ./scripts/build-installer.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=scripts/common.sh
source "${SCRIPT_DIR}/common.sh"

REPO_ROOT="$(dirname "${SCRIPT_DIR}")"
REGISTRY="${REGISTRY:-ghcr.io/schwankner}"
BUILD_ARG_TAG="${BUILD_ARG_TAG:-${KERNEL_VERSION}}"
CACHE_REGISTRY="${CACHE_REGISTRY:-${REGISTRY}/build-cache}"

INSTALLER_IMAGE="${REGISTRY}/talos-rk3588-npu-installer:v${TALOS_VERSION#v}"
INSTALLER_BASE_IMAGE="${REGISTRY}/talos-rk3588-npu-installer-base:v${TALOS_VERSION#v}"

log() { echo "[build-installer] $*"; }

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "ERROR: $1 is not installed or not in PATH" >&2
        exit 1
    fi
}

require_cmd docker
require_cmd curl

# ---------------------------------------------------------------------------
# Shared pkgs tree (same as build-extensions.sh)
# ---------------------------------------------------------------------------

PKGS_WORK_DIR=""

setup_pkgs_tree() {
    if [ -n "${PKGS_WORK_DIR}" ]; then
        return
    fi
    PKGS_WORK_DIR="$(mktemp -d)"
    # shellcheck disable=SC2064
    trap 'rm -rf "${PKGS_WORK_DIR}"' EXIT

    log "Downloading siderolabs/pkgs @ ${PKGS_COMMIT}..."
    curl -fsSL \
        "https://github.com/siderolabs/pkgs/archive/${PKGS_COMMIT}.tar.gz" \
        -o "${PKGS_WORK_DIR}/pkgs.tar.gz"
    mkdir -p "${PKGS_WORK_DIR}/pkgs"
    tar -xzf "${PKGS_WORK_DIR}/pkgs.tar.gz" \
        --strip-components=1 \
        -C "${PKGS_WORK_DIR}/pkgs"

    # Inject our extensions so bldr can resolve all stages in one tree
    cp -r "${REPO_ROOT}/rockchip-rknpu"     "${PKGS_WORK_DIR}/pkgs/rockchip-rknpu"
    cp -r "${REPO_ROOT}/rockchip-rknn-libs" "${PKGS_WORK_DIR}/pkgs/rockchip-rknn-libs"
}

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
# Step 1: Export the kernel binary from our pkgs build
# ---------------------------------------------------------------------------

log "Exporting kernel binary from pkgs @ ${PKGS_COMMIT}..."
setup_pkgs_tree

KERNEL_OUT="${PKGS_WORK_DIR}/kernel-out"
mkdir -p "${KERNEL_OUT}"

docker buildx build \
    --builder "${BUILDER_NAME}" \
    --file "${PKGS_WORK_DIR}/pkgs/Pkgfile" \
    --target kernel \
    --platform linux/arm64 \
    --build-arg TAG="${BUILD_ARG_TAG}" \
    --build-arg PKGS="${PKGS_COMMIT}" \
    --cache-from "type=registry,ref=${CACHE_REGISTRY}/rockchip-rknpu" \
    --output "type=local,dest=${KERNEL_OUT}" \
    "${PKGS_WORK_DIR}/pkgs"

log "Kernel output:"
find "${KERNEL_OUT}" -type f | sort

# Locate vmlinuz (look in common paths)
VMLINUZ=""
for candidate in \
    "${KERNEL_OUT}/usr/install/arm64/vmlinuz" \
    "${KERNEL_OUT}/boot/vmlinuz" \
    "${KERNEL_OUT}/boot/vmlinuz-arm64"; do
    if [ -f "${candidate}" ]; then
        VMLINUZ="${candidate}"
        break
    fi
done

if [ -z "${VMLINUZ}" ]; then
    echo "ERROR: Could not find vmlinuz in kernel output. Files found:" >&2
    find "${KERNEL_OUT}" -type f >&2
    exit 1
fi
log "Using vmlinuz: ${VMLINUZ}"

# ---------------------------------------------------------------------------
# Step 2: Build custom installer-base (swap siderolabs vmlinuz → ours)
# ---------------------------------------------------------------------------

log "Building custom installer-base ${INSTALLER_BASE_IMAGE}..."

# Determine the target path inside installer-base
VMLINUZ_DEST="/usr/install/arm64/vmlinuz"

# Build context: copy vmlinuz into a temp dir
INSTALLER_BASE_CTX="${PKGS_WORK_DIR}/installer-base-ctx"
mkdir -p "${INSTALLER_BASE_CTX}"
cp "${VMLINUZ}" "${INSTALLER_BASE_CTX}/vmlinuz"

cat > "${INSTALLER_BASE_CTX}/Dockerfile" <<DOCKERFILE
FROM ghcr.io/siderolabs/installer-base:v${TALOS_VERSION#v}

# Replace the siderolabs kernel (which has siderolabs' ephemeral signing key)
# with our own kernel built from the same PKGS_COMMIT source.
# Both kernels are functionally identical; only the module signing key differs.
# Our rknpu.ko is signed with our key -> must boot with our kernel.
COPY vmlinuz ${VMLINUZ_DEST}
DOCKERFILE

docker buildx build \
    --builder "${BUILDER_NAME}" \
    --platform linux/arm64 \
    --tag "${INSTALLER_BASE_IMAGE}" \
    --push \
    "${INSTALLER_BASE_CTX}"

log "Pushed installer-base: ${INSTALLER_BASE_IMAGE}"

# ---------------------------------------------------------------------------
# Step 3: Build final installer via Talos imager
# ---------------------------------------------------------------------------

log "Building installer with Talos imager..."

INSTALLER_OUT="${PKGS_WORK_DIR}/installer-out"
mkdir -p "${INSTALLER_OUT}"

docker run --rm \
    -v "${INSTALLER_OUT}:/out" \
    "ghcr.io/siderolabs/imager:v${TALOS_VERSION#v}" \
    installer \
    --arch arm64 \
    --base-installer-image "${INSTALLER_BASE_IMAGE}" \
    --overlay-image ghcr.io/siderolabs/sbc-rockchip:v0.2.0 \
    --overlay-name turingrk1 \
    --system-extension-image "ghcr.io/${REGISTRY#ghcr.io/}/rockchip-rknpu:${RKNPU_VERSION}-${KERNEL_VERSION}" \
    --system-extension-image "ghcr.io/${REGISTRY#ghcr.io/}/rockchip-rknn-libs:${RKNN_RUNTIME_VERSION}-${KERNEL_VERSION}" \
    2>&1

log "Loading and pushing installer to ${INSTALLER_IMAGE}..."
docker load -i "${INSTALLER_OUT}/installer-arm64.tar"
docker tag "ghcr.io/siderolabs/installer-base:v${TALOS_VERSION#v}" "${INSTALLER_IMAGE}"
docker push "${INSTALLER_IMAGE}"

log "Done! Installer pushed: ${INSTALLER_IMAGE}"
log ""
log "Upgrade node with:"
log "  talosctl upgrade --nodes <IP> --image ${INSTALLER_IMAGE} --preserve"
