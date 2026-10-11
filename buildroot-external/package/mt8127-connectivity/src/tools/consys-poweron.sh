#!/bin/sh
# Power the MT8127 CONSYS block from a shell on the tablet and print its chip id (expect 0x00008127).
# Needs devmem and tb3-pmic (package tb3-tools). Mirrors mtk_wcn_consys_hw_reg_ctrl() and
# spm_mtcmos_ctrl_connsys() of the stock kernel.
pmic_rmw() { v=$(tb3-pmic read $1 | sed 's/.*: //'); tb3-pmic write $1 "$(printf 0x%04x $(( (v & ~$2) | $3 )))" >/dev/null; }
rmw() { v=$(devmem $1 32); devmem $1 32 $(( (v & ~$2) | $3 )) >/dev/null; }

pmic_rmw 0x0512 0x2 0x4000      # VCN18: LP mode off, enable
usleep 1000
pmic_rmw 0x041c 0 0x1000        # VCN28: enable

PWR=0x10006280                  # SPM_CONN_PWR_CON
rmw $PWR 0 0x4                  # PWR_ON
rmw $PWR 0 0x8                  # PWR_ON_S
i=0
while [ $i -lt 100 ]; do        # SPM_PWR_STATUS and _S, bit 1 = CONN
	[ $(( $(devmem 0x1000660c 32) & 2 )) -ne 0 ] && [ $(( $(devmem 0x10006610 32) & 2 )) -ne 0 ] && break
	i=$((i + 1))
done
rmw $PWR 0x10 0                 # CLK_DIS off
rmw $PWR 0x2 0                  # ISO off
rmw $PWR 0 0x1                  # RST_B
rmw $PWR 0x100 0                # SRAM_PDN off
rmw 0x10001220 0x104 0          # release the bus protection (TOPAXI_PROT_EN)
devmem 0x10001044 32 0x1000     # INFRA_PDN_CLR: open the CONNMCU clock gate (bit 12)
usleep 20000
echo "CONN_MCU chip id: $(devmem 0x18070008 32)"
