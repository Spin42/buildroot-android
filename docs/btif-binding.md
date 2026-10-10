# MT8127 BTIF device tree binding

The BTIF block is the AP-side UART-like link to the CONSYS MCU. WMT commands,
wrapped in STP frames, travel over it. The `mtk_btif` module binds to it and
exports the `mtk_wcn_btif_*` API used by the WMT/STP layer.

## Node

| Property | Description |
|----------|-------------|
| `compatible` | `"mediatek,mt8127-btif"` |
| `reg` | Required entry `btif`; entries `tx_dma` and `rx_dma` are needed for DMA mode |
| `reg-names` | `"btif"`, `"tx_dma"`, `"rx_dma"` |
| `interrupts` | One entry per name in `interrupt-names`, trigger type included |
| `interrupt-names` | `"btif"`; `"tx_dma"` and `"rx_dma"` for DMA mode |
| `clocks` | PERICFG gates, see below |
| `clock-names` | `"btif"` (optional) and `"apdma"` |

### reg

| Name | Block | Address (MT8127) | Size |
|------|-------|------------------|------|
| `btif` | BTIF controller | `0x1100c000` | `0x1000` |
| `tx_dma` | APDMA BTIF Tx virtual-FIFO channel (APDMA base `0x11000000` + `0x780`) | `0x11000780` | `0x80` |
| `rx_dma` | APDMA BTIF Rx virtual-FIFO channel (APDMA base `0x11000000` + `0x800`) | `0x11000800` | `0x80` |

The APDMA channels are mapped without claiming the region, because other
drivers (I2C) own neighbouring channels of the same APDMA block.

### interrupts

| Name | SPI | Notes |
|------|-----|-------|
| `btif` | 50 | PIO Tx/Rx interrupt |
| `tx_dma` | 71 | APDMA Tx channel |
| `rx_dma` | 72 | APDMA Rx channel |

All three are level-low on the vendor kernel (`IRQ_TYPE_LEVEL_LOW` through the
SYSIRQ). The trigger type of the DT entry is passed to `request_irq()`.

### clocks

| Name | Provider | Notes |
|------|----------|-------|
| `btif` | PERICFG BTIF gate | Optional. When absent the driver warns and assumes the gate is open. |
| `apdma` | PERICFG `CLK_PERI_AP_DMA` | Shared by both DMA channels. Optional; when absent the driver warns and assumes the gate is open. Not requested in PIO mode. |

Both gates are prepared once at probe and then enabled/disabled with reference
counting from atomic context (IRQ handlers and the Tx path).

### Operating mode

DMA mode (Tx and Rx through APDMA, the normal STP configuration) is used when
the `tx_dma`/`rx_dma` registers and interrupts are all present. If any is
missing the driver falls back to PIO for both directions. The module parameter
`force_pio=1` selects PIO regardless of the node, for bring-up. In PIO mode only
the `btif` register block and interrupt are used.

The vFIFO memory (8 KiB per direction) is allocated with `dma_alloc_coherent()`
from the platform device, with a 32-bit DMA mask.

## Example

```dts
btif: btif@1100c000 {
	compatible = "mediatek,mt8127-btif";
	reg = <0 0x1100c000 0 0x1000>,
	      <0 0x11000780 0 0x80>,
	      <0 0x11000800 0 0x80>;
	reg-names = "btif", "tx_dma", "rx_dma";
	interrupts = <GIC_SPI 50 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 71 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 72 IRQ_TYPE_LEVEL_LOW>;
	interrupt-names = "btif", "tx_dma", "rx_dma";
	clocks = <&pericfg CLK_PERI_AP_DMA>;
	clock-names = "apdma";
};
```

Once the PERICFG driver exposes the BTIF gate, add it as `<&pericfg CLK_PERI_BTIF>`
with `clock-names = "btif", "apdma"` (clocks and names stay in the same order).
