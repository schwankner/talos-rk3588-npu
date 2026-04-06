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

# ---------------------------------------------------------------------------
# DTB patch: replace per-core rknn-core nodes with vendor rknpu node
#
# The sbc-rockchip DTB ships with three per-core nodes
# (compatible = "rockchip,rk3588-rknn-core") for the mainline rocket driver.
# The vendor w568w/rknpu-module driver expects a single unified node
# (compatible = "rockchip,rk3588-rknpu") covering all three cores.  We patch
# the DTB in the installer so the correct node is on disk after talosctl upgrade.
#
# Phandle values are numeric (the DTB has no __symbols__); they are derived
# from the sbc-rockchip v0.2.0 rk3588-turing-rk1.dtb and must be re-verified
# if the sbc-rockchip version is updated.
# ---------------------------------------------------------------------------

log "Extracting rk3588-turing-rk1.dtb from overlay installer..."
DTB_EXTRACT_CID=$("${CONTAINER_RUNTIME}" create "${OVERLAY_REF}")
"${CONTAINER_RUNTIME}" cp \
    "${DTB_EXTRACT_CID}:/overlay/artifacts/arm64/dtb/rockchip/rk3588-turing-rk1.dtb" \
    "${COMBINED_CTX}/rk3588-turing-rk1.dtb"
"${CONTAINER_RUNTIME}" rm "${DTB_EXTRACT_CID}"
chmod 644 "${COMBINED_CTX}/rk3588-turing-rk1.dtb"

# Write the Python DTS patching script into the build context.
cat > "${COMBINED_CTX}/patch_dtb.py" << 'PYEOF'
#!/usr/bin/env python3
"""
Patch rk3588-turing-rk1 DTS: replace per-core rknn-core NPU nodes with
a single vendor rknpu node so the w568w/rknpu-module driver binds.

Phandle values derived from rk3588-turing-rk1.dtb (sbc-rockchip v0.2.0):
  scmi_clk (prot@14) = 0x0a, NPU SCMI clock ID = 0x06
  cru phandle = 0x21, core0 srst_a reset ID = 0x110
  power-controller = 0x22: NPUTOP=0x09, NPU1=0x0a, NPU2=0x0b
  iommu phandles: fdab9000=0x66, fdac9000=0x67, fdad9000=0x68
  npu-supply = 0x36 (vdd_npu_s0)
  interrupts: SPI 0x6e/0x6f/0x70 (110/111/112), level-high (0x04)
"""
import sys

NEW_NODE = (
    '\n'
    '\trknpu: rknpu@fdab0000 {\n'
    '\t\tcompatible = "rockchip,rk3588-rknpu", "rockchip,rknpu";\n'
    '\t\treg = <0x00 0xfdab0000 0x00 0x9000\n'
    '\t\t       0x00 0xfdac0000 0x00 0x9000\n'
    '\t\t       0x00 0xfdad0000 0x00 0x9000>;\n'
    '\t\treg-names = "rknpu_core0", "rknpu_core1", "rknpu_core2";\n'
    '\t\tinterrupts = <0x00 0x6e 0x04 0x00\n'
    '\t\t              0x00 0x6f 0x04 0x00\n'
    '\t\t              0x00 0x70 0x04 0x00>;\n'
    '\t\tinterrupt-names = "npu_irq0", "npu_irq1", "npu_irq2";\n'
    '\t\tclocks = <0x0a 0x06>;\n'
    '\t\tclock-names = "clk_npu";\n'
    '\t\tassigned-clocks = <0x0a 0x06>;\n'
    '\t\tassigned-clock-rates = <0xbebc200>;\n'
    '\t\tresets = <0x21 0x110>;\n'
    '\t\treset-names = "srst_a";\n'
    '\t\tpower-domains = <0x22 0x09 0x22 0x0a 0x22 0x0b>;\n'
    '\t\tpower-domain-names = "nputop", "npu1", "npu2";\n'
    '\t\tiommus = <0x66 0x67 0x68>;\n'
    '\t\tnpu-supply = <0x36>;\n'
    '\t\tsram-supply = <0x36>;\n'
    '\t\tstatus = "okay";\n'
    '\t};\n'
)


def process_node(dts, node_name, disable=False, insert_after=None):
    lines = dts.split('\n')
    result = []
    in_node = False
    depth = 0
    node_marker = '\t' + node_name + ' {'

    for line in lines:
        if not in_node:
            if line.rstrip() == node_marker:
                in_node = True
                depth = 1
                result.append(line)
            else:
                result.append(line)
        else:
            depth += line.count('{') - line.count('}')
            if disable and 'status = "okay"' in line:
                line = line.replace('status = "okay"', 'status = "disabled"')
            result.append(line)
            if depth <= 0:
                in_node = False
                if insert_after is not None:
                    result.append(insert_after)

    return '\n'.join(result)


if __name__ == '__main__':
    dts = sys.stdin.read()

    for addr in ['npu@fdab0000', 'npu@fdac0000', 'npu@fdad0000']:
        dts = process_node(dts, addr, disable=True)

    dts = process_node(dts, 'iommu@fdad9000', insert_after=NEW_NODE)

    sys.stdout.write(dts)
PYEOF

log "Patching rk3588-turing-rk1.dtb (disabling rknn-core nodes, adding rknpu node)..."
# Run dtc + python3 inside a container so no host toolchain is required.
# dtc is architecture-independent: it produces the same binary DTB regardless
# of the host CPU.  alpine:3.21 is used to avoid 'latest' tag drift.
"${CONTAINER_RUNTIME}" run --rm \
    --platform linux/amd64 \
    -v "${COMBINED_CTX}:/work" \
    alpine:3.21 \
    sh -c "apk add --quiet dtc python3 && \
           dtc -I dtb -O dts /work/rk3588-turing-rk1.dtb 2>/dev/null | \
           python3 /work/patch_dtb.py | \
           dtc -W no-unique_unit_address -W no-clocks_property \
               -W no-cooling_device_property \
               -I dts -O dtb -o /work/rk3588-turing-rk1-patched.dtb 2>/dev/null"
log "DTB patched: $(du -sh "${COMBINED_CTX}/rk3588-turing-rk1-patched.dtb" | cut -f1)"

COMBINED_REF="combined-installer:${TALOS_VERSION#v}"
if [ "${CONTAINER_RUNTIME}" = "docker" ]; then
    # docker buildx uses an isolated image store (docker-container driver) and
    # cannot see images loaded into the Docker daemon via `docker load`.  Use
    # `docker commit` against the daemon directly so no registry round-trip is
    # needed and no buildx image-store isolation issue arises.
    PATCH_CID=$(docker create "${OVERLAY_REF}")
    docker cp "${COMBINED_CTX}/vmlinuz.efi" "${PATCH_CID}:/usr/install/arm64/vmlinuz.efi"
    docker cp "${COMBINED_CTX}/rk3588-turing-rk1-patched.dtb" \
        "${PATCH_CID}:/overlay/artifacts/arm64/dtb/rockchip/rk3588-turing-rk1.dtb"
    docker commit "${PATCH_CID}" "${COMBINED_REF}"
    docker rm "${PATCH_CID}"
else
    cat > "${COMBINED_CTX}/Dockerfile" <<DOCKERFILE
FROM ${OVERLAY_REF}
# Replace the bare overlay-installer UKI (no extensions in initramfs) with the
# one built in Pass 1 (standard imager path, extensions embedded as squashfs).
COPY vmlinuz.efi /usr/install/arm64/vmlinuz.efi
# Replace the sbc-rockchip DTB with the patched version that has the vendor
# rknpu node (replaces per-core rknn-core nodes for w568w/rknpu-module).
COPY rk3588-turing-rk1-patched.dtb /overlay/artifacts/arm64/dtb/rockchip/rk3588-turing-rk1.dtb
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
