#!/bin/sh
# USB gadget of the kit's test images, through configfs: ACM serial (ttyGS0) + NCM network (usb0).
# Identity (see tools/lib/kit_gadget.sh): 1d6b:0104, manufacturer "porting-kit",
# product "<port id>-<image>", serial = device.serial, so the host tells it apart from fastboot
# (18d1:d00d) and finds the right phone. Phone 10.0.42.1, the host gets 10.0.42.2 by DHCP.
[ -f /etc/kit-image ] && . /etc/kit-image
G=/sys/kernel/config/usb_gadget/g1
IP=10.0.42.1
CLIENT=10.0.42.2

# Whole seconds and /proc/mounts: fractional sleep and mountpoint are optional BusyBox features
wait_for() { i=0; while [ ! -e "$1" ] && [ $i -lt "$2" ]; do sleep 1; i=$((i+1)); done; [ -e "$1" ]; }

grep -q " /sys/kernel/config " /proc/mounts || mount -t configfs configfs /sys/kernel/config
wait_for /sys/kernel/config/usb_gadget 10 || { echo "usb-gadget: no configfs usb_gadget (CONFIG_USB_CONFIGFS)"; exit 1; }

# The UDC appears once the USB controller, its PHY and their regulators/extcon have probed
i=0
while [ -z "$(ls /sys/class/udc 2>/dev/null)" ] && [ $i -lt 30 ]; do sleep 1; i=$((i+1)); done
UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
[ -n "$UDC" ] || { echo "usb-gadget: no UDC after 30 s (USB controller, PHY or extcon did not probe)"; exit 1; }

mkdir -p $G
echo 0x1d6b > $G/idVendor
echo 0x0104 > $G/idProduct
echo 0x0100 > $G/bcdDevice
echo 0x0200 > $G/bcdUSB
mkdir -p $G/strings/0x409
echo "porting-kit" > $G/strings/0x409/manufacturer
echo "${KIT_PORT:-port}-${KIT_IMAGE:-test}" > $G/strings/0x409/product
echo "${KIT_SERIAL:-0}" > $G/strings/0x409/serialnumber
mkdir -p $G/configs/c.1/strings/0x409
echo "ACM+NCM" > $G/configs/c.1/strings/0x409/configuration
echo 250 > $G/configs/c.1/MaxPower
mkdir -p $G/functions/acm.usb0 && ln -s $G/functions/acm.usb0 $G/configs/c.1/
mkdir -p $G/functions/ncm.usb0
# Fixed MAC addresses: random ones per boot make the host take a minute to reconnect after reboots
echo "${KIT_DEV_ADDR:-02:4b:49:54:00:01}" > $G/functions/ncm.usb0/dev_addr
echo "${KIT_HOST_ADDR:-02:4b:49:54:00:02}" > $G/functions/ncm.usb0/host_addr
ln -s $G/functions/ncm.usb0 $G/configs/c.1/
echo "$UDC" > $G/UDC || { echo "usb-gadget: binding to $UDC failed"; exit 1; }

wait_for /sys/class/net/usb0 10 || { echo "usb-gadget: usb0 did not appear"; exit 1; }
ip addr add $IP/24 dev usb0
ip link set usb0 up
if command -v unudhcpd >/dev/null; then
	unudhcpd -i usb0 -s $IP -c $CLIENT >/dev/null 2>&1 &
else
	udhcpd -S /etc/udhcpd.conf
fi
echo "usb-gadget: $UDC bound, 1d6b:0104 ${KIT_PORT:-port}-${KIT_IMAGE:-test}, usb0 at $IP, host gets $CLIENT"
