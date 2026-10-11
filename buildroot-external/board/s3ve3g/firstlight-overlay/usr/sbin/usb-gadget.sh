#!/bin/sh
# USB gadget for first light, through configfs: ACM serial (ttyGS0) + NCM network (usb0).
# 1d6b:0104 (Linux Foundation multifunction composite gadget), product "s3ve3g-firstlight",
# so the host can tell it apart from lk2nd fastboot (18d1:d00d) and the rescue gadget (18d1:d001).
# The UDC appears once the ChipIdea controller has probed, which needs the SM5502 extcon and the
# RPM regulators of the PHY: wait for it.
G=/sys/kernel/config/usb_gadget/g1
IP=10.0.42.1

wait_for() { i=0; while [ ! -e "$1" ] && [ $i -lt "$2" ]; do sleep 0.5; i=$((i+1)); done; [ -e "$1" ]; }

mountpoint -q /sys/kernel/config || mount -t configfs configfs /sys/kernel/config
wait_for /sys/kernel/config/usb_gadget 20 || { echo "usb-gadget: no configfs usb_gadget"; exit 1; }

i=0
while [ -z "$(ls /sys/class/udc 2>/dev/null)" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i+1)); done
UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
[ -n "$UDC" ] || { echo "usb-gadget: no UDC after 30 s (ChipIdea, PHY or extcon did not probe)"; exit 1; }

mkdir -p $G
echo 0x1d6b > $G/idVendor
echo 0x0104 > $G/idProduct
echo 0x0100 > $G/bcdDevice
echo 0x0200 > $G/bcdUSB
mkdir -p $G/strings/0x409
echo "porting-kit" > $G/strings/0x409/manufacturer
echo "s3ve3g-firstlight" > $G/strings/0x409/product
echo "31cf1659" > $G/strings/0x409/serialnumber
mkdir -p $G/configs/c.1/strings/0x409
echo "ACM+NCM" > $G/configs/c.1/strings/0x409/configuration
echo 250 > $G/configs/c.1/MaxPower
mkdir -p $G/functions/acm.usb0 && ln -s $G/functions/acm.usb0 $G/configs/c.1/
mkdir -p $G/functions/ncm.usb0 && ln -s $G/functions/ncm.usb0 $G/configs/c.1/
echo "$UDC" > $G/UDC || { echo "usb-gadget: binding to $UDC failed"; exit 1; }

wait_for /sys/class/net/usb0 20 || { echo "usb-gadget: usb0 did not appear"; exit 1; }
ip addr add $IP/24 dev usb0
ip link set usb0 up
mkdir -p /tmp
udhcpd -S /etc/udhcpd.conf
echo "usb-gadget: $UDC bound, 1d6b:0104, usb0 at $IP, DHCP gives the host 10.0.42.2"
