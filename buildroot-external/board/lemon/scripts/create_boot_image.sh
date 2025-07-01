#!/bin/sh
set -e

# Paths from Buildroot environment variables
BOOT_IMG="${BINARIES_DIR}/boot.ext2"
BOOT_SIZE_MB=64
BOOT_STAGING="${BINARIES_DIR}/boot_staging"

echo "Creating boot.ext2 image without root..."

# Clean staging directory
rm -rf "$BOOT_STAGING"
mkdir -p "$BOOT_STAGING"

# Copy boot files (adjust paths if needed)
cp "${BINARIES_DIR}/zImage" "$BOOT_STAGING/"
cp "${BINARIES_DIR}/initramfs.gz" "$BOOT_STAGING/"
cp "${BINARIES_DIR}/qcom-msm8974pro-fairphone-fp2.dtb" "$BOOT_STAGING/"

# Copy extlinux directory from your board overlay
cp -r "${BR2_EXTERNAL_CITRONICS_PATH}/board/lemon/extlinux" "$BOOT_STAGING/"

# Create the ext2 image using genext2fs
genext2fs -b $((BOOT_SIZE_MB * 1024)) \
    -d "$BOOT_STAGING" \
    -L boot \
    "$BOOT_IMG"

echo "boot.ext2 created at $BOOT_IMG"
