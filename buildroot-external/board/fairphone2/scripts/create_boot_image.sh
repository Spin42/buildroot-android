#!/bin/sh
set -e

# Paths from Buildroot environment variables
BOOT_IMG="${BINARIES_DIR}/boot.ext2"
BOOT_SIZE_MB=64
BOOT_STAGING="${BINARIES_DIR}/boot_staging"

echo "Creating boot.ext2 image..."

# Clean staging directory
rm -rf "$BOOT_STAGING"
mkdir -p "$BOOT_STAGING"

# Copy boot files (adjust paths if needed)
cp "${BINARIES_DIR}/zImage" "$BOOT_STAGING/"
cp "${BINARIES_DIR}/initramfs.gz" "$BOOT_STAGING/"
cp "${BINARIES_DIR}/qcom-msm8974pro-fairphone-fp2.dtb" "$BOOT_STAGING/"

# Copy extlinux directory from your board overlay
cp -r "${BR2_EXTERNAL_FP2_PATH}/board/fairphone2/extlinux" "$BOOT_STAGING/"

# Create empty ext2 image
dd if=/dev/zero of="$BOOT_IMG" bs=1M count=$BOOT_SIZE_MB

# Format image as ext2 with label "boot"
mkfs.ext2 -L boot "$BOOT_IMG"

# Mount image
mkdir -p /tmp/bootmnt
sudo mount -o loop "$BOOT_IMG" /tmp/bootmnt

# Copy files into mounted image
sudo cp -a "$BOOT_STAGING/"* /tmp/bootmnt/

# Unmount image and clean
sudo umount /tmp/bootmnt
rmdir /tmp/bootmnt

echo "boot.ext2 created at $BOOT_IMG"
