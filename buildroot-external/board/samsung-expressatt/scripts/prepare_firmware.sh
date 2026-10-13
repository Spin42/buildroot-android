#!/usr/bin/env bash
# Extract this Express phone's WCNSS firmware and private calibration without mounting backups.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PORT_DIR=$(cd "$HERE/../../../../.." && pwd)
BACKUPS=$(realpath -- "${1:-$PORT_DIR/backups}")
OUT=${2:-$HERE/../overlay/lib/firmware}
[ "$#" -le 2 ] || { echo "Usage: $0 [BACKUPS_DIRECTORY [OUTPUT_DIRECTORY]]" >&2; exit 2; }
for tool in debugfs sha256sum python3; do command -v "$tool" >/dev/null; done
for image in system.img persist.img; do
    line=$(awk -v image="$image" '$2==image {print}' "$BACKUPS/SHA256SUMS")
    [ "$(grep -c . <<< "$line")" = 1 ] || { echo "missing or duplicate checksum for $image" >&2; exit 1; }
    (cd "$BACKUPS" && sha256sum --check --strict - <<< "$line")
done
mkdir -p "$OUT"
OUT=$(realpath -- "$OUT")
STAGE=$(mktemp -d "$OUT/.extract.XXXXXXXX")
trap 'rm -r -- "$STAGE"' EXIT
mkdir -p "$STAGE/wlan/prima"
files=(wcnss.mdt wcnss.b00 wcnss.b01 wcnss.b02 wcnss.b04)
for file in "${files[@]}"; do
    debugfs -R "dump /etc/firmware/$file \"$STAGE/$file\"" "$BACKUPS/system.img"
    [ -s "$STAGE/$file" ] || { echo "empty firmware: $file" >&2; exit 1; }
done
(cd "$STAGE" && sha256sum --check --strict "$HERE/wcnss-system.sha256")
[ "$(stat -c %s "$STAGE/wcnss.mdt")" = 6752 ] || { echo "unexpected MDT size" >&2; exit 1; }
cmp "$STAGE/wcnss.b01" <(tail -c 6540 "$STAGE/wcnss.mdt")
nv=wlan/prima/WCNSS_qcom_wlan_nv.bin
debugfs -R "dump /WCNSS_qcom_wlan_nv.bin \"$STAGE/raw-nv.bin\"" "$BACKUPS/persist.img"
python3 "$HERE/project_legacy_nv.py" "$STAGE/raw-nv.bin" "$STAGE/$nv"
files+=("$nv")
for file in "${files[@]}"; do
    if [ -e "$OUT/$file" ] && ! cmp -s "$OUT/$file" "$STAGE/$file"; then
        echo "refusing to replace different firmware: $OUT/$file" >&2
        exit 1
    fi
done
for file in "${files[@]}"; do
    install -D -m 0644 "$STAGE/$file" "$OUT/$file"
done
(cd "$OUT" && sha256sum "${files[@]}")
echo "Firmware ready in $OUT; includes private device calibration, keep it local."
echo "Source: original SYSTEM /etc/firmware; MODEM /image is not accepted for this Express."
echo "The original signed MDT uses packed program headers; see dossier/wifi-firmware.md for loader compatibility."
