#!/usr/bin/env bash
# Write the persistent system image to userdata, addressing the phone by serial. Dry run by
# default: read-only checks, then the plan. --execute writes.
#
# Usage: reflash.sh [--mode odin|fastboot] [--execute] [--image <file>] [--known-hosts <file>]
#                   [--no-wait] [--boot <img>] <serial>
#   --mode odin      (default) Samsung download mode with heimdall. If the running system answers
#                    ssh on 10.0.42.1, it remounts its root read-only and "reboot-mode download"s;
#                    if the phone already shows download mode, ssh is skipped. Image: raw ext2,
#                    default $S3VE3G_OUTPUT/images/rootfs.ext2.
#   --mode fastboot  lk2nd fastboot, entered by holding Volume Down at the Samsung logo.
#                    Image: sparse or raw ext2, default $S3VE3G_OUTPUT/images/userdata.simg.
#   --image          the userdata image (S3VE3G_OUTPUT default: the port's output/full)
#   --known-hosts    default: <output>/../rescue-ssh/known_hosts ($S3VE3G_RESCUE_SSH_DIR)
#   --no-wait        do not wait for the new system on USB and ssh after the write
#   --boot <img>     odin mode only: also write the whole BOOT partition. BOOT holds lk2nd, so
#                    <img> replaces lk2nd itself; a bad image leaves only download mode.
#
# Only USERDATA (and BOOT with --boot) is ever written: no --repartition, no --pit, no AP tar.
# userdata keeps no backup (port.yaml safety.accepted_loss), hence this script and not the kit's
# tools/safe-flash.sh. Show the user the plan it prints and get a yes before --execute.
# ok/bad/info always succeed, so "A && ok || bad" is a safe if/else; the remote script is
# single-quoted on purpose (it expands on the phone).
# shellcheck disable=SC2015,SC2016
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
COMPAT="samsung,s3ve3g"
IP=10.0.42.1
ODIN_ID="04e8:685d"
GADGET_ID="1d6b:0104"
OUT=${S3VE3G_OUTPUT:-$(cd "$HERE/../../../../.." && pwd)/output/full}
SSH_DIR=${S3VE3G_RESCUE_SSH_DIR:-$(dirname "$OUT")/rescue-ssh}
# ext2 features lk2nd's LK driver reads (same list as create_userdata_image.sh): no ro_compat
# beyond sparse_super/large_file, no extents, journal or 64bit.
EXT2_OK="ext_attr resize_inode dir_index filetype sparse_super large_file"

usage() { sed -n '2,21p' "$0"; exit "${1:-0}"; }
MODE=odin; EXEC=0; WAIT=1; IMG=""; BOOT_IMG=""; KH="$SSH_DIR/known_hosts"
while [ $# -gt 0 ]; do
	case "$1" in
	--mode) MODE=${2:-}; shift 2 ;;
	--execute) EXEC=1; shift ;;
	--image) IMG=${2:-}; shift 2 ;;
	--known-hosts) KH=${2:-}; shift 2 ;;
	--no-wait) WAIT=0; shift ;;
	--boot) BOOT_IMG=${2:-}; shift 2 ;;
	-h|--help) usage 0 ;;
	-*) echo "unknown option $1"; usage 2 ;;
	*) break ;;
	esac
done
SER=${1:-}
[ -n "$SER" ] && [ $# -eq 1 ] || usage 2
case "$MODE" in
odin) IMG=${IMG:-$OUT/images/rootfs.ext2} ;;
fastboot) IMG=${IMG:-$OUT/images/userdata.simg}
	[ -z "$BOOT_IMG" ] || { echo "--boot is for --mode odin only"; exit 2; } ;;
*) echo "--mode must be odin or fastboot"; exit 2 ;;
esac

SSHO=(-o ConnectTimeout=5 -o BatchMode=yes -o ServerAliveInterval=2 -o ServerAliveCountMax=3
	-o UserKnownHostsFile="$KH" -o StrictHostKeyChecking=yes -o LogLevel=ERROR)
fails=0
ok()   { echo "  ok    $*"; }
bad()  { echo "  FAIL  $*"; fails=$((fails + 1)); }
info() { echo "  info  $*"; }
rssh() { timeout "${RSSH_TIMEOUT:-20}" ssh "${SSHO[@]}" root@$IP "$@"; }

# USB devices with vendor:product $1 (and serial $2 if given), one sysfs path per line.
usb_find() {
	local d
	for d in /sys/bus/usb/devices/*; do
		[ "$(cat "$d/idVendor" 2>/dev/null):$(cat "$d/idProduct" 2>/dev/null)" = "$1" ] || continue
		[ -z "${2:-}" ] || [ "$(cat "$d/serial" 2>/dev/null)" = "$2" ] || continue
		echo "$d"
	done
}
usb_count() { usb_find "$@" | grep -c . ; }

# Image checks shared by both modes. Sets RAW_SIZE.
RAW_SIZE=""
check_image() {
	local magic feat f sums
	if [ ! -f "$IMG" ]; then bad "image not found: $IMG"; return; fi
	magic=$(od -An -tx4 -N4 "$IMG" | tr -d ' ')
	if [ "$magic" = "ed26ff3a" ]; then
		if [ "$MODE" = odin ]; then
			bad "Android sparse image: heimdall writes raw bytes, use rootfs.ext2"
		else
			# sparse header: magic, major/minor, hdr sizes, blk_sz @12, total_blks @16
			read -r blk total <<< "$(od -An -tu4 -j12 -N8 "$IMG")"
			RAW_SIZE=$((blk * total))
			ok "Android sparse image, $(stat -c %s "$IMG") bytes, expands to $RAW_SIZE"
		fi
	elif [ "$(od -An -tx2 -j1080 -N2 "$IMG" | tr -d ' ')" = "ef53" ]; then
		RAW_SIZE=$(stat -c %s "$IMG")
		ok "raw ext2 image, $RAW_SIZE bytes"
		if ! command -v dumpe2fs >/dev/null; then
			bad "dumpe2fs not installed: cannot check the ext2 features"
		else
			feat=$(dumpe2fs -h "$IMG" 2>/dev/null | sed -n 's/^Filesystem features: *//p')
			local extra=""
			for f in $feat; do
				case " $EXT2_OK " in *" $f "*) ;; *) extra="$extra $f" ;; esac
			done
			if [ -z "$feat" ]; then bad "dumpe2fs cannot read the superblock"
			elif [ -n "$extra" ]; then bad "features lk2nd cannot read:$extra"
			else ok "ext2 features lk2nd reads: $feat"; fi
		fi
	else
		bad "neither raw ext2 nor an Android sparse image: $IMG"
	fi
	sums="$(dirname "$IMG")/SHA256SUMS"
	if [ -f "$sums" ] && grep -q " $(basename "$IMG")\$" "$sums"; then
		(cd "$(dirname "$IMG")" && grep " $(basename "$IMG")\$" SHA256SUMS | sha256sum -c --quiet - >/dev/null 2>&1) \
			&& ok "sha256 matches SHA256SUMS" || bad "sha256 does not match SHA256SUMS"
	else
		bad "no SHA256SUMS entry for $(basename "$IMG") next to it"
	fi
}

# Waits for the new system: the gadget with the serial on USB, then ssh. Prints PASS or FAIL.
wait_new_system() {
	local found=""
	echo "waiting for the persistent system on USB ($GADGET_ID, serial $SER)..."
	for _ in $(seq 1 90); do
		found=$(usb_find "$GADGET_ID" "$SER" | head -n1)
		[ -n "$found" ] && break
		sleep 2
	done
	if [ -z "$found" ]; then
		echo "FAIL: no USB gadget after 180 s. Look at the screen. Recovery: docs/s3ve3g.md, 'Recovery from a bad image'."
		exit 1
	fi
	echo "USB gadget up, product '$(cat "$found/product" 2>/dev/null)'"
	for _ in $(seq 1 20); do
		if RSSH_TIMEOUT=15 rssh 'cat /etc/hostname; uname -r; cat /sys/kernel/reboot/mode; mount | grep " on / "' 2>/dev/null; then
			echo "PASS: the new system answers ssh on $IP"
			exit 0
		fi
		sleep 3
	done
	echo "FAIL: no ssh on $IP after 60 s (the host needs 10.0.42.2 on the new USB interface)"
	exit 1
}

echo "reflash: mode=$MODE serial=$SER partition=userdata image=$IMG${BOOT_IMG:+ boot=$BOOT_IMG}"
echo "checks:"
check_image

# =================================================================================================
if [ "$MODE" = fastboot ]; then
	getvar() { fastboot -s "$SER" getvar "$1" 2>&1 | awk -v k="$1" -F': ' '$1 == k {print $2; exit}'; }
	devs=$(timeout 10 fastboot devices 2>/dev/null | awk '{print $1}')
	if echo "$devs" | grep -qx "$SER"; then
		ok "$SER is in fastboot mode"
		others=$(echo "$devs" | grep -vx "$SER" | grep . || true)
		[ -z "$others" ] && ok "no other fastboot device attached" || bad "other fastboot devices attached: $others (unplug them)"
		ver=$(getvar lk2nd:version)
		[ -n "$ver" ] && ok "lk2nd $ver is running" || bad "not lk2nd (no lk2nd:version)"
		compat=$(getvar lk2nd:compatible)
		[ "$compat" = "$COMPAT" ] && ok "lk2nd:compatible '$compat'" || bad "lk2nd:compatible '$compat' is not '$COMPAT'"
		hex=$(getvar "partition-size:userdata")
		if [ -z "$hex" ]; then
			bad "lk2nd does not report partition-size:userdata"
		elif [ -n "$RAW_SIZE" ]; then
			[ "$RAW_SIZE" -le $((hex)) ] && ok "image fits userdata ($RAW_SIZE <= $((hex)) bytes)" \
				|| bad "image ($RAW_SIZE) larger than userdata ($((hex)) bytes)"
		fi
	else
		bad "$SER is not in lk2nd fastboot (power on, hold Volume Down at the Samsung logo until the lk2nd menu shows)"
	fi
	CMD=(fastboot -s "$SER" flash userdata "$IMG")
	echo "plan:"
	echo "  1. ${CMD[*]}"
	echo "  2. fastboot -s $SER reboot   (lk2nd boots /extlinux/extlinux.conf from userdata)"
	if [ $fails -gt 0 ]; then echo "REFUSED: $fails check(s) failed."; exit 1; fi
	if [ $EXEC = 0 ]; then echo "dry run: all checks passed. Re-run with --execute once the user approved this plan."; exit 0; fi
	"${CMD[@]}" || { echo "FAIL: flash failed; lk2nd stays in fastboot"; exit 1; }
	fastboot -s "$SER" reboot || exit 1
	[ $WAIT = 1 ] || exit 0
	wait_new_system
fi

# =================================================================================================
# odin mode
if [ -n "$BOOT_IMG" ]; then
	echo "  WARN  --boot writes the whole BOOT partition, which holds lk2nd: $BOOT_IMG REPLACES lk2nd."
	echo "        Without a working lk2nd, extlinux on userdata no longer boots; only download mode remains."
	if [ ! -f "$BOOT_IMG" ]; then
		bad "boot image not found: $BOOT_IMG"
	else
		[ "$(head -c 8 "$BOOT_IMG")" = "ANDROID!" ] && ok "BOOT image has the Android boot header" \
			|| bad "BOOT image has no ANDROID! header"
		grep -aq lk2nd "$BOOT_IMG" && ok "BOOT image contains lk2nd" \
			|| bad "BOOT image does not contain lk2nd: the system on userdata would no longer boot"
		bsums="$(dirname "$BOOT_IMG")/SHA256SUMS"
		if [ -f "$bsums" ] && grep -q " $(basename "$BOOT_IMG")\$" "$bsums"; then
			(cd "$(dirname "$BOOT_IMG")" && grep " $(basename "$BOOT_IMG")\$" SHA256SUMS | sha256sum -c --quiet - >/dev/null 2>&1) \
				&& ok "BOOT image sha256 matches SHA256SUMS" || bad "BOOT image sha256 does not match SHA256SUMS"
		else
			bad "no SHA256SUMS entry for $(basename "$BOOT_IMG") next to it"
		fi
	fi
fi

command -v heimdall >/dev/null && ok "heimdall installed" || bad "heimdall not installed"

n_odin=$(usb_count "$ODIN_ID")
VIA_SSH=0
if [ "$n_odin" -eq 1 ]; then
	ok "one device in Samsung download mode ($ODIN_ID): ssh step skipped"
	info "download mode does not report a serial: make sure the phone on screen is $SER"
elif [ "$n_odin" -gt 1 ]; then
	bad "$n_odin devices in Samsung download mode: unplug all but $SER"
elif [ ! -f "$KH" ]; then
	bad "no known_hosts file $KH (use --known-hosts)"
elif out=$(rssh '
	s=$(tr -d "\0" < /proc/device-tree/serial-number 2>/dev/null)
	echo "dtserial=$s"
	echo "gserial=$(cat /sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber 2>/dev/null)"
	echo "rmode=$(cat /sys/kernel/reboot/mode 2>/dev/null)"
	echo "tool=$(command -v reboot-mode)"
	for d in /sys/class/block/*; do
		n=$(sed -n "s/^PARTNAME=//p" $d/uevent 2>/dev/null)
		case "$n" in userdata|boot) echo "size_$n=$(( $(cat $d/size) * 512 ))" ;; esac
	done' 2>/dev/null); then
	VIA_SSH=1
	ok "the running system answers ssh on $IP"
	val() { echo "$out" | sed -n "s/^$1=//p" | head -n1; }
	if [ "$(val dtserial)" = "$SER" ] || [ "$(val gserial)" = "$SER" ]; then
		ok "serial $SER confirmed (device tree '$(val dtserial)', gadget '$(val gserial)')"
	else
		bad "serial mismatch: device tree '$(val dtserial)', gadget '$(val gserial)', expected $SER"
	fi
	[ "$(usb_count "$GADGET_ID" "$SER")" -eq 1 ] && ok "gadget $GADGET_ID with serial $SER on this host's USB" \
		|| bad "no USB gadget $GADGET_ID with serial $SER on this host"
	rmode=$(val rmode)
	[ "$rmode" = warm ] && ok "/sys/kernel/reboot/mode is warm" \
		|| info "/sys/kernel/reboot/mode is '$rmode'; the plan writes warm before rebooting"
	[ -n "$(val tool)" ] && ok "reboot-mode at $(val tool)" || bad "no reboot-mode tool on the phone"
	us=$(val size_userdata)
	if [ -z "$us" ]; then bad "the phone reports no userdata partition"
	elif [ -n "$RAW_SIZE" ]; then
		[ "$RAW_SIZE" -le "$us" ] && ok "image fits userdata ($RAW_SIZE <= $us bytes)" \
			|| bad "image ($RAW_SIZE) larger than userdata ($us bytes)"
	fi
	bs=$(val size_boot)
	if [ -n "$BOOT_IMG" ] && [ -f "$BOOT_IMG" ] && [ -n "$bs" ]; then
		[ "$(stat -c %s "$BOOT_IMG")" -le "$bs" ] && ok "BOOT image fits boot ($bs bytes)" \
			|| bad "BOOT image larger than boot ($bs bytes)"
	fi
else
	bad "no ssh on $IP and no device in download mode (from Linux: 'reboot-mode download'; powered off: Volume Down + Power)"
fi

FLASH=(heimdall flash --USERDATA "$IMG")
[ -z "$BOOT_IMG" ] || FLASH+=(--BOOT "$BOOT_IMG")
echo "plan:"
step=1
if [ $VIA_SSH = 1 ]; then
	echo "  $step. ssh root@$IP 'sync; mount -o remount,ro /; echo warm > /sys/kernel/reboot/mode; reboot-mode download'"
	step=$((step + 1))
	echo "  $step. wait for exactly one $ODIN_ID (Samsung download mode)"; step=$((step + 1))
fi
echo "  $step. heimdall detect"; step=$((step + 1))
echo "  $step. ${FLASH[*]}"; step=$((step + 1))
echo "     (heimdall reboots; aboot -> lk2nd -> /extlinux/extlinux.conf on userdata)"
[ $WAIT = 0 ] || echo "  $step. wait for $GADGET_ID serial $SER, then ssh $IP"
if [ $fails -gt 0 ]; then echo "REFUSED: $fails check(s) failed."; exit 1; fi
if [ $EXEC = 0 ]; then echo "dry run: all checks passed. Re-run with --execute once the user approved this plan."; exit 0; fi

if [ $VIA_SSH = 1 ]; then
	echo "asking the running system to reboot into download mode..."
	RSSH_TIMEOUT=15 rssh 'sync; mount -o remount,ro /; echo warm > /sys/kernel/reboot/mode; reboot-mode download' >/dev/null 2>&1 || true
	for _ in $(seq 1 45); do [ "$(usb_count "$ODIN_ID")" -ge 1 ] && break; sleep 2; done
fi
n_odin=$(usb_count "$ODIN_ID")
[ "$n_odin" -eq 1 ] || { echo "FAIL: expected one $ODIN_ID device, found $n_odin. Nothing was written."; exit 1; }
heimdall detect || { echo "FAIL: heimdall detect. Nothing was written."; exit 1; }
"${FLASH[@]}" || { echo "FAIL: heimdall flash. Download mode stays up: re-run with --execute, or see docs/s3ve3g.md."; exit 1; }
[ $WAIT = 1 ] || exit 0
wait_new_system
