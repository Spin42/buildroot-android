#!/bin/sh
# Probe the camera sensors of the MT8127 tablet from a shell: power the ISP domain, open the camera clock gates,
# generate the 24 MHz MCLK, then run the stock power-up sequence of each candidate sensor and read its chip id on I2C0.
# Needs devmem, tb3-pmic (package tb3-tools) and i2c-tools. Sequences: stock kd_camera_hw.c; clock recipe: docs/seninf-mipi.md.
# usage: camera-probe.sh [gc2355|sp2508|gc0310|sp0a20|all]
BUS=${BUS:-0}

rd() { devmem "$1" 32; }
wr() { devmem "$1" 32 "$2" >/dev/null; }
rmw() { v=$(rd "$1"); wr "$1" $(( (v & ~$2) | $3 )); }
pmic_rmw() { v=$(tb3-pmic read "$1" | sed 's/.*: //'); tb3-pmic write "$1" "$(printf 0x%04x $(( (v & ~$2) | $3 )))" >/dev/null; }
ms() { usleep $(( $1 * 1000 )); }

# GPIO output level through the SET/CLR registers (blocks of 16 pins, DOUT at 0x10005400)
gpio() { b=$(( 0x10005400 + ($1 / 16) * 0x10 )); bit=$(( 1 << ($1 % 16) )); if [ "$2" = 1 ]; then wr $((b + 4)) $bit; else wr $((b + 8)) $bit; fi; }
MAIN_RST=53; MAIN_PDN=56; SUB_RST=55; SUB_PDN=54

isp_power_on() {
	PWR=0x10006238
	if [ $(( $(rd 0x1000660c) & 0x20 )) -ne 0 ]; then return; fi
	rmw $PWR 0 0x4; rmw $PWR 0 0x8
	i=0; while [ $i -lt 100 ]; do
		[ $(( $(rd 0x1000660c) & 0x20 )) -ne 0 ] && [ $(( $(rd 0x10006610) & 0x20 )) -ne 0 ] && break; i=$((i + 1)); done
	rmw $PWR 0x10 0; rmw $PWR 0x2 0; rmw $PWR 0 0x1; rmw $PWR 0xf00 0
	i=0; while [ $i -lt 100 ] && [ $(( $(rd $PWR) & 0x3000 )) -ne 0 ]; do i=$((i + 1)); done
}

clocks_on() {
	wr 0x15000008 0x1e1                        # IMG_CG_CLR: LARB2, CAM_SMI, CAM_CAM, SEN_TG, SEN_CAM
	rmw 0x10000050 0x87000000 0x01000000       # camtg mux = univpll_d26 (48 MHz), not powered down
	rmw 0x15008000 0 0x400                     # SENINF_TOP_CTRL
	wr 0x15008304 0x00010001                   # TG1 phase counter: divide 48 MHz by 2
	v=$(rd 0x15008300); wr 0x15008300 $(( (v & ~0xb0000047) | 0x80000001 ))   # PCEN, MCLK still gated (bit 29 = 0)
	rmw 0x15004410 0 0x1
}
mclk() { if [ "$1" = 1 ]; then rmw 0x15008300 0 0x20000000; else rmw 0x15008300 0x20000000 0; fi; }

vcamio() { if [ "$1" = 1 ]; then pmic_rmw 0x055e 0 0x4000; else pmic_rmw 0x055e 0x4000 0; fi; }
vcama() { if [ "$1" = 1 ]; then pmic_rmw 0x0412 0x60 0x60; pmic_rmw 0x0408 0 0x8000; else pmic_rmw 0x0408 0x8000 0; fi; }

all_off() { mclk 0; vcama 0; vcamio 0; gpio $MAIN_PDN 1; gpio $MAIN_RST 0; gpio $SUB_PDN 1; gpio $SUB_RST 0; }

# the other slot is held in power down and reset while one is probed
hold_sub() { gpio $SUB_PDN 1; gpio $SUB_RST 0; }
hold_main() { gpio $MAIN_PDN 1; gpio $MAIN_RST 0; }

i2c_rd() { i2ctransfer -y $BUS w1@$1 $2 r1@$1 2>&1 | tr -d '\n'; }
i2c_wr() { i2ctransfer -y $BUS w2@$1 $2 $3 >/dev/null 2>&1; }

probe_gc2355() {
	hold_sub; mclk 0; ms 5; gpio $MAIN_PDN 1; ms 1; gpio $MAIN_RST 0; ms 10; vcamio 1; ms 1; vcama 1; ms 2
	mclk 1; ms 5; gpio $MAIN_PDN 0; ms 1; gpio $MAIN_RST 1; ms 20
	echo "gc2355 @0x3c: id=$(i2c_rd 0x3c 0xf0)$(i2c_rd 0x3c 0xf1) (expect 0x23 0x55)"
}
probe_sp2508() {
	hold_sub; mclk 0; gpio $MAIN_PDN 0; ms 1; gpio $MAIN_RST 0; ms 5; vcama 1; ms 5; vcamio 1; ms 1; mclk 1; ms 2
	gpio $MAIN_PDN 0; ms 1; gpio $MAIN_PDN 1; ms 100; gpio $MAIN_PDN 0; ms 20; gpio $MAIN_RST 1; ms 5
	i2c_wr 0x3c 0xfd 0x00
	echo "sp2508 @0x3c: id=$(i2c_rd 0x3c 0x02)$(i2c_rd 0x3c 0x03) (expect 0x25 0x08)"
}
probe_gc0310() {
	hold_main; mclk 0; ms 5; gpio $SUB_PDN 0; ms 1; vcamio 1; ms 1; vcama 1; ms 1; mclk 1; ms 5
	gpio $SUB_PDN 0; ms 1; gpio $SUB_PDN 1; ms 1; gpio $SUB_PDN 0; ms 20
	echo "gc0310 @0x21: id=$(i2c_rd 0x21 0xf0)$(i2c_rd 0x21 0xf1) (expect 0xa3 0x10)"
}
probe_sp0a20() {
	hold_main; mclk 0; gpio $SUB_PDN 0; ms 1; vcama 1; ms 5; vcamio 1; ms 5; mclk 1; ms 5
	gpio $SUB_PDN 0; ms 1; gpio $SUB_PDN 1; ms 100; gpio $SUB_PDN 0; ms 5
	i2c_wr 0x21 0xfd 0x00
	echo "sp0a20 @0x21: id=$(i2c_rd 0x21 0x02) (expect 0x2b)"
}

isp_power_on; clocks_on
echo "ISP power status: $(rd 0x1000660c)  IMG_CG: $(rd 0x15000000)  CLK_CFG_1: $(rd 0x10000050)  TG300: $(rd 0x15008300)"
for s in ${1:-all}; do
	case $s in
		gc2355) all_off; probe_gc2355;;
		sp2508) all_off; probe_sp2508;;
		gc0310) all_off; probe_gc0310;;
		sp0a20) all_off; probe_sp0a20;;
		all) for t in gc2355 sp2508 gc0310 sp0a20; do all_off; ms 50; probe_$t; done;;
	esac
done
all_off
