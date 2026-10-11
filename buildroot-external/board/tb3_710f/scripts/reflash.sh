#!/usr/bin/env bash
# Whole cycle without touching the tablet: ssh in, reboot into the bootloader, flash, boot, wait for ssh.
# Usage: reflash.sh [image]   Needs the running system to have tb3-tools (reboot-mode).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SER=${TB3_SERIAL:-PJPV59BIIBKJQOZ9}
IP=${TB3_IP:-10.0.42.1}
SSHO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5"

if sshpass -p root ssh $SSHO root@$IP true 2>/dev/null; then
	echo "asking the running system to reboot into the bootloader..."
	# fire and forget: the connection hangs when the tablet reboots, so cap it and move on
	timeout 6 sshpass -p root ssh $SSHO -o ServerAliveInterval=2 -o ServerAliveCountMax=1 \
		root@$IP 'reboot-mode bootloader' >/dev/null 2>&1 || true
	echo "waiting for the tablet to show up in fastboot..."
	for i in $(seq 1 30); do fastboot devices | grep -q "^$SER" && break; sleep 2; done
else
	echo "tablet not reachable over ssh; assuming it is already in fastboot"
fi
"$HERE/flash.sh" "$@"
echo "waiting for ssh..."
for i in $(seq 1 40); do
	sleep 4
	sshpass -p root ssh $SSHO root@$IP 'uname -rv' 2>/dev/null && exit 0
done
echo "no ssh after ~160 s"; exit 1
