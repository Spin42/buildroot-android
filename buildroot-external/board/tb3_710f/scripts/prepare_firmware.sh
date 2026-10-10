#!/usr/bin/env bash
# Collect the proprietary firmware of the TB3-710F into board/tb3_710f/firmware (git-ignored).
# usage: prepare_firmware.sh <stock kernel source dir> <stock /system/etc/firmware dir>
#   stock kernel source: the 3.10.54 tree (it holds the Silead touch firmware as C arrays)
#   stock firmware:      the CONSYS files extracted from the stock system partition
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
KERNEL=${1:?stock kernel source dir}
STOCK=${2:?stock firmware dir}
OUT=$HERE/../firmware

mkdir -p "$OUT/silead"
"$HERE/extract_silead_fw.py" \
	"$KERNEL/drivers/input/touchscreen/mediatek/GSL3670/gsl_ts_fw.h" GSLX68X_FW_2 \
	"$OUT/silead/gsl3670-tb3-710f.fw"
for f in ROMv2_patch_1_0_hdr.bin ROMv2_patch_1_1_hdr.bin WIFI_RAM_CODE_8127 WMT_SOC.cfg; do
	install -m 0644 "$STOCK/$f" "$OUT/$f"
done
echo "firmware ready in $OUT"
