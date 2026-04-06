#!/usr/bin/env bash
# Build a custom Talos installer image for RK3588 NPU.
#
# Problem: Talos enforces CONFIG_MODULE_SIG_FORCE=y — all kernel modules must be
# signed with the key that was used to build the running kernel.  The signing
# key generated during our bldr rebuild does NOT match the key embedded in the
# officially distributed siderolabs kernel binary.
#
# Root cause: the Talos imager (ghcr.io/siderolabs/imager) has the siderolabs
# kernel binary embedded at /usr/install/arm64/vmlinuz inside the imager image
# itself.  The --base-installer-image flag only controls the installer binary
# (grub, installer script) — the imager ignores any vmlinuz placed in the
# base installer image.
#
# Solution:
#   1. Export the kernel binary from our bldr build (same PKGS_COMMIT → same
#      signing key that was used to sign rknpu.ko).
#   2. Create a custom imager image that replaces the siderolabs vmlinuz with
#      ours (FROM siderolabs/imager + COPY our vmlinuz).
#   3. Build the final installer via our custom imager WITHOUT sbc-rockchip
#      overlay.  The no-overlay installer does not repartition the eMMC on
#      upgrade (Bug 13), so STATE is preserved and the node rejoins the cluster.
#      Existing U-Boot and DTBs remain on the eMMC from the last board-support
#      install; a DTB overlay in the rknpu extension will add the NPU DT nodes.
#
# Usage:
#   REGISTRY=ghcr.io/schwankner ./scripts/build-installer.sh
#
# Environment variables:
#   CONTAINER_RUNTIME  Override container runtime (default: podman; use docker in CI)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=scripts/common.sh
source "${SCRIPT_DIR}/common.sh"

REPO_ROOT="$(dirname "${SCRIPT_DIR}")"
REGISTRY="${REGISTRY:-ghcr.io/schwankner}"
BUILD_ARG_TAG="${BUILD_ARG_TAG:-${KERNEL_VERSION}}"
CACHE_REGISTRY="${CACHE_REGISTRY:-${REGISTRY}/build-cache}"
CONTAINER_RUNTIME="${CONTAINER_RUNTIME:-podman}"

# The final installer image reuses the existing installer-base package to avoid
# GHCR org-level restrictions on creating brand-new packages.
INSTALLER_IMAGE="${REGISTRY}/talos-rk3588-npu-installer-base:installer-v${TALOS_VERSION#v}"

log() { echo "[build-installer] $*"; }

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "ERROR: $1 is not installed or not in PATH" >&2
        exit 1
    fi
}

require_cmd "${CONTAINER_RUNTIME}"
require_cmd curl

# ---------------------------------------------------------------------------
# Shared pkgs tree (same as build-extensions.sh)
# ---------------------------------------------------------------------------

PKGS_WORK_DIR=""

setup_pkgs_tree() {
    if [ -n "${PKGS_WORK_DIR}" ]; then
        return
    fi
    # Use /private/tmp so docker volume mounts work with colima — colima only
    # mounts /private/tmp via virtiofs (rw), not the macOS /var/folders path
    # that mktemp -d uses by default.
    PKGS_WORK_DIR="/private/tmp/talos-build-work-$$"
    mkdir -p "${PKGS_WORK_DIR}"
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
# buildx builder setup (docker only; podman uses native buildah backend)
# ---------------------------------------------------------------------------

BUILDER_NAME="talos-rk3588-builder"
if [ "${CONTAINER_RUNTIME}" = "docker" ]; then
    if ! docker buildx inspect "${BUILDER_NAME}" >/dev/null 2>&1; then
        log "Creating buildx builder ${BUILDER_NAME}..."
        docker buildx create --name "${BUILDER_NAME}" --driver docker-container --use
    else
        docker buildx use "${BUILDER_NAME}"
    fi
    BUILDX_CMD="docker buildx build --builder ${BUILDER_NAME}"
else
    BUILDX_CMD="podman build"
fi

# ---------------------------------------------------------------------------
# Step 1: Export the kernel binary from our pkgs build
# ---------------------------------------------------------------------------

log "Exporting kernel binary from pkgs @ ${PKGS_COMMIT}..."
setup_pkgs_tree

KERNEL_OUT="${PKGS_WORK_DIR}/kernel-out"
mkdir -p "${KERNEL_OUT}"

# shellcheck disable=SC2086
${BUILDX_CMD} \
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
# Step 2: Build custom imager (replace siderolabs vmlinuz → ours)
#
# The siderolabs imager image has the kernel at /usr/install/arm64/vmlinuz.
# The --base-installer-image flag does NOT affect which kernel is used for
# the UKI build — the imager always uses its own embedded vmlinuz.
# We must therefore replace the vmlinuz INSIDE the imager image.
# ---------------------------------------------------------------------------

log "Building custom imager with our kernel (replaces siderolabs vmlinuz)..."

CUSTOM_IMAGER_CTX="${PKGS_WORK_DIR}/custom-imager-ctx"
mkdir -p "${CUSTOM_IMAGER_CTX}"
cp "${VMLINUZ}" "${CUSTOM_IMAGER_CTX}/vmlinuz"

cat > "${CUSTOM_IMAGER_CTX}/Dockerfile" <<DOCKERFILE
FROM ghcr.io/siderolabs/imager:v${TALOS_VERSION#v}

# Replace the siderolabs kernel (which has siderolabs' ephemeral signing key)
# with our own kernel built from the same PKGS_COMMIT source.
# The imager reads /usr/install/arm64/vmlinuz to build the UKI — this is the
# correct location, NOT the --base-installer-image.
COPY vmlinuz /usr/install/arm64/vmlinuz
DOCKERFILE

if [ "${CONTAINER_RUNTIME}" = "docker" ]; then
    # Build locally (no push needed — we run the imager immediately)
    docker buildx build \
        --builder "${BUILDER_NAME}" \
        --platform linux/arm64 \
        --tag "custom-imager:${TALOS_VERSION#v}" \
        --load \
        "${CUSTOM_IMAGER_CTX}"
    CUSTOM_IMAGER_REF="custom-imager:${TALOS_VERSION#v}"
else
    podman build \
        --platform linux/arm64 \
        --tag "custom-imager:${TALOS_VERSION#v}" \
        "${CUSTOM_IMAGER_CTX}"
    CUSTOM_IMAGER_REF="custom-imager:${TALOS_VERSION#v}"
fi

log "Custom imager ready: ${CUSTOM_IMAGER_REF}"

# ---------------------------------------------------------------------------
# Step 3: Build UKI with extensions embedded (NO overlay)
#
# Running the imager WITHOUT --overlay-image causes the standard Talos code
# path, which embeds the extension squashfs (and schematic) inside the UKI
# initramfs.  This is the correct and complete installer — we do not need the
# sbc-rockchip overlay because:
#
#   a) The existing U-Boot and board DTBs are already on the eMMC from the
#      initial Talos installation with the turingrk1 overlay.  talosctl upgrade
#      does not touch the bootloader partition when there is no overlay installer.
#   b) Without an overlay installer, talosctl upgrade does NOT repartition the
#      eMMC (Bug 13), so the STATE partition (machine config, cluster secrets)
#      is preserved across upgrades.
#   c) NPU device-tree nodes will be added via a DTB overlay in the rknpu
#      extension rather than relying on the sbc-rockchip rknn.patch (Bug 7).
# ---------------------------------------------------------------------------

log "Building UKI installer with extensions (no overlay)..."

INSTALLER_OUT="${PKGS_WORK_DIR}/installer-out"
mkdir -p "${INSTALLER_OUT}"

"${CONTAINER_RUNTIME}" run --rm \
    -v "${INSTALLER_OUT}:/out" \
    "${CUSTOM_IMAGER_REF}" \
    installer \
    --arch arm64 \
    --system-extension-image "ghcr.io/${REGISTRY#ghcr.io/}/rockchip-rknpu:${RKNPU_VERSION}-${KERNEL_VERSION}" \
    --system-extension-image "ghcr.io/${REGISTRY#ghcr.io/}/rockchip-rknn-libs:${RKNN_RUNTIME_VERSION}-${KERNEL_VERSION}" \
    --extra-kernel-arg cma=128MB \
    --extra-kernel-arg console=ttyS9,115200 \
    --extra-kernel-arg console=ttyS2,115200 \
    --extra-kernel-arg sysctl.kernel.kexec_load_disabled=1 \
    --extra-kernel-arg talos.dashboard.disabled=1 \
    2>&1

log "Loading installer image..."
INSTALLER_LOAD=$("${CONTAINER_RUNTIME}" load -i "${INSTALLER_OUT}/installer-arm64.tar" 2>&1)
echo "${INSTALLER_LOAD}"
INSTALLER_LOCAL_IMAGE=$(echo "${INSTALLER_LOAD}" | grep -oE 'Loaded image[^:]*: \S+' | awk '{print $NF}' | head -1)
if [ -z "${INSTALLER_LOCAL_IMAGE}" ]; then
    echo "ERROR: Could not determine loaded image name from: ${INSTALLER_LOAD}" >&2
    exit 1
fi
INSTALLER_LOCAL_REF="npu-installer:${TALOS_VERSION#v}"
"${CONTAINER_RUNTIME}" tag "${INSTALLER_LOCAL_IMAGE}" "${INSTALLER_LOCAL_REF}"
log "Installer tagged: ${INSTALLER_LOCAL_REF}"

log "Pushing installer to ${INSTALLER_IMAGE}..."
# Tag and push to an EXISTING package (installer-base) with a new tag — this
# avoids GHCR org-level restrictions that block creating new packages via
# GITHUB_TOKEN.
"${CONTAINER_RUNTIME}" tag "${INSTALLER_LOCAL_REF}" "${INSTALLER_IMAGE}"
"${CONTAINER_RUNTIME}" push "${INSTALLER_IMAGE}"

log "Done! Installer pushed: ${INSTALLER_IMAGE}"
log ""
log "Upgrade node with:"
log "  talosctl upgrade --nodes <IP> --image ${INSTALLER_IMAGE} --preserve"
