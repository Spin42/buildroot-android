#!/usr/bin/env bash
# Development loop on the tablet: reboot, copy the modules, load the stack, switch the WiFi function on
# and save the kernel log. Usage: test-wmt.sh [no-reboot]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
IP=${TB3_IP:-10.0.42.1}
SSHO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8"
r() { sshpass -p root ssh $SSHO root@$IP "$@"; }
LOG=${LOG:-/tmp/wmt-test.log}

if [ "${1:-}" != no-reboot ]; then
	timeout 8 sshpass -p root ssh $SSHO -o ServerAliveInterval=2 -o ServerAliveCountMax=1 root@$IP 'reboot -f' >/dev/null 2>&1
	sleep 25
fi
for i in $(seq 1 30); do timeout 8 sshpass -p root ssh $SSHO root@$IP true 2>/dev/null && break; sleep 4; done
sshpass -p root scp $SSHO "$HERE"/btif/mtk_btif.ko "$HERE"/consys/mt8127_consys.ko "$HERE"/wmt/mtk_stp_wmt_soc.ko \
	"$HERE"/wmt/test/wmt_wifi_ctl.ko root@$IP:/root/
r "mount -t debugfs none /sys/kernel/debug 2>/dev/null; dmesg -n 4
   insmod /root/mt8127_consys.ko && insmod /root/mtk_btif.ko ${BTIF_ARGS:-} && insmod /root/mtk_stp_wmt_soc.ko ${WMT_ARGS:-} && insmod /root/wmt_wifi_ctl.ko
   dmesg -c >/dev/null
   ${PRE_CMD:-:}
   (echo 1 > /sys/kernel/debug/wmt_wifi/power; echo \"func_on rc=\$?\" > /root/funcon.rc) &
   sleep ${WAIT:-20}; cat /root/funcon.rc 2>&1; dmesg" > "$LOG" 2>&1
echo "log: $LOG"
# the firmware trace area of the shared memory, read right after the run
r 'cat /sys/kernel/debug/mt8127_consys/emi' > "${LOG%.log}.emi" 2>/dev/null
r 'cat /sys/kernel/debug/mt8127_consys/emi_full' > "${LOG%.log}.emifull" 2>/dev/null
echo "emi dump: ${LOG%.log}.emi"
