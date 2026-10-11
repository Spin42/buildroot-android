# mediatek,mt8127-wlan

Device-tree binding of the AHB host interface of the gen2 WiFi driver (`wlan_mt`, layer
`wlan/mt_wifi/wlan/os/linux/hif/ahb`). The WiFi MAC is reached through the memory-mapped
CONN_HIF_CR block, with DMA assistance from one channel of the AP DMA block. The platform driver
bound to this node maps the windows and looks up the interrupt and clock when it probes; the WiFi
function itself is brought up by the WMT core (`mtk_wcn_wmt_func_on`), which calls the HIF probe
callback once the CONSYS is powered and patched.

## Properties

| Property | Description |
|---|---|
| `compatible` | `"mediatek,mt8127-wlan"` |
| `reg`, `reg-names` | `"hif"` (required): CONN_HIF_CR, 0x180F0000 size 0x5c. `"mcu-config"` (optional): CONN_MCU_CONFIG, 0x18070000 size 0x200; only used to dump the CPU program counter (offset 0x160) when a DMA transfer times out. `"apdma-hif"` (optional): AP DMA channel HIF_1, 0x11000180 size 0x54; without it the driver moves data with PIO instead of DMA. |
| `interrupts` | One entry, the WF_HIF interrupt: SPI 184, `IRQ_TYPE_LEVEL_LOW`. The trigger type of the property is used as is. |
| `clocks`, `clock-names` | Optional. `"apdma"`: `CLK_PERI_AP_DMA` of the PERICFG clock controller. Enabled when the WiFi function starts and disabled when it stops. |
| `local-mac-address` | Optional, 6 bytes. Read by the glue layer through `glGetHifDevice()`. |

All windows are mapped with `devm_ioremap()` and never claimed: the consys driver maps
0x18070000 itself and the AP DMA block is shared with the I2C and BTIF channels (channel `n` of
the block is at `0x11000000 + 0x80 * (n + 1)`; `HIF_1` is channel 2).

The node must be enabled together with the `mediatek,mt8127-consys` node (power, handshake memory,
VCN33 WiFi PA rail) and the WMT core; this node carries no supplies.

## Example

```dts
wifi: wifi@180f0000 {
	compatible = "mediatek,mt8127-wlan";
	reg = <0 0x180f0000 0 0x5c>,
	      <0 0x18070000 0 0x200>,
	      <0 0x11000180 0 0x54>;
	reg-names = "hif", "mcu-config", "apdma-hif";
	interrupts = <GIC_SPI 184 IRQ_TYPE_LEVEL_LOW>;
	clocks = <&pericfg CLK_PERI_AP_DMA>;
	clock-names = "apdma";
	/* local-mac-address = [00 00 00 00 00 00]; */
};
```

Place it next to the `consys` node under the SoC bus node (it uses the same two-cell address and
size layout).
