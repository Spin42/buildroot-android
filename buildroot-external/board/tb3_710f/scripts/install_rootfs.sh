#!/usr/bin/env bash
# Install a root filesystem on the tablet's userdata partition (mmcblk0p7) and flash the boot image.
# The partition must not be mounted while it is written, so the tablet is first booted into the RAM rescue
# system. Needs the running tablet reachable over usb networking (10.0.42.1), or already in fastboot.
# Usage: install_rootfs.sh [images dir]   (default: buildroot output/images; needs boot.img, rootfs.ext2,
# and the rescue image, which is built from the same directory when missing)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
IMAGES=${1:-$HERE/../../../../output/images}
IP=${TB3_IP:-10.0.42.1}
SSHO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8"
r() { sshpass -p root ssh $SSHO root@$IP "$@"; }

ROOTFS=$IMAGES/rootfs.ext2
[ -f "$ROOTFS" ] && [ -f "$IMAGES/boot.img" ] || { echo "need rootfs.ext2 and boot.img in $IMAGES"; exit 1; }
[ -f "$IMAGES/boot-rescue.img" ] || "$HERE/mkrescue_image.sh" "$IMAGES"

echo "booting the rescue system..."
"$HERE/reflash.sh" "$IMAGES/boot-rescue.img"
r 'grep -q mmcblk0p7 /proc/mounts && { echo "userdata is mounted: not in rescue"; exit 1; }; true'

echo "writing $ROOTFS to /dev/mmcblk0p7..."
cat "$ROOTFS" | r 'dd of=/dev/mmcblk0p7 bs=1M conv=fsync 2>/dev/null'
want=$(sha256sum "$ROOTFS" | cut -d' ' -f1)
size=$(stat -c %s "$ROOTFS")
got=$(r "dd if=/dev/mmcblk0p7 bs=1M count=$((size / 1048576)) 2>/dev/null | sha256sum | cut -d' ' -f1")
[ "$want" = "$got" ] || { echo "read-back mismatch"; exit 1; }

echo "growing the filesystem (the last 21 MiB of the partition are the vendor bad-block pool)..."
r 'e2fsck -f -p /dev/mmcblk0p7 >/dev/null 2>&1; resize2fs /dev/mmcblk0p7 5309440K'

echo "flashing the kernel-only boot image..."
"$HERE/reflash.sh" "$IMAGES/boot.img"
echo "done"
