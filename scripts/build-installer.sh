#!/usr/bin/env bash
# Build a custom Talos installer image for RK3588 NPU.
#
# The sbc-rockchip overlay installer (v0.2.0) intercepts --system-extension-image
# flags and does NOT embed the extension squashfs files inside the UKI initramfs
# (Bug 14).  We work around this with a two-pass build:
#
#   Pass 1 (no overlay, with extensions):
#     The standard Talos imager code path embeds extension squashfs files inside
#     the UKI initramfs.  This produces a vmlinuz.efi with the correct extensions.
#
#   Pass 2 (with overlay, no extensions):
#     The sbc-rockchip overlay installer writes U-Boot, DTBs, and configures GRUB
#     for the turingrk1 board layout.  Without this, talosctl upgrade writes the
#     kernel to the wrong EFI partition path and the node hangs at GRUB.
#
#   Combine:
#     Take the board-support artifacts from Pass 2 (GRUB, U-Boot, DTBs) and
#     replace its bare vmlinuz.efi with the extension-bearing one from Pass 1.
#
# No custom kernel is required.  The module signing key in rknpu.ko is the same
# as the key baked into the siderolabs kernel binary: both are built from the
# same siderolabs/pkgs commit (a92bed5), where certs/signing_key.pem is
# committed to the repository (not generated at build time).
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

REGISTRY="${REGISTRY:-ghcr.io/schwankner}"
CONTAINER_RUNTIME="${CONTAINER_RUNTIME:-podman}"

# The final installer image reuses the existing installer-base package to avoid
# GHCR org-level restrictions on creating brand-new packages.
INSTALLER_IMAGE="${REGISTRY}/talos-rk3588-npu-installer-base:installer-v${TALOS_VERSION#v}"

# Use /private/tmp so docker volume mounts work with colima — colima only
# mounts /private/tmp via virtiofs (rw), not the macOS /var/folders path
# that mktemp -d uses by default.
WORK_DIR="/private/tmp/talos-build-work-$$"
mkdir -p "${WORK_DIR}"
trap 'rm -rf "${WORK_DIR}"' EXIT

log() { echo "[build-installer] $*"; }

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "ERROR: $1 is not installed or not in PATH" >&2
        exit 1
    fi
}

require_cmd "${CONTAINER_RUNTIME}"

IMAGER_REF="ghcr.io/siderolabs/imager:v${TALOS_VERSION#v}"

# ---------------------------------------------------------------------------
# Pass 1: Build UKI with extensions embedded (no overlay)
#
# The standard imager path embeds all --system-extension-image squashfs files
# and the schematic inside the UKI initramfs CPIO.  The sbc-rockchip overlay
# intercepts these flags and discards them (Bug 14), so we must run without
# --overlay-image to get the correct initramfs.
# ---------------------------------------------------------------------------

log "Pass 1: building UKI with extensions (no overlay)..."

UKI_EXT_OUT="${WORK_DIR}/uki-ext-out"
mkdir -p "${UKI_EXT_OUT}"

"${CONTAINER_RUNTIME}" run --rm \
    -v "${UKI_EXT_OUT}:/out" \
    "${IMAGER_REF}" \
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

log "Loading Pass 1 installer image..."
UKI_EXT_LOAD=$("${CONTAINER_RUNTIME}" load -i "${UKI_EXT_OUT}/installer-arm64.tar" 2>&1)
echo "${UKI_EXT_LOAD}"
UKI_EXT_IMAGE=$(echo "${UKI_EXT_LOAD}" | grep -oE 'Loaded image[^:]*: \S+' | awk '{print $NF}' | head -1)
if [ -z "${UKI_EXT_IMAGE}" ]; then
    echo "ERROR: Could not determine loaded image name from: ${UKI_EXT_LOAD}" >&2
    exit 1
fi
UKI_EXT_REF="uki-ext-installer:${TALOS_VERSION#v}"
"${CONTAINER_RUNTIME}" tag "${UKI_EXT_IMAGE}" "${UKI_EXT_REF}"
log "Pass 1 installer tagged: ${UKI_EXT_REF}"

# ---------------------------------------------------------------------------
# Pass 2: Build overlay installer (no extensions)
#
# Produces the board-support artifacts (U-Boot, DTBs, GRUB config) that the
# turingrk1 board requires.  The vmlinuz.efi produced here has no extensions
# in its initramfs; it will be replaced with the Pass 1 UKI in the next step.
# ---------------------------------------------------------------------------

log "Pass 2: building overlay installer (no extensions)..."

OVERLAY_OUT="${WORK_DIR}/overlay-out"
mkdir -p "${OVERLAY_OUT}"

"${CONTAINER_RUNTIME}" run --rm \
    -v "${OVERLAY_OUT}:/out" \
    "${IMAGER_REF}" \
    installer \
    --arch arm64 \
    --overlay-image ghcr.io/siderolabs/sbc-rockchip:v0.2.0 \
    --overlay-name turingrk1 \
    2>&1

log "Loading Pass 2 installer image..."
OVERLAY_LOAD=$("${CONTAINER_RUNTIME}" load -i "${OVERLAY_OUT}/installer-arm64.tar" 2>&1)
echo "${OVERLAY_LOAD}"
OVERLAY_IMAGE=$(echo "${OVERLAY_LOAD}" | grep -oE 'Loaded image[^:]*: \S+' | awk '{print $NF}' | head -1)
if [ -z "${OVERLAY_IMAGE}" ]; then
    echo "ERROR: Could not determine loaded image name from: ${OVERLAY_LOAD}" >&2
    exit 1
fi
OVERLAY_REF="overlay-installer:${TALOS_VERSION#v}"
"${CONTAINER_RUNTIME}" tag "${OVERLAY_IMAGE}" "${OVERLAY_REF}"
log "Pass 2 overlay installer tagged: ${OVERLAY_REF}"

# ---------------------------------------------------------------------------
# Combine: overlay installer base + extension-bearing UKI
#
# Extract vmlinuz.efi from Pass 1 (extensions embedded in initramfs CPIO) and
# replace the bare vmlinuz.efi in the Pass 2 image with it.
# ---------------------------------------------------------------------------

log "Combining: replacing vmlinuz.efi in overlay installer with extension-bearing UKI..."

COMBINED_CTX="${WORK_DIR}/combined-ctx"
mkdir -p "${COMBINED_CTX}"

# Extract vmlinuz.efi by creating a temporary container.
# chmod 644: the in-container file mode is 0400 (Talos installer images); the
# subsequent docker cp into the patch container would fail with "permission denied".
EXTRACT_CID=$("${CONTAINER_RUNTIME}" create "${UKI_EXT_REF}")
"${CONTAINER_RUNTIME}" cp "${EXTRACT_CID}:/usr/install/arm64/vmlinuz.efi" "${COMBINED_CTX}/vmlinuz.efi"
"${CONTAINER_RUNTIME}" rm "${EXTRACT_CID}"
chmod 644 "${COMBINED_CTX}/vmlinuz.efi"
log "Extracted vmlinuz.efi ($(du -sh "${COMBINED_CTX}/vmlinuz.efi" | cut -f1)) from ${UKI_EXT_REF}"

COMBINED_REF="combined-installer:${TALOS_VERSION#v}"
if [ "${CONTAINER_RUNTIME}" = "docker" ]; then
    # docker buildx uses an isolated image store (docker-container driver) and
    # cannot see images loaded into the Docker daemon via `docker load`.  Use
    # `docker commit` against the daemon directly so no registry round-trip is
    # needed and no buildx image-store isolation issue arises.
    PATCH_CID=$(docker create "${OVERLAY_REF}")
    docker cp "${COMBINED_CTX}/vmlinuz.efi" "${PATCH_CID}:/usr/install/arm64/vmlinuz.efi"
    docker commit "${PATCH_CID}" "${COMBINED_REF}"
    docker rm "${PATCH_CID}"
else
    cat > "${COMBINED_CTX}/Dockerfile" <<DOCKERFILE
FROM ${OVERLAY_REF}
# Replace the bare overlay-installer UKI (no extensions in initramfs) with the
# one built in Pass 1 (standard imager path, extensions embedded as squashfs).
COPY vmlinuz.efi /usr/install/arm64/vmlinuz.efi
DOCKERFILE
    podman build \
        --platform linux/arm64 \
        --tag "${COMBINED_REF}" \
        "${COMBINED_CTX}"
fi
log "Combined installer built: ${COMBINED_REF}"

log "Pushing installer to ${INSTALLER_IMAGE}..."
# Tag and push to an EXISTING package (installer-base) with a new tag — this
# avoids GHCR org-level restrictions that block creating new packages via
# GITHUB_TOKEN.
"${CONTAINER_RUNTIME}" tag "${COMBINED_REF}" "${INSTALLER_IMAGE}"
"${CONTAINER_RUNTIME}" push "${INSTALLER_IMAGE}"

log "Done! Installer pushed: ${INSTALLER_IMAGE}"
log ""
log "Upgrade node with:"
log "  talosctl upgrade --nodes <IP> --image ${INSTALLER_IMAGE} --preserve"
log ""
log "Note: The sbc-rockchip overlay installer repartitions the eMMC (Bug 13),"
log "wiping STATE. After upgrade, the node enters maintenance mode and gets"
log "a DHCP address on the management network (10.0.70.x). Apply the machine"
log "config with: talosctl apply-config --insecure --nodes <10.0.70.x> -f worker2.yaml"
