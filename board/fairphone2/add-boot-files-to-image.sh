#!/bin/bash

# Exit on any error
set -e

# Paths
IMAGE="${BINARIES_DIR}/sdcard.img"  # The image file
LOOP_DEVICE=""                      # Loop device placeholder
MOUNT_POINT="/tmp/rootfs"          # Temporary mount point

# Ensure the mount point exists
mkdir -p "${MOUNT_POINT}"

# Set up a loop device for the image
LOOP_DEVICE=$(sudo losetup --find --show -Pf "${IMAGE}")

# Mount the loop device
sudo mount "${LOOP_DEVICE}p1" "${MOUNT_POINT}"

UUID=$(sudo blkid -s UUID -o value "${LOOP_DEVICE}p1")

# adding partition mount info to /etc/fstab
echo "Adding partition mount info to /etc/fstab..."
touch /tmp/fstab
#echo "UUID=${UUID} / ext2 defaults 0 0" >> /tmp/fstab
echo "/dev/mmcblk0p20p1 / ext2 defaults 0 0" >> /tmp/fstab

# Copy files into the image
echo "Copying files into the image..."
sudo cp "${BINARIES_DIR}/rootfs.cpio.gz" /tmp/rootfs/boot/rootfs.cpio.gz
sudo cp -r "${BINARIES_DIR}/qcom" /tmp/rootfs/boot/
sudo cp /tmp/fstab /tmp/rootfs/etc/fstab
# Sync to ensure all writes are flushed
sync
# Unmount and detach the loop device
sudo umount "${MOUNT_POINT}"
sudo losetup -d "${LOOP_DEVICE}"

# Clean up the mount point
rm /tmp/fstab
rmdir "${MOUNT_POINT}"

echo "Files successfully copied to ${IMAGE}."

