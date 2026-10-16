#!/bin/bash
# Build the network flasher initramfs from a Fairphone 2 Buildroot output and
# put it, with that output's kernel and device tree, in a TFTP directory.
# Usage: mkrescue.sh <buildroot output dir> <tftp dir> [dtb name]
set -eu
O=$(realpath "$1")
DST=$(realpath "$2")
DTB=${3:-qcom-msm8974pro-fairphone-fp2.dtb}
HERE=$(dirname "$(realpath "$0")")
K=$O/build/linux-custom
REL=$(cat "$K/include/config/kernel.release")

BBCONF=$(ls "$O"/build/busybox-*/.config)
for a in GUNZIP UNLZOP GETTY SETSID UDHCPC IP MD5SUM; do
	grep -q "^CONFIG_$a=y" "$BBCONF" || { echo "busybox lacks $a" >&2; exit 1; }
done

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W"/{bin,lib/modules,proc,sys,tmp}
cp "$O/target/bin/busybox" "$W/bin/"
ln -s busybox "$W/bin/sh"
for l in ld-linux-armhf.so.3 libc.so.6 libm.so.6 libresolv.so.2; do
	[ -e "$O/target/lib/$l" ] && cp -L "$O/target/lib/$l" "$W/lib/"
done
for m in power/supply/qcom_smbb net/mii net/usb/usbnet net/usb/smsc95xx; do
	f=$O/target/lib/modules/$REL/kernel/drivers/$m.ko
	[ -e "$f" ] && cp "$f" "$W/lib/modules/"
done
CC=$(ls "$O"/host/bin/*-linux-gnueabihf-gcc | head -1)
"$CC" -O2 -Wall -o "$W/bin/netrecv" "$HERE/netrecv.c"
install -m 755 "$HERE/init" "$W/init"
install -m 755 "$HERE/udhcpc.script" "$W/udhcpc.script"

# gen_initramfs.sh adds /dev/console and /dev/kmsg without needing root.
mkdir -p "$DST"
(cd "$K" && usr/gen_initramfs.sh -u "$(id -u)" -g "$(id -g)" "$W" "$HERE/console.list") |
	gzip -9 > "$DST/netflash.cpio.gz"
cp "$O/images/zImage" "$DST/zImage"
cp "$O/images/$DTB" "$DST/netflash.dtb"
ls -l "$DST/netflash.cpio.gz" "$DST/zImage" "$DST/netflash.dtb"
