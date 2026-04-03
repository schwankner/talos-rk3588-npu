#!/usr/bin/env bash
# Wrap the UKI into a flashable raw disk image.
# Usage: BOARD=turing-rk1 ./build-usb-image.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

BOARD="${BOARD:-turing-rk1}"
UKI_PATH="${DIST}/uki-${BOARD}/metal-arm64-uki.efi"
OUTPUT="${DIST}/talos-${BOARD}.raw"

if [[ ! -f "${UKI_PATH}" ]]; then
  echo "ERROR: UKI not found at ${UKI_PATH} — run build-uki.sh first"
  exit 1
fi

echo "Wrapping ${UKI_PATH} into ${OUTPUT}"
mkdir -p "${DIST}"

# TODO: create FAT32 MBR disk image with UKI at EFI/BOOT/BOOTAA64.EFI
# macOS: hdiutil  |  Linux: losetup + mkfs.fat
