# Camera hardware of the TB3-710F

## Sensors (identified on the tablet, 2026-10-10, `tools/camera-probe.sh`)

| Slot | Sensor | I2C0 address | Chip id | Interface | Output |
|---|---|---|---|---|---|
| rear (main) | SP2508 | 0x3c | 0x2508 (regs 0x02/0x03 after writing 0xfd = 0) | MIPI CSI-2, 1 lane | raw Bayer, 1600x1200 |
| front (sub) | SP0A20 | 0x21 | 0x2b (reg 0x02 after writing 0xfd = 0) | MIPI CSI-2 | YUV (UYVY), 632x474 |

The other candidates of the stock configuration (GC2355 rear, GC0310 front) read zero: not fitted. No lens driver answers
at 0x18 (fixed focus). Mainline has no driver for either fitted sensor (`gc0310`, `gc0308`, `gc2145` exist for other parts).

## Power and control (stock `kd_camera_hw.c`; verified by the probe)

- Rails (MT6323): VCAMA 2.8 V (`ANALDO_CON4` bit 15, voltage in `ANALDO_CON10` bits 6:5 = 3), VCAM_IO 1.8 V (`DIGLDO_CON53` bit 14).
- GPIOs: main RST 53 (active low), main PDN 56 (active high); sub RST 55, sub PDN 54. MCLK pad GPIO119 function 1.
- Both sensors share one MCLK and one CSI receiver; the other slot is held in power down with reset asserted.
- SP2508 sequence: PDN 0, RST 0, VCAMA, VCAM_IO, MCLK on, PDN pulse (0 1 0 with 100 ms high), RST 1.
- SP0A20 sequence: PDN 0, VCAMA, VCAM_IO, MCLK on, PDN pulse (0 1 0 with 100 ms high).

## Clocks (docs/seninf-mipi.md)

ISP power domain `SPM+0x238`; IMG clock gates `0x15000008` (clear bits 0, 5, 6, 7, 8); camtg mux `CLK_CFG_1` = 0x10000050
bits 26:24 = 1 (48 MHz) with bit 31 cleared; MCLK 24 MHz from `0x15008304` = 0x00010001 and `0x15008300` bit 29 + PCEN.
