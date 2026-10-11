# WiFi AHB host interface: port notes

Scope: `wlan/mt_wifi/wlan/os/linux/hif/ahb/**` and `wlan/Kbuild.hif`. All three objects (`arm.o`,
`ahb.o`, `mt8127/ahb_pdma.o`) compile on 6.18 ARM32 without warnings of their own; the other layers'
warnings (`-Wundef` on `CFG_*` in config.h) show up through the shared headers. The module does not
link until the core and glue layers are done.

## What changed against the import

- Vendor facilities removed: `mach/*` headers, `enable_clock`/`disable_clock`, EMI MPU, `hwPowerOn`,
  `mt_set_gpio_*`, `ioremap_nocache`, fixed virtual addresses (`0xF0000024`, `0xF0003018`,
  `0xF8070000`), PM `suspend/resume` with `pm_message_t`, `__exit_p`.
- `struct platform_driver` (`mt-wifi`, match `mediatek,mt8127-wlan`) now provides the resources:
  register windows (`devm_ioremap`, not claimed), IRQ (`platform_get_irq`), optional `apdma` clock.
  Its probe registers the WMT callbacks (`mtk_wcn_wmt_wlan_reg`), its remove unregisters them,
  removes a still-probed WiFi device and clears the resources.
- `HifAhbProbe()` (WMT callback, WiFi function on): switches the WiFi PA rail on
  (`mtk_wcn_consys_hw_wifi_paldo_ctrl(1)`), enables the AP DMA clock, runs `pfWlanProbe()`; undoes
  both when it fails. `HifAhbRemove()` runs `pfWlanRemove()` and switches both off. A mutex
  serializes both with the platform remove, and a probed flag makes remove idempotent.
- `glSetHifInfo()` takes the mapped windows from the platform resources; `glClearHifInfo()` only
  clears the pointers (the mappings live as long as the platform device).
- `glBusSetIrq()/glBusFreeIrq()` use the interrupt of the node, trigger type from the DT.
- DMA: `HifPdmaInit()` enables DMA only when the `apdma-hif` window exists; without it the HIF uses
  PIO. `dma_map_single()` results are checked. The register semantics of `ahb_pdma.c` are unchanged.
- `hif.h`: `MCU_REG_READL` returns 0 without the optional `mcu-config` window; new
  `glGetHifDevice()`; the static-VA `CONSYS_REG_READ`/`CONN_MCU_CONFIG_BASE` macros are gone (unused).

## DT binding summary

`mediatek,mt8127-wlan`; `reg-names`: `hif` (0x180F0000, 0x5c), optional `mcu-config`
(0x18070000, 0x200), optional `apdma-hif` (0x11000180, 0x54); one interrupt, SPI 184 level low;
optional `clocks`/`clock-names = "apdma"` (`CLK_PERI_AP_DMA`); optional `local-mac-address`.
Details and example: `docs/wlan-binding.md`. Requests to the other layers: `docs/wlan-hif-needs.md`.

## Assumptions not verifiable without hardware

- The MT8127 PDMA channel is `HIF_1` at 0x11000180. The vendor kernel has no `CONFIG_OF`, so stock
  used the non-OF value 0x11000180; the `0x11000080` of the `CONFIG_OF` branch is MT6752's and
  would be channel 0 (GDMA 1) on MT8127. The vendor `mt_dma.c` channel table (`0x80 * (n + 1)`,
  `HIF_1` = third entry) agrees.
- The AP DMA clock held on while the WiFi function is on is enough; stock enabled it around each
  transfer. If the clock must be gated for power, `HifPdmaClockCtrl()` is the place, but the clock
  API may sleep and the callers are not known to be sleepable.
- The EMI MPU region protection of the stock `HifPdmaInit()` is not applied (see
  `docs/consys-binding.md`: no EMI MPU driver, region is `no-map`).
- `mtk_wcn_consys_hw_wifi_paldo_ctrl()` is idempotent with respect to the WMT core's own PA control
  (consys keeps a per-rail state flag).
- The level-low interrupt is delivered through the sysirq polarity inversion of the default
  `interrupt-parent`; the handler masks the source (`WHLPCR_INT_EN_CLR`) before returning.
- `dma_map_single()` on the buffers handed to `kalDevPort*()` works with a 32-bit mask and the
  linear mapping; DMA bus addresses equal physical addresses (no IOMMU, no offset).

## Risks

- A buffer that is not cache-line aligned in the receive path can lose neighbouring data on ARM32
  (non-coherent `DMA_FROM_DEVICE` invalidation); the stock code has the same TODO.
- Polling for DMA completion runs up to 5 s with no sleep (stock behaviour); the debug branch
  (`CONF_HIF_CONNSYS_DBG == 1`) never aborts the wait and dumps CPUPCR 512 times.
- Without a node the module loads and does nothing: WMT finds no WiFi callbacks, and the WiFi
  function fails with "null pointer mtk_wcn_wlan_probe".
- If the WMT core powers the WiFi function off while the module is unloading, `HifAhbPltmRemove()`
  removes the device first; the WMT bookkeeping (`WMT_WIFI_ON`) is then not cleared by a normal
  func-off.
