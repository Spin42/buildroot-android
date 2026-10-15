#!/usr/bin/env bash
# Reflash only Express userdata, starting from its verified running kit Linux.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
PORT=samsung-expressatt
PORT_DIR=$(cd "$REPO/.." && pwd)
KIT=$(cd "$PORT_DIR/../.." && pwd)
Y=$PORT_DIR/port.yaml
. "$KIT/tools/lib/kit_gadget.sh"
cfg() { python3 -I "$KIT/tools/lib/portcfg.py" "$Y" "$1"; }
fail() { echo "reflash: $*" >&2; exit 1; }
usage() { echo "Usage: $0 [--dry-run|--execute] ROOTFS.ext2"; }
EXEC=0
case ${1:-} in
    --execute) EXEC=1; shift ;;
    --dry-run) shift ;;
    --help) usage; exit 0 ;;
esac
[ "$#" = 1 ] || { usage >&2; exit 2; }
IMAGE=$(realpath -- "$1")
SER=$(cfg device.serial)
[ "$SER" = 08a571a0 ] || fail "this helper is for Express serial 08a571a0"
[ "$(cfg device.return_mode)" = download ] || fail "return mode must be download"
[ "$(stat -c %s "$IMAGE")" = 268435456 ] || fail "expected a 256 MiB userdata ext2 image"
[ "$(cfg device.partition_sizes.userdata)" -ge 268435456 ] || fail "userdata size is too small"
HOST_TOOLS=$REPO/output/firstlight-2026.08/host/sbin
for tool in dumpe2fs e2fsck debugfs; do
    [ -x "$HOST_TOOLS/$tool" ] || fail "missing Buildroot host tool: $HOST_TOOLS/$tool"
done
features=$(LC_ALL=C "$HOST_TOOLS/dumpe2fs" -h "$IMAGE" 2>/dev/null | sed -n 's/^Filesystem features: *//p')
[ "$features" = 'filetype sparse_super large_file' ] || fail "unexpected ext2 features: $features"
"$HOST_TOOLS/e2fsck" -fn "$IMAGE"
identity=$("$HOST_TOOLS/debugfs" -R 'cat /etc/kit-image' "$IMAGE" 2>/dev/null)
for required in "KIT_PORT=$PORT" "KIT_SERIAL=$SER" KIT_IMAGE=system KIT_RETURN_MODE=download; do
    grep -qx "$required" <<< "$identity" || fail "image identity missing: $required"
done
sha256sum "$IMAGE"
if [ "$EXEC" = 0 ]; then
    echo "Host checks passed; no device commands executed."
    echo "Execute verifies USB and Linux serials, returns by ACM to Odin, flashes only USERDATA, then waits up to 90 seconds for SSH."
    printf 'Flash plan: %q --port %q --mode samsung-download --execute flash userdata %q\n' "$KIT/tools/safe-flash.sh" "$PORT" "$IMAGE"
    echo 'Reboot plan: heimdall print-pit --resume'
    exit 0
fi

OWNER=${EXPRESSATT_REFLASH_OWNER:?set EXPRESSATT_REFLASH_OWNER to the current device owner}
[ "$(sed -n 's/^agent: //p' "$PORT_DIR/.device-owner")" = "$OWNER" ] || fail "device owner does not match"
for tool in ssh sshpass heimdall timeout; do command -v "$tool" >/dev/null || fail "missing $tool"; done
unset KIT_USB_SYSFS
LOG_DIR=$PORT_DIR/journal/plans
mkdir -p "$LOG_DIR"
KNOWN_HOSTS=$LOG_DIR/expressatt-reflash-known-hosts
export SSHPASS=${EXPRESSATT_REFLASH_PASSWORD:-root}

guard_allow() {
    local command verdict
    printf -v command '%q ' "$@"
    if ! verdict=$(python3 -I "$KIT/hooks/device_guard.py" --check "$command" 2>&1); then
        fail "guard rejected $command: $verdict"
    fi
    [[ "$verdict" = ALLOW* ]] || fail "guard did not return ALLOW: $verdict"
    printf '%s\n' "$verdict" >&2
}
select_gadget() {
    local rows
    rows=$(kit_gadgets | awk -v s="$SER" '$1==s')
    [ "$(grep -c . <<< "$rows")" = 1 ] || return 1
    read -r found product TTY NET <<< "$rows"
    [ "$found" = "$SER" ] || return 1
    case "$product" in "$PORT-system"|"$PORT-rescue") ;; *) return 1 ;; esac
    [[ "$TTY" = /dev/ttyACM* && "$NET" != - && "$NET" =~ ^[a-zA-Z0-9_.:-]+$ ]] || return 1
    USB_NODE=$(readlink -f "/sys/class/net/$NET/device/..")
    [ "$(cat "$USB_NODE/serial")" = "$SER" ] || return 1
}
odin_nodes() {
    local node
    for node in /sys/bus/usb/devices/*; do
        [ -f "$node/idVendor" ] || continue
        [ "$(cat "$node/idVendor")" = 04e8 ] || continue
        [ "$(cat "$node/idProduct")" = 685d ] || continue
        readlink -f "$node"
    done
}
prepare_ssh() {
    local seconds=$1
    SSH=(timeout "$seconds" sshpass -e ssh -F /dev/null -o "BindInterface=$NET"
        -o ConnectTimeout=3 -o ConnectionAttempts=1 -o ServerAliveInterval=2
        -o ServerAliveCountMax=1 -o StrictHostKeyChecking=accept-new
        -o "UserKnownHostsFile=$KNOWN_HOSTS" -o GlobalKnownHostsFile=/dev/null
        -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1
        root@10.0.42.1 "grep -qx KIT_RETURN_MODE=download /etc/kit-image && tr -d '\\000' < /proc/device-tree/serial-number")
}
select_gadget || fail "exactly one serial-matched kit gadget with ACM and networking is required"
ORIGINAL_USB_NODE=$USB_NODE
[ -z "$(odin_nodes)" ] || fail "another Odin device is already connected"
prepare_ssh 8
guard_allow "${SSH[@]}"
remote_serial=$("${SSH[@]}") || fail "cannot verify Linux serial over interface $NET"
[ "$remote_serial" = "$SER" ] || fail "remote Linux serial mismatch"
echo "Verified $SER on $NET ($USB_NODE); returning to download mode through $TTY"
# Use the serial-selected ACM branch from return-to-flash.sh, without its unbound SSH fallback.
guard_allow "$KIT/tools/return-to-flash.sh" --port "$PORT" --timeout 30
guard_allow python3 -I "$KIT/tools/lib/acm_probe.py" "$TTY" --timeout 5 --return
python3 -I "$KIT/tools/lib/acm_probe.py" "$TTY" --timeout 5 --return > "$LOG_DIR/expressatt-reflash-acm.log" 2>&1 || true
deadline=$((SECONDS + 30))
while :; do
    nodes=$(odin_nodes)
    [ "$nodes" = "$ORIGINAL_USB_NODE" ] && break
    [ -z "$nodes" ] || fail "Odin device count or physical USB path changed"
    [ "$SECONDS" -lt "$deadline" ] || fail "download mode did not appear within 30 seconds"
    sleep 1
done
FLASH=("$KIT/tools/safe-flash.sh" --port "$PORT" --mode samsung-download --execute flash userdata "$IMAGE")
# Check the visible wrapper and its actual device commands before the wrapper performs them.
guard_allow heimdall detect
guard_allow heimdall flash --USERDATA "$IMAGE" --no-reboot
guard_allow "${FLASH[@]}"
"${FLASH[@]}"
[ "$(odin_nodes)" = "$ORIGINAL_USB_NODE" ] || fail "Odin identity changed after flash"
guard_allow heimdall print-pit --resume
heimdall print-pit --resume > "$LOG_DIR/expressatt-reflash-reboot.log" 2>&1

deadline=$((SECONDS + 90))
rotated=0
while [ "$SECONDS" -lt "$deadline" ]; do
    if select_gadget && [ "$USB_NODE" = "$ORIGINAL_USB_NODE" ] && [ "$product" = "$PORT-system" ]; then
        if [ "$rotated" = 0 ]; then
            if [ -e "$KNOWN_HOSTS" ]; then
                backup=$(mktemp "$KNOWN_HOSTS.before-reflash.XXXXXXXX")
                cp -p -- "$KNOWN_HOSTS" "$backup"
                : > "$KNOWN_HOSTS"
                echo "Previous SSH host keys retained at $backup"
            fi
            rotated=1
        fi
        remaining=$((deadline - SECONDS))
        [ "$remaining" -gt 0 ] || break
        [ "$remaining" -le 6 ] || remaining=6
        prepare_ssh "$remaining"
        guard_allow "${SSH[@]}"
        if remote_serial=$("${SSH[@]}") && [ "$remote_serial" = "$SER" ]; then
            echo "Reflash passed: serial $SER returned over $NET; SSH host key saved in $KNOWN_HOSTS"
            exit 0
        fi
    fi
    sleep 1
done
fail "serial-verified SSH did not return within 90 seconds; keep Odin recovery available"
