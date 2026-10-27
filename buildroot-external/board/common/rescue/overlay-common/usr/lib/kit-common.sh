# Shared by the /init of every kit test image (first light, rescue). Sourced, not run.
# Install as /usr/lib/kit-common.sh, next to /etc/kit-image and /usr/sbin/usb-gadget.sh.
[ -f /etc/kit-image ] && . /etc/kit-image
KIT_IMAGE=${KIT_IMAGE:-test}
KIT_RETURN_MODE=${KIT_RETURN_MODE:-bootloader}
KIT_FAIL_REBOOT=${KIT_FAIL_REBOOT:-120}

kit_mount() {
	mount -t devtmpfs devtmpfs /dev 2>/dev/null
	mount -t proc proc /proc
	mount -t sysfs sysfs /sys
	mkdir -p /dev/pts /run /tmp /sys/kernel/config /sys/fs/pstore
	mount -t devpts devpts /dev/pts
	mount -t tmpfs tmpfs /run
	mount -t tmpfs tmpfs /tmp
	mount -t configfs configfs /sys/kernel/config 2>/dev/null
	mount -t pstore pstore /sys/fs/pstore 2>/dev/null
	ip link set lo up
	# kit.fail_reboot=N on the kernel cmdline overrides the image default (0: never)
	for a in $(cat /proc/cmdline); do
		case $a in kit.fail_reboot=*) KIT_FAIL_REBOOT=${a#*=} ;; esac
	done
}

# busybox reboot/poweroff/halt signal PID 1; a shell /init must act on them itself.
kit_signals() {
	trap 'sync; reboot -f' TERM INT
	trap 'sync; poweroff -f' USR2
	trap 'sync; halt -f' USR1
}

# Fed from a separate process, the watchdog only fires when the kernel stops scheduling.
kit_watchdog() {
	[ -e /dev/watchdog ] && watchdog -T 20 -t 5 /dev/watchdog
}

kit_say() {
	echo "$*" > /dev/tty0 2>/dev/null
	echo "$KIT_IMAGE: $*" > /dev/kmsg
}

# Every power supply (battery, charger, USB): state, voltage (µV) and capacity (%), for the host
kit_battery() {
	for p in /sys/class/power_supply/*; do
		[ -d "$p" ] || continue
		echo "KIT-BATTERY ${p##*/} type=$(cat "$p/type" 2>/dev/null) status=$(cat "$p/status" 2>/dev/null) online=$(cat "$p/online" 2>/dev/null) voltage_uv=$(cat "$p/voltage_now" 2>/dev/null) capacity=$(cat "$p/capacity" 2>/dev/null)"
	done
}

kit_banner() {
	echo "=========================================="
	echo "  ${KIT_PORT:-?} $KIT_IMAGE"
	echo "=========================================="
	uname -a
	grep MemTotal /proc/meminfo
	echo "fb0: $(cat /sys/class/graphics/fb0/name 2>/dev/null) $(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null)"
	echo "watchdog: $([ -e /dev/watchdog ] && echo fed || echo absent)"
	echo "cmdline: $(cat /proc/cmdline)"
	kit_battery
	# One machine-readable line for tools/await-boot.sh
	echo "KIT-READY port=${KIT_PORT:-?} image=$KIT_IMAGE kernel=$(uname -r)"
}

# Key-free way back to where the host can flash: the vendor's flash mode through reboot-mode
# (bootloader for fastboot, download on Samsung), else a plain reboot.
kit_return_to_flash_mode() {
	kit_say "returning to $KIT_RETURN_MODE"
	sync
	command -v reboot-mode >/dev/null && reboot-mode "$KIT_RETURN_MODE"
	reboot -f
}

# In the background (the UDC can take seconds to appear): USB gadget, a shell server on the
# network, then the banner, dmesg and a shell on the ACM console. Without USB the host cannot
# see this image, so it returns to the flash mode after KIT_FAIL_REBOOT seconds.
kit_start_usb() {
	(
		if /usr/sbin/usb-gadget.sh > /tmp/usb-gadget.log 2>&1; then
			kit_say "$(tail -1 /tmp/usb-gadget.log)"
			if command -v dropbear >/dev/null; then
				mkdir -p /etc/dropbear
				dropbear -R -B
			else
				telnetd -l /bin/sh
			fi
			# Writes to ttyGS0 block until the host opens the port
			while [ -e /dev/ttyGS0 ]; do
				{ kit_banner; echo "--- dmesg ---"; dmesg; echo "--- shell ---"; } > /dev/ttyGS0 2>&1
				setsid sh -c 'exec /bin/sh -i <>/dev/ttyGS0 >&0 2>&0'
				sleep 1
			done
		else
			kit_say "no USB: $(tail -1 /tmp/usb-gadget.log)"
			if [ "$KIT_FAIL_REBOOT" -gt 0 ]; then
				kit_say "back to $KIT_RETURN_MODE in ${KIT_FAIL_REBOOT}s (kit.fail_reboot=0 keeps this screen)"
				sleep "$KIT_FAIL_REBOOT"
				kit_return_to_flash_mode
			fi
		fi
	) &
}
