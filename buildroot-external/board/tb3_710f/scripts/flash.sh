#!/usr/bin/env bash
# Flash a boot image to the tablet's boot partition, addressing it only by serial.
# Usage: flash.sh [image]   (default: buildroot/output/images/boot.img)
# Refuses if any other device is visible to fastboot (the host usually has other phones attached),
# and checks the bootloader reports the TB3-710F before writing.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SER=${TB3_SERIAL:-PJPV59BIIBKJQOZ9}
PRODUCT=HQ8127_TB_B2B_L
IMG=${1:-$HERE/../../../../buildroot/output/images/boot.img}

[ -f "$IMG" ] || { echo "no such image: $IMG"; exit 1; }
adb -s "$SER" get-state >/dev/null 2>&1 && adb -s "$SER" reboot bootloader
for i in $(seq 1 30); do fastboot devices | grep -q "^$SER" && break; sleep 2; done
others=$(fastboot devices | awk '{print $1}' | grep -v "^$SER" || true)
[ -z "$others" ] || { echo "REFUSING: other fastboot device(s) present: $others"; exit 1; }
prod=$(fastboot -s "$SER" getvar product 2>&1 | awk '/product:/{print $2}')
[ "$prod" = "$PRODUCT" ] || { echo "REFUSING: product is '$prod', expected $PRODUCT"; exit 1; }
fastboot -s "$SER" flash boot "$IMG"
fastboot -s "$SER" continue
