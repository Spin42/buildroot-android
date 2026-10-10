# mediatek,mt8127-consys

Device-tree binding of the `mt8127_consys` platform driver (`consys/`). The node describes the
CONSYS (WiFi/BT/GPS/FM combo subsystem) power sequencing, the shared-memory handshake window and
the wake IRQ. The WMT core (`mtk_stp_wmt_soc`), BTIF and the WiFi driver find their own resources
separately; they call into this driver through the exported `mtk_wcn_consys_hw_*`, `wmt_plat_*` and
`mtk_wcn_cmb_stub_*` functions.

## Properties

| Property | Description |
|---|---|
| `compatible` | `"mediatek,mt8127-consys"` |
| `reg`, `reg-names` | Two entries. `"spm"`: SPM block, 0x10006000 size 0x1000 (power domain control and status; mapped without claiming the region because other drivers may own it). `"mcu-config"`: CONN_MCU_CONFIG, 0x18070000 size 0x200 (chip id at +0x08, CPU PC capture at +0x160). |
| `mediatek,infracfg` | Phandle to the INFRACFG_AO syscon (0x10001000, size 0x1000). Used for: bus protection (0x220/0x228), CONNMCU clock gate (0x40 set / 0x44 clear, bit 12), EMI mapping register (0x310), AP2CONN wake register (0x800). |
| `interrupts`, `interrupt-names` | `"bgf"` (required): the BGF wake IRQ (CONN2AP_BTIF_WAKEUP), SPI 185, level low. Requested by the driver on demand; the trigger type comes from this property. `"wifi"` (SPI 184) and `"watchdog"` (SPI 163) may be listed for documentation; the driver does not request them. |
| `memory-region` | Phandle to a `reserved-memory` node, `no-map`, 1 MiB aligned, at least 1 MiB. Carries the firmware handshake and the coredump area. |
| `vcn18-supply` | MT6323 `vcn18`, 1.8 V digital rail, enabled at power-on. |
| `vcn28-supply` | MT6323 `vcn28`, 2.8 V analog rail, enabled at power-on (or on demand in co-clock mode). |
| `vcn33-wifi-supply` | MT6323 `vcn33_wifi`, 3.3 V WiFi PA rail, enabled when the WiFi function starts. |
| `vcn33-bt-supply` | MT6323 `vcn33_bt`, 3.3 V BT PA rail, enabled when the BT function starts. |

No GPIO is used: the SoC-integrated radio has no PMU_EN or reset pin.

The `vcn18` regulator is switched out of low-power mode before it is enabled. That needs
`regulator-allowed-modes` / mode-change permission on the regulator node; when the regulator rejects
the mode change the power-up continues with the rail's default mode.

## Reserved memory

The firmware handshake and the coredump use the window `base + 0x80000 .. +343 KiB` of the 1 MiB
region. The base must be 1 MiB aligned because the CONSYS MCU sees the region through a 1 MiB window
programmed as `(phys >> 20) | BIT(12)` into INFRACFG_AO + 0x310 (stock `INFRA_BASE + 0x1310`, with
`INFRA_BASE` = 0x10000000). The stock EMI MPU protection of the upper 512 KiB is not applied: the
mainline kernel has no EMI MPU driver and the region is `no-map`, so the kernel never touches it.

## Example

```dts
reserved-memory {
	#address-cells = <2>;
	#size-cells = <2>;
	ranges;

	consys_mem: consys@bc000000 {
		reg = <0 0xbc000000 0 0x100000>;
		no-map;
	};
};

infracfg: syscon@10001000 {
	compatible = "mediatek,mt8127-infracfg", "syscon";
	reg = <0 0x10001000 0 0x1000>;
};

consys: consys@18070000 {
	compatible = "mediatek,mt8127-consys";
	reg = <0 0x10006000 0 0x1000>,
	      <0 0x18070000 0 0x200>;
	reg-names = "spm", "mcu-config";
	mediatek,infracfg = <&infracfg>;
	interrupts = <GIC_SPI 185 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 184 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 163 IRQ_TYPE_LEVEL_LOW>;
	interrupt-names = "bgf", "wifi", "watchdog";
	memory-region = <&consys_mem>;
	vcn18-supply = <&mt6323_vcn18_reg>;
	vcn28-supply = <&mt6323_vcn28_reg>;
	vcn33-wifi-supply = <&mt6323_vcn33_wifi_reg>;
	vcn33-bt-supply = <&mt6323_vcn33_bt_reg>;
};
```

The regulator labels are the MT6323 regulator nodes (`vcn18`, `vcn28`, `vcn33_wifi`, `vcn33_bt`).
The node needs no `status` handling beyond the usual `okay`. The 1 MiB region must not overlap the
system RAM the kernel uses (carve it out of the end of RAM, for example below the framebuffer).

## debugfs

`/sys/kernel/debug/mt8127_consys/power` (write 1 or 0) runs the stock power-on or power-off sequence
(regulators, power domain, clock, chip-id poll) without the WMT core; `chipid` reads
CONN_MCU_CONFIG + 0x08 while powered (`0x00008127` expected). Do not use it while the WMT core is
running.

## Differences from the stock driver

- Regulators are controlled through the regulator API. The PMIC "HW/SW control mode" bits of VCN28
  and VCN33 (`upmu_set_vcn28_on_ctrl`, `upmu_set_vcn33_on_ctrl_*`) are not reachable and are not
  touched; the rails are plain software-enabled.
- The power domain code uses bounded polling and the same register sequence as
  `spm_mtcmos_ctrl_connsys()`. Bus protection status is polled with a timeout and does not fail the
  sequence.
- Power-up fails with `-ENODEV` and unwinds when the chip id is not 0x8127, 0x6582 or 0x6572.
- The MCU JTAG pin setup (`mtk_wcn_consys_jtag_flag_ctrl`) records the flag only.
- No IPOH/hibernate restore callback and no CONSYS sleep-mode (`osc_en`) control.
- `mt_combo_plt_*_deep_idle` accept every interface and do nothing.
