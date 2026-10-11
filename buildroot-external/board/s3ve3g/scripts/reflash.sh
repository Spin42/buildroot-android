#!/usr/bin/env bash
# Write the persistent system image to userdata through lk2nd's fastboot, addressing the phone
# by serial only. Dry run by default: read-only checks, then the exact command. --execute writes.
#
# Usage: reflash.sh [--execute] [--via-ssh] [--no-wait] <serial> [image]
#   image      default: $S3VE3G_OUTPUT/images/userdata.simg (sparse; rootfs.ext2 also works),
#              S3VE3G_OUTPUT default: the port's output/full next to buildroot-android
#   --via-ssh  with --execute: if the running system answers ssh on 10.0.42.1, remount its root
#              read-only and "reboot-mode bootloader" into lk2nd fastboot first
#   --no-wait  with --execute: do not wait for the new system on USB after "fastboot reboot"
#
# Checks: image is sparse or ext2 and matches SHA256SUMS next to it; the serial is in fastboot and
# is the only fastboot device; lk2nd runs (lk2nd:version) and lk2nd:compatible is samsung,s3ve3g;
# the image fits userdata as lk2nd reports it.
# Without lk2nd fastboot: power on, wait for the Samsung logo, then hold Volume Down until the
# lk2nd menu shows (Volume Down from power-on gives Samsung download mode instead).
#
# The kit's tools/safe-flash.sh insists on a verified backup of the partition; userdata has none
# on purpose (port.yaml safety.accepted_loss), hence this script. Show the user the command it
# prints and get a yes before running it with --execute.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
COMPAT="samsung,s3ve3g"
PART=userdata
IP=10.0.42.1
OUT=${S3VE3G_OUTPUT:-$(cd "$HERE/../../../../.." && pwd)/output/full}
SSH_DIR=${S3VE3G_RESCUE_SSH_DIR:-$(dirname "$OUT")/rescue-ssh}
SSHO=(-o ConnectTimeout=5 -o BatchMode=yes -o UserKnownHostsFile="$SSH_DIR/known_hosts" -o StrictHostKeyChecking=yes)

EXEC=0; VIA_SSH=0; WAIT=1
while [ $# -gt 0 ]; do
	case "$1" in
	--execute) EXEC=1; shift ;;
	--via-ssh) VIA_SSH=1; shift ;;
	--no-wait) WAIT=0; shift ;;
	-h|--help) sed -n '2,20p' "$0"; exit 0 ;;
	-*) echo "unknown option $1"; exit 2 ;;
	*) break ;;
	esac
done
SER=${1:-}; IMG=${2:-$OUT/images/userdata.simg}
[ -n "$SER" ] || { sed -n '2,20p' "$0"; exit 2; }

fails=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fails=$((fails + 1)); }

in_fastboot() { timeout 10 fastboot devices 2>/dev/null | awk '{print $1}' | grep -qx "$SER"; }
getvar() { fastboot -s "$SER" getvar "$1" 2>&1 | awk -v k="$1" -F': ' '$1 == k {print $2; exit}'; }

if [ $VIA_SSH = 1 ] && ! in_fastboot; then
	if [ $EXEC = 1 ] && ssh "${SSHO[@]}" root@$IP true 2>/dev/null; then
		echo "asking the running system on $IP to reboot into lk2nd fastboot..."
		timeout 10 ssh "${SSHO[@]}" root@$IP 'sync; mount -o remount,ro / 2>/dev/null; reboot-mode bootloader' >/dev/null 2>&1 || true
		for _ in $(seq 1 30); do in_fastboot && break; sleep 2; done
	elif [ $EXEC = 0 ]; then
		echo "(dry run: --via-ssh would ask $IP to 'reboot-mode bootloader' now)"
	fi
fi

echo "reflash: serial=$SER partition=$PART image=$IMG"
echo "checks:"

# --- image --------------------------------------------------------------------------------------
raw_size=""
if [ ! -f "$IMG" ]; then
	bad "image not found: $IMG"
else
	magic=$(od -An -tx4 -N4 "$IMG" | tr -d ' ')
	if [ "$magic" = "ed26ff3a" ]; then
		# sparse header: magic, major/minor, hdr sizes, blk_sz @12, total_blks @16
		read -r blk total <<< "$(od -An -tu4 -j12 -N8 "$IMG")"
		raw_size=$((blk * total))
		ok "Android sparse image, $(stat -c %s "$IMG") bytes, expands to $raw_size"
	elif [ "$(od -An -tx2 -j1080 -N2 "$IMG" | tr -d ' ')" = "ef53" ]; then
		raw_size=$(stat -c %s "$IMG")
		ok "raw ext2 image, $raw_size bytes"
		if command -v dumpe2fs >/dev/null; then
			feat=$(dumpe2fs -h "$IMG" 2>/dev/null | sed -n 's/^Filesystem features: *//p')
			echo "$feat" | grep -qwE 'has_journal|extent|flex_bg|huge_file|dir_nlink|extra_isize|metadata_csum|64bit' \
				&& bad "ext features lk2nd cannot read: $feat" || ok "ext2 features readable by lk2nd: $feat"
		fi
	else
		bad "neither an Android sparse image nor ext2: $IMG"
	fi
	sums="$(dirname "$IMG")/SHA256SUMS"
	if [ -f "$sums" ] && grep -q " $(basename "$IMG")\$" "$sums"; then
		(cd "$(dirname "$IMG")" && grep " $(basename "$IMG")\$" SHA256SUMS | sha256sum -c --quiet - >/dev/null 2>&1) \
			&& ok "sha256 matches SHA256SUMS" || bad "sha256 does not match SHA256SUMS"
	else
		echo "  info  no SHA256SUMS entry for $(basename "$IMG")"
	fi
fi

# --- device -------------------------------------------------------------------------------------
devs=$(timeout 10 fastboot devices 2>/dev/null | awk '{print $1}')
if echo "$devs" | grep -qx "$SER"; then
	ok "$SER is in fastboot mode"
	others=$(echo "$devs" | grep -vx "$SER" | grep . || true)
	[ -z "$others" ] && ok "no other fastboot device attached" || bad "other fastboot devices attached: $others (unplug them)"
	ver=$(getvar lk2nd:version)
	[ -n "$ver" ] && ok "lk2nd $ver is running" || bad "not lk2nd (no lk2nd:version): stock fastboot or another bootloader"
	compat=$(getvar lk2nd:compatible)
	[ "$compat" = "$COMPAT" ] && ok "lk2nd:compatible '$compat'" || bad "lk2nd:compatible '$compat' is not '$COMPAT'"
	hex=$(getvar "partition-size:$PART")
	if [ -z "$hex" ]; then
		bad "lk2nd does not report partition-size:$PART"
	elif [ -n "$raw_size" ]; then
		[ "$raw_size" -le $((hex)) ] && ok "image fits $PART ($raw_size <= $((hex)) bytes)" \
			|| bad "image ($raw_size) larger than $PART ($((hex)) bytes)"
	fi
else
	bad "$SER is not in fastboot mode (lk2nd: hold Volume Down after the Samsung logo)"
fi

CMD=(fastboot -s "$SER" flash "$PART" "$IMG")
echo "command: ${CMD[*]}"
echo "then:    fastboot -s $SER reboot   (lk2nd boots /extlinux/extlinux.conf from $PART)"
if [ $fails -gt 0 ]; then echo "REFUSED: $fails check(s) failed."; exit 1; fi
if [ $EXEC = 0 ]; then echo "dry run: all checks passed. Re-run with --execute once the user approved this command."; exit 0; fi

"${CMD[@]}" || { echo "flash failed; lk2nd stays in fastboot, the previous boot path is unchanged"; exit 1; }
fastboot -s "$SER" reboot || exit 1
[ $WAIT = 1 ] || exit 0

echo "waiting for the persistent system on USB (1d6b:0104, serial $SER, product s3ve3g)..."
found=""
for _ in $(seq 1 60); do
	for d in /sys/bus/usb/devices/*; do
		[ "$(cat "$d/idVendor" 2>/dev/null):$(cat "$d/idProduct" 2>/dev/null)" = "1d6b:0104" ] || continue
		[ "$(cat "$d/serial" 2>/dev/null)" = "$SER" ] || continue
		found="$(cat "$d/product" 2>/dev/null)"
	done
	[ -n "$found" ] && break
	sleep 2
done
[ -n "$found" ] || { echo "no USB gadget after 120 s: look at the screen; lk2nd log: fastboot -s $SER oem log"; exit 1; }
echo "USB gadget up, product '$found'"
for _ in $(seq 1 20); do
	if ssh "${SSHO[@]}" root@$IP 'cat /etc/hostname; uname -r; mount | grep " on / "' 2>/dev/null; then exit 0; fi
	sleep 3
done
echo "no ssh on $IP after 60 s (host side needs 10.0.42.2 on the new USB interface)"; exit 1
