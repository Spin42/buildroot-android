# Staged capture prototype (`mtk_cam_proto`)

Out-of-tree module in `cam/` that proves the camera hardware path on mainline before the V4L2 drivers exist. It
drives the stock register programming from `docs/hardware.md`, `docs/seninf-mipi.md` and `docs/isp-capture.md`
through debugfs, one stage at a time, and captures one frame into a DMA buffer. The register code is organised
by block (`cam_power.c`, `cam_seninf.c`, `cam_isp.c`) so that it can move into the V4L2 drivers.

Build: `make -k -j8` in the repository root produces `cam/mtk_cam_proto.ko` (KDIR defaults to the Buildroot
kernel build tree).

## Device tree node

Add to the board DTS (the `soc` node uses two address cells; `i2c0`, `pio` and the MT6323 regulators are those
of `mt8127-lenovo-tb3-710f.dts`):

```dts
/ {
	soc {
		cam_proto: camera@15004000 {
			compatible = "mediatek,mt8127-cam-proto";
			reg = <0 0x15000000 0 0x1000>,
			      <0 0x15004000 0 0x4000>,
			      <0 0x15008000 0 0x1000>,
			      <0 0x1500c000 0 0x1000>,
			      <0 0x10010800 0 0x100>,
			      <0 0x10006000 0 0x1000>,
			      <0 0x10000000 0 0x1000>,
			      <0 0x10005000 0 0x1000>;
			reg-names = "imgsys", "isp", "seninf", "mipi-rx",
				    "mipi-ana", "spm", "topckgen", "gpio";
			interrupts = <GIC_SPI 143 IRQ_TYPE_LEVEL_LOW>;
			vcama-supply = <&mt6323_vcama_reg>;
			vcamio-supply = <&mt6323_vcamio_reg>;
			main-reset-gpios = <&pio 53 GPIO_ACTIVE_HIGH>;
			main-powerdown-gpios = <&pio 56 GPIO_ACTIVE_HIGH>;
			sub-reset-gpios = <&pio 55 GPIO_ACTIVE_HIGH>;
			sub-powerdown-gpios = <&pio 54 GPIO_ACTIVE_HIGH>;
			i2c-bus = <&i2c0>;
		};
	};
};
```

Notes:

- All GPIOs are `GPIO_ACTIVE_HIGH`: the sequences use physical levels (reset is active low, power-down active
  high), exactly as `tools/camera-probe.sh`. The module requests them in the off state (power-down 1, reset 0).
- Optional extra window `emi` (`reg-names` entry, `<0 0x10203000 0 0x1000>`): when present, stage 3 sets
  `EMI+0x120 |= 0x3f` before the ISP reset like the stock `ISP_Reset` (bandwidth limiter for the TG). Left out
  by default.
- The windows are mapped with `devm_ioremap`, without claiming the regions. The SPM, TOPCKGEN and GPIO accesses
  are read-modify-write. The `isp` window is 0x4000 bytes (the stock userspace maps 0x4000..0x7fff for ISP).
- Do not bind other drivers to the sensors at I2C0 0x3c / 0x21 while this module runs: it talks to the adapter
  with raw `i2c_transfer`.
- Pins `GPIO100..119` (MIPI pads, CMMCLK) are muxed by writing the GPIO mode registers directly, as the stock
  code does; no pinctrl state is needed.

## Usage

```sh
insmod mtk_cam_proto.ko            # parameters below can be given here or in /sys/module/mtk_cam_proto/parameters
cd /sys/kernel/debug/mt8127_cam
echo sp2508 > sensor               # or sp0a20
echo 1 > stage ; dmesg | tail -20  # power + sensor id
echo 2 > stage ; cat status        # sensor programmed, CSI-2 receiving
echo 3 > stage ; cat status        # ISP reset and pass-1 configured
echo 4 > stage ; cat status        # one frame captured
cat frame_info
cat frame > /tmp/frame.bin
echo 0 > stage                     # power everything down
```

Files:

| File | Meaning |
|---|---|
| `stage` | write 0..4. Stages run cumulatively. Writing a stage at or below the current one powers down and restarts from stage 1. Reads the current stage. |
| `sensor` | `sp2508` (rear, I2C0 0x3c, CSI0) or `sp0a20` (front, 0x21, CSI1). Only while at stage 0. |
| `status` | key registers (below). At stage 0 it shows the post-mortem snapshot of the last failed stage. |
| `frame`, `frame_info` | captured buffer (stays readable after `stage 0`) and its geometry. |
| `capture` | write 1 at stage 4 to capture another frame without powering down. |
| `path_regs` | the pass-1 SET/CLR images of the selected sensor. Write `EN1_SET 0x40001001` style lines (names EN1, EN2, DMA, FMT, CTL, MUX, MUX2, CONF) before stage 3 (DMA before stage 4). |
| `reg` | write `<window> <offset>` to read or `<window> <offset> <value>` to write a register of any mapped window (names as `reg-names`); result in dmesg and on read. Needs stage 1 or later. |

Any failing stage dumps a snapshot (readable in `status`), logs the error, and powers down.

## Stages

**Stage 1, power.** ISP power domain (SPM `PWR_CON` sequence, left untouched when it is already on), IMG clock
gates (`IMG_CG_CLR = 0x1e1`, verified to read back clear), camtg mux to 48 MHz, TG phase counter for 24 MHz,
GPIO119 CMMCLK function and drive strength, then the sensor's rail and GPIO sequence from the probe script and
a chip-id read over I2C. Expected log: `ISP power domain on`, `clocks on: IMG_CG 0x0 ... PH_CNT 0xa0000001`,
`chip id 0x2508 (expected 0x2508)` (SP0A20: `0x2b`). Failures: domain timeout means the SPM sequence or the
status bit is wrong; gates not clearing means the IMGSYS window is not alive; a NACK means the rails/reset/MCLK
sequence or the bus is wrong (compare with `tools/camera-probe.sh`).

**Stage 2, sensor and receiver.** Analog PHY, pin mux, lane map and calibration pulse (`initTg1CSI2(1)`), CSI-2
control/settle (`setTg1CSI2`), the sensor init table (SP2508: init + flip/mirror, optional colour bar with
`test_pattern=1`; SP0A20: init), SENINF1 reset, routing and CSI-2 again, `SENINF1_INTEN = 0x7f`, a wait of
`stream_wait_ms`, then a report. No ISP register beyond the clock setup is touched. This proves reception:

- `MIPI RX ready: STA0 ... STA1 ...` should satisfy `STA0 & 0x10101` and `STA1 & 0x101` (the stock code only
  logs when not).
- `SENINF1 detected size` (`seninf+0x2c`) should show non-zero lines and pixels matching the sensor (the exact
  encoding of the two halves is [I]); `NCSI2 error flags` (masked `0xfb8`) should be 0; `SENINF1_INTSTA` bit 0 set
  means FIFO overrun. Read `status` twice: a size that changes between reads means frames are being counted.

Failures: size 0 and error flags 0 means no data reaches the receiver (sensor not streaming, wrong port or lane
map; try `port=1`/`port=0`, `lanes=`); error flags set with a size means a timing/format mismatch (`csi2_settle`,
`csi2_ctrl`, `csi2_ecc_order`).

**Stage 3, ISP pass 1 without DMA.** Optional EMI limiter, `ISP_Reset` sequence (bit 1 is polled, bounded), pass-1
defaults, TG (grab window, sensor mode, TG format, view-finder mode, CMOS enable), the EN1/EN2/FMT/CTL/MUX/MUX2/CONF
SET/CLR images (`path_regs`) and PIX_ID for RAW. Readback verification lines `path <NAME>: base reg ... applied`
or `... differ from SET/CLR` tell whether the SET/CLR helper assumption holds; on `differ` lines try
`set_clr_mode=0`. A `TG phase counter lost its MCLK setup` warning means the ISP reset also clears the phase
counter. VF stays off and DMA is not enabled.

**Stage 4, IMGO DMA and one frame.** Allocates a coherent buffer (`dma_alloc_coherent`, bus address programmed
as is), programs IMGO geometry/base, enables DMA, writes INT_EN, sets `TG_VF_CON` single shot then VFDATA_EN,
waits for PASS1_TG1_DON (IRQ, or polling with `irq_mode=0`; timeout `capture_timeout_ms`), stops VF/DMA/INT and
reports `bytes_changed=.../... min max mean first=<hex>`. The buffer is pre-filled with `frame_fill` (0xa5) so an
untouched buffer is detected (`-ENODATA`). Return codes: `-ETIMEDOUT` no frame done interrupt, `-EIO` TG1_ERR /
IMGO_ERR / DMA_ERR in INT_STATUS, `-ENODATA` done but nothing written.

`frame_info` gives `width`, `height`, `stride_bytes`, `size`. RAW10 is the MTK packed layout of the
stock geometry (4 pixels in 5 bytes, `stride_bytes = width * 5 / 4`, little-endian bitstream, see `isp-capture.md` 6.1). YUV422 is
2 bytes per pixel, expected UYVY. Quick looks: `python3 -c` unpacking, or for YUV `ffmpeg -f rawvideo -pix_fmt
uyvy422 -s 632x474 -i frame.bin out.png` (stride equals `2*width`).

## Module parameters (all adjustable live, `0644`)

Open questions in the docs, with the default and what to try:

| Parameter | Default | Question |
|---|---|---|
| `irq_mode` | 1 | `0` polls INT_STATUS instead of using SPI 143. |
| `set_clr_mode` | 1 | SET/CLR pairs (1) or read-modify-write of the base registers (0). |
| `imgo_base_mode` | 1 | bit0 write IMGO_BASE_ADDR before VF start, bit1 rewrite at SOF1, bit2 rewrite at PASS1_DON (IRQ mode). |
| `fbc_mode` | 0 | 1 = vendor FBC init with one buffer; 0 leaves FBC off. |
| `vf_single` | 1 | single-shot TG (VF_CON bit1) or continuous. |
| `csi2_ctrl`, `csi2_ecc_order`, `csi2_vsync`, `csi2_settle`, `lanes` | computed, sensor, 14 | `SENINF1_CSI2_CTRL` composition and settle count. |
| `port` | -1 | CSI0 for the rear, CSI1 for the front is the stock convention (unverified on this board). |
| `tg_pad`, `tg_src`, `tg_fmt`, `sync_a`, `sync_b` | sensor | `setTg1InputCfg`/`setTg1SensorModeCfg` arguments (RAW stock table: pad 0, src 3, fmt 1; YUV: pad 4, src 8, fmt 3). `src != 8` sets TG_SEN_MODE bit 8, which `isp-capture.md` section 4 says is parallel only: try `tg_src=8` for the SP2508 if the TG error flag is set. |
| `grab_x`, `grab_y`, `width`, `height`, `pixid` | sensor | TG grab window, frame size, Bayer start. Compare with the detected size of stage 2. |
| `int_en`, `dma_int_bits`, `imgo_con`, `imgo_con2`, `imgo_pb`, `imgo_stride_px`, `done_mask` | vendor values | ISP interrupt and IMGO constants. |
| `mclk_pinmux`, `io_drive`, `isp_domain_keep`, `test_pattern`, `stream_wait_ms`, `run_while_error`, `capture_timeout_ms`, `frame_fill` | see `modinfo` | housekeeping. |

## Unverified assumptions

1. The ISP register map of `isp-capture.md` was never run without the stock libraries; the SET/CLR helper
   semantics, the pass-1 images (ID 8), the constants at `+0x06c/0x150/0x280/0x380/0x39c` and the IMGO CON values
   are replayed from the vendor code.
2. DMA addresses: the module programs the CPU/DMA bus address of a coherent buffer. M4U translation is assumed
   off and SMI larb2 assumed to pass physical addresses; the stock stack configures the camera ports as virtual.
   If the frame buffer stays untouched with a clean `PASS1_TG1_DON`, suspect this first.
3. `IMGO_BASE_ADDR` latching, FBC-off behaviour, and single-shot (`VF_CON` bit1) operation without the command
   queue are untested.
4. Whether the rear/front sensors use CSI0 (4-lane capable) and CSI1, and 1 lane each (SP0A20's lane count is
   not set in its stock `GetInfo`).
5. SP0A20 grab window (stock `GetInfo` says 1,1 while `Preview` reports 128/17) and size 632x474; SP2508 grab
   2,2 (preview) vs 4,4 (capture) and 1600x1200. The SENINF detected size gives the truth.
6. The eFuse trim of `ana+0x24` is skipped (no eFuse window); the MIPI pin mux is not restored on power down.
7. The ISP reset may clear TG/SENINF state differently from the assumption (the module warns if PH_CNT changes);
   the stock sensors' MCLK recipe is the one verified by the probe script.
8. `SENINF1` bit meanings, the `0x802c` size encoding and the NCSI2 error register at `0x618` are inferred.

## Risks

Registers of an unpowered ISP domain can hang the bus: stage order guarantees domain, gates and camtg mux are on
before any `isp`, `seninf`, `mipi-rx` access, and `reg`/`status` refuse to read while clocks are off. A failed
IRQ line configuration (level low, SPI 143) with an unmasked `INT_EN` could storm: the module clears `INT_EN` at
every stop and tear-down. All waits are bounded. The sensor rails are enabled by the module itself; `stage 0` or
`rmmod` powers everything down. Writing `reg` or the module parameters directly can bypass all of this.

## Measured results (2026-10-10)

Both sensors capture a complete frame to DRAM with the module as committed:

| | SP2508 (rear) | SP0A20 (front) |
|---|---|---|
| Port / lanes | CSI0, 1 lane | CSI1 side of the stock convention (port -1 default), 1 lane |
| `csi2_vsync` | **1 is required**: with 0 the receiver sees frame packets (22 Hz VS1/SOF1) but no pixels, detected size stays 0 and no frame completes | **1 is required** (same symptom) |
| Detected size (`SENINF1` +0x2c) | 1616 x 1216 (hi = pixels, lo = lines) | 1280 x 480 (hi = bytes per line, lo = lines: 640 x 480 UYVY) |
| ISP interrupts | `INT_STATUS 0x1401` = VS1 + PASS1_DON + SOF1 | same |
| Output | RAW10, MediaTek packing (4 pixels in 5 bytes as a little-endian bitstream, pixel i in bits [10i, 10i + 10)), stride 2000 bytes for 1600 pixels | UYVY 8-bit, stride 1264 bytes for 632 pixels |
| Result | `docs/samples/sp2508-*`: colour bars decode into proper bars, a real scene decodes into a recognisable image (noisy: no black level/gain/denoise) | `docs/samples/sp0a20-first-frame.jpg`: a room, orange cast (no white balance), brightened x4.5 |

Confirmed by the hardware: the register recipes of docs/seninf-mipi.md and docs/isp-capture.md work without the stock
libraries, with the command queue unused: reset, TG programming, SET/CLR images for EN1/EN2/DMA/FMT/CTL, IMGO geometry
(XSIZE = bytes per line - 1), FBC off, `IMGO_BASE_ADDR` written before the view finder starts, single shot,
M4U/SMI passing physical addresses. A matrix over `tg_src`, FBC, single/continuous and the base-address modes made no
difference before the vsync fix. The 24 MHz MCLK, the PMIC rails and the GPIO sequences are as in `tools/camera-probe.sh`.
