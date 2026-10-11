#!/usr/bin/env bash
# Development loop for the whole WiFi stack: reboot, copy the modules, load them in order
# (wlan_mt starts the WiFi function itself) and save the kernel log and the network state.
# Usage: test-wlan.sh [no-reboot]; env: WLAN_ARGS, WMT_ARGS, BTIF_ARGS, WAIT, KDIR.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
IP=${TB3_IP:-10.0.42.1}
KDIR=${KDIR:-$HOME/Projects/lenovo_tab3_7/port/build-sensors}
SSHO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8"
r() { sshpass -p root ssh $SSHO root@$IP "$@"; }
LOG=${LOG:-/tmp/wlan-test.log}

if [ "${1:-}" != no-reboot ]; then
	timeout 8 sshpass -p root ssh $SSHO -o ServerAliveInterval=2 -o ServerAliveCountMax=1 root@$IP 'reboot -f' >/dev/null 2>&1
	sleep 25
fi
for i in $(seq 1 30); do timeout 8 sshpass -p root ssh $SSHO root@$IP true 2>/dev/null && break; sleep 4; done
sshpass -p root scp $SSHO "$KDIR"/net/wireless/cfg80211.ko "$HERE"/btif/mtk_btif.ko "$HERE"/consys/mt8127_consys.ko \
	"$HERE"/wmt/mtk_stp_wmt_soc.ko "$HERE"/wlan/wlan_mt.ko root@$IP:/root/
r "mount -t debugfs none /sys/kernel/debug 2>/dev/null; dmesg -n 4; dmesg -c >/dev/null
   insmod /root/cfg80211.ko && insmod /root/mt8127_consys.ko && insmod /root/mtk_btif.ko ${BTIF_ARGS:-} \
     && insmod /root/mtk_stp_wmt_soc.ko ${WMT_ARGS:-} && (insmod /root/wlan_mt.ko ${WLAN_ARGS:-}; echo \"wlan_mt rc=\\\$?\" > /root/wlan.rc) &
   sleep ${WAIT:-25}; cat /root/wlan.rc 2>&1; echo '--- interfaces'; ls /sys/class/net; ip -br link; echo '--- dmesg'; dmesg" > "$LOG" 2>&1
echo "log: $LOG"
