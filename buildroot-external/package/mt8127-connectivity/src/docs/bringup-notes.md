# Bring-up notes

## Hardware facts (stock 3.10.54 sources)

- The WiFi is the on-die CONSYS subsystem (chip id 0x8127, analog die MT6627). The MCU is controlled with WMT commands in
  STP frames over **BTIF** (0x1100C000, SPI 50, APDMA TX/RX SPI 71/72). The WiFi data path is a memory-mapped **AHB HIF** at
  0x180F0000 (SPI 184), with DMA (PDMA 0x11000080 or GDMA 0x11000100). `CONN_MCU_CONFIG` is at 0x18070000.
- BGF wake-up interrupt: SPI 185. CONN watchdog: SPI 163.
- Power: MT6323 VCN18 (`DIGLDO_CON11` bit 14), VCN28 (`ANALDO_CON19` bit 12), VCN33_WIFI (`ANALDO_CON17` bit 12) and
  VCN33_BT (`ANALDO_CON16` bit 7) switched when the functions start. Power domain CONN: `SPM_CONN_PWR_CON` at 0x10006280
  (RST_B b0, ISO b1, ON b2, ON_S b3, CLK_DIS b4, SRAM_PDN b8), status at 0x1000660c/0x10006610 bit 1, bus protection
  `TOPAXI_PROT_EN` 0x10001220, mask 0x104. Clock gate: INFRA gate bit 12 (`INFRA_PDN_CLR` 0x10001044).
- A 1 MiB region of RAM, 1 MiB aligned, is used for the firmware handshake and dumps; INFRA+0x1310 =
  `(phys >> 20) | 0x1000` maps it for the CONSYS MCU. The vendor kernel also protects the upper half with an EMI MPU.
- No GPIOs are involved for WiFi (no PMU_EN or reset pin).
- Firmware: `ROMv2_patch_1_0_hdr.bin` and `ROMv2_patch_1_1_hdr.bin` (selected by hardware version, 0x8A00 and 0x8A01),
  `WIFI_RAM_CODE_8127`, `WMT_SOC.cfg`. The WiFi MAC address comes from the stock NVRAM partition.

## Milestone 1 (2026-10-10): CONSYS powered, chip id read

`tools/consys-poweron.sh` run on the tablet (kernel 6.18.55) with every rail and the power domain off before: afterwards
`CONN_MCU_CONFIG+0x08` reads `0x00008127`. The bus protection status stays clear, so the sequence does not need the
bus-protect handshake that other domains have.

## Milestone 2 (2026-10-10): BTIF/STP link and ROM patch download

`tools/test-wmt.sh` reboots the tablet, loads `mt8127_consys`, `mtk_btif`, `mtk_stp_wmt_soc` and the debugfs switch
`wmt_wifi_ctl`, powers the WiFi function and saves the kernel log (`WMT_ARGS="dbg_level=2 btif_trace=1"` adds the WMT
log and a trace of every BTIF buffer). Result: the chip answers over STP (chip id 0x8127, hardware version 0x8a01 = E2,
ROM version 0x8a00), both ROM patch parts download and the firmware reports patch version 20150918. The WMT core then
asks for the WiFi function and stops at `null pointer mtk_wcn_wlan_probe`, the callback the WiFi driver registers.

Findings that cost time:

- **Patch parts.** The stock userspace launcher scanned `/lib/firmware` for `ROMv2_patch_*` files whose header firmware
  version (big-endian u16 at offset 22) matches the ROM version, and handed each to the driver with its sequence and
  address. Header bytes 24..27: low nibble of byte 24 = part sequence, high nibble = part count, bytes 25..27 = address of
  the part (byte 24 is cleared). For E2 both files are parts of one patch: sequence 1 is `ROMv2_patch_1_1_hdr.bin` at
  0xf00e0000 (inside the shared 1 MiB region, so the region must be mapped for the MCU), sequence 2 is
  `ROMv2_patch_1_0_hdr.bin` at 0x00060000. A misaligned address made the ROM abort after the last fragment.
- **Reading the firmware's own trace.** After an assert the firmware leaves its register dump in the shared region;
  `/sys/kernel/debug/mt8127_consys/emi` and `emi_full` expose it (`exception type: ABT`, registers). The faulting register
  matched the patch address, which gave the clue.
- **Method.** `btif_trace=1` logs the STP traffic, which is how the exchange was followed frame by frame.
