#!/usr/bin/env bash
# FM bring-up loop: copy the FM module, the fm-radio tool and the FM firmware to the tablet, load
# the module on top of the running WMT stack, tune and save the kernel log and the tuner state.
# Every tablet command is bounded: the tablet BusyBox has no timeout(1), so long commands run in
# the background and are killed after a few seconds.
# Usage: test-fm.sh [MHz]; env: TB3_IP, FW_DIR (stock /system/etc/firmware/mt6627), CC, LOG.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
IP=${TB3_IP:-10.0.42.1}
FREQ=${1:-98.0}
FW_DIR=${FW_DIR:-$HOME/Projects/lenovo_tab3_7/dump/bootloader_analysis/system_extract/etc/firmware/mt6627}
CC=${CC:-$HOME/Projects/buildroot-android-tb3/output/host/bin/arm-buildroot-linux-gnueabihf-gcc}
LOG=${LOG:-/tmp/fm-test.log}
SSHO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8"
r() { timeout 60 sshpass -p root ssh $SSHO root@$IP "$@"; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$CC" -O2 -Wall -o "$TMP/fm-radio" "$HERE/tools/fm-radio.c" || exit 1

timeout 30 sshpass -p root scp $SSHO "$HERE/fm/mtk_consys_fm.ko" "$TMP/fm-radio" \
	"$FW_DIR/mt6627_fm_v1_patch.bin" "$FW_DIR/mt6627_fm_v1_coeff.bin" root@$IP:/root/ || exit 1

# run <seconds> <command>: background the command and kill it when it overruns
r "mkdir -p /lib/firmware/mediatek
   cp /root/mt6627_fm_v1_*.bin /lib/firmware/mediatek/
   chmod 755 /root/fm-radio
   run() { t=\$1; shift; \"\$@\" & p=\$!; i=0
           while kill -0 \$p 2>/dev/null && [ \$i -lt \$((t * 10)) ]; do usleep 100000; i=\$((i + 1)); done
           kill -0 \$p 2>/dev/null && { echo \"*** killed after \${t}s: \$*\"; kill -9 \$p; }; }
   mount -t debugfs none /sys/kernel/debug 2>/dev/null
   echo 'file fm_*.c +p' > /sys/kernel/debug/dynamic_debug/control 2>/dev/null
   dmesg -c >/dev/null
   lsmod | grep -q mtk_consys_fm && rmmod mtk_consys_fm
   insmod /root/mtk_consys_fm.ko || exit 1
   echo '--- tune $FREQ'; run 20 /root/fm-radio tune $FREQ
   echo '--- status'; run 5 /root/fm-radio status
   echo '--- rds'; run 8 /root/fm-radio rds 5
   echo '--- dmesg'; dmesg" > "$LOG" 2>&1
echo "log: $LOG"
