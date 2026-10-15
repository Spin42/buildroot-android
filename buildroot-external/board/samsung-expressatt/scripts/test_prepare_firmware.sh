#!/usr/bin/env bash
set -euo pipefail
[ "$#" = 2 ] || { echo "Usage: $0 BACKUPS_DIRECTORY NEW_EVIDENCE_DIRECTORY" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
BACKUPS=$(realpath -- "$1")
[ ! -e "$2" ] || { echo "evidence directory must be new" >&2; exit 1; }
mkdir -p "$2"
EVIDENCE=$(realpath -- "$2")
exec > >(tee "$EVIDENCE/test.log") 2>&1
debugfs -R "dump /WCNSS_qcom_wlan_nv.bin \"$EVIDENCE/raw-nv.bin\"" "$BACKUPS/persist.img"
python3 "$HERE/test_legacy_nv.py" "$EVIDENCE/raw-nv.bin" "$EVIDENCE/nv-tests"
bash "$HERE/prepare_firmware.sh" "$BACKUPS" "$EVIDENCE/first"
bash "$HERE/prepare_firmware.sh" "$BACKUPS" "$EVIDENCE/second"
cmp "$EVIDENCE/nv-tests/projected.bin" "$EVIDENCE/first/wlan/prima/WCNSS_qcom_wlan_nv.bin"
diff -r "$EVIDENCE/first" "$EVIDENCE/second"
echo "PASS: two independent extractions are byte-identical"
[ "$(stat -c %s "$EVIDENCE/first/wcnss.b04")" = 1808268 ]
grep -aFq 'M8960AAAAANAAW1050126H' "$EVIDENCE/first/wcnss.b04"
echo "PASS: original SYSTEM payload size and embedded version"
(cd "$EVIDENCE/first" && sha256sum wcnss.* wlan/prima/WCNSS_qcom_wlan_nv.bin) > "$EVIDENCE/correct.sha256"
mkdir "$EVIDENCE/modem-reference"
for file in wcnss.mdt wcnss.b00 wcnss.b01 wcnss.b02 wcnss.b04; do
    mcopy -i "$BACKUPS/modem.img" "::/image/$file" "$EVIDENCE/modem-reference/$file"
done
(cd "$EVIDENCE/modem-reference" && sha256sum wcnss.*) > "$EVIDENCE/modem-reference.sha256"
if (cd "$EVIDENCE/modem-reference" && sha256sum --check "$HERE/wcnss-system.sha256"); then
    echo "FAIL: MODEM set accepted"; exit 1
fi
echo "PASS: pinned manifest rejects MODEM set"
if bash "$HERE/prepare_firmware.sh" "$BACKUPS" "$EVIDENCE/modem-reference"; then
    echo "FAIL: different existing output replaced"; exit 1
fi
(cd "$EVIDENCE/modem-reference" && sha256sum --check "$EVIDENCE/modem-reference.sha256")
[ ! -e "$EVIDENCE/modem-reference/wlan" ]
echo "PASS: conflicting output unchanged, no partial calibration installation"
mkdir "$EVIDENCE/bad-backups"
for file in system.img persist.img; do ln -s "$BACKUPS/$file" "$EVIDENCE/bad-backups/$file"; done
printf '%064d  system.img\n' 0 > "$EVIDENCE/bad-backups/SHA256SUMS"
if bash "$HERE/prepare_firmware.sh" "$EVIDENCE/bad-backups" "$EVIDENCE/rejected"; then
    echo "FAIL: bad backup checksum accepted"; exit 1
fi
[ ! -e "$EVIDENCE/rejected" ]
echo "PASS: invalid backup checksum rejected before extraction"
mkdir "$EVIDENCE/invalid-nv-backups"
ln -s "$BACKUPS/system.img" "$EVIDENCE/invalid-nv-backups/system.img"
cp --reflink=auto "$BACKUPS/persist.img" "$EVIDENCE/invalid-nv-backups/persist.img"
debugfs -w -R 'rm /WCNSS_qcom_wlan_nv.bin' "$EVIDENCE/invalid-nv-backups/persist.img"
debugfs -w -R "write \"$EVIDENCE/nv-tests/missing-bit-18.bin\" /WCNSS_qcom_wlan_nv.bin" "$EVIDENCE/invalid-nv-backups/persist.img"
(cd "$EVIDENCE/invalid-nv-backups" && sha256sum system.img persist.img) > "$EVIDENCE/invalid-nv-backups/SHA256SUMS"
if bash "$HERE/prepare_firmware.sh" "$EVIDENCE/invalid-nv-backups" "$EVIDENCE/rejected-nv"; then
    echo "FAIL: invalid calibration installed"; exit 1
fi
[ -z "$(ls -A "$EVIDENCE/rejected-nv")" ]
echo "PASS: invalid NV fails before any firmware is installed"
echo "PASS: all host firmware fixture scenarios"
