# Camera sensor drivers (SP2508, SP0A20)

Out-of-tree V4L2 sensor subdev drivers for the two sensors fitted on the Lenovo Tab3 7 Essential (MT8127), see
`hardware.md`: `sensors/sp2508.c` (rear, raw) and `sensors/sp0a20.c` (front, YUV). Both are device-tree only, use
`<media/v4l2-cci.h>` (8-bit register addresses, page register 0xfd written explicitly), runtime PM, and expose a
subdev node plus a media-controller entity. Register values, limits and power sequences are taken from the stock
3.10 drivers (`imgsensor/src/mt8127/sp2508_mipi_raw`, `sp0a20_mipi_yuv`, `kd_camera_hw.c`).

Kernel options needed by the modules: `CONFIG_I2C`, `CONFIG_MEDIA_SUPPORT`, `CONFIG_MEDIA_CAMERA_SUPPORT`,
`CONFIG_MEDIA_PLATFORM_SUPPORT`, `CONFIG_VIDEO_DEV`, `CONFIG_MEDIA_CONTROLLER`, `CONFIG_VIDEO_V4L2_SUBDEV_API`,
`CONFIG_V4L2_FWNODE`, `CONFIG_V4L2_CCI_I2C` (no prompt: select it by enabling any CCI driver, e.g.
`CONFIG_VIDEO_GC0308=m`), `CONFIG_PM`, `CONFIG_GPIOLIB`, `CONFIG_REGULATOR`, `CONFIG_OF`, `CONFIG_COMMON_CLK`.
`CONFIG_VIDEO_CAMERA_SENSOR` is the menu that groups sensor drivers.

## Device tree

Bindings: `bindings/superpix,sp2508.yaml`, `bindings/superpix,sp0a20.yaml` (the `superpix` vendor prefix is not in
`vendor-prefixes.yaml` of the kernel tree yet).

| Property | SP2508 | SP0A20 | Notes |
|---|---|---|---|
| `compatible` | `superpix,sp2508` | `superpix,sp0a20` | |
| `reg` | `0x3c` | `0x21` | I2C0 |
| `clocks`, `clock-names = "xclk"` | required | required | 24 MHz MCLK from the camsys node (`docs/v4l2-driver.md`) |
| `clock-frequency` | optional, 24000000 | optional, 24000000 | the clock rate is used when absent |
| `avdd-supply` | `vcama` (2.8 V) | `vcama` | enabled first |
| `dovdd-supply` | `vcamio` (1.8 V) | `vcamio` | enabled 5 ms after `avdd` |
| `reset-gpios` | required, active low | optional, active low | logical 1 = reset asserted |
| `powerdown-gpios` | required, active high | required, active high | logical 1 = powered down |
| `orientation`, `rotation` | optional | optional | `v4l2_ctrl_new_fwnode_properties` |
| `port/endpoint` | `data-lanes = <1>`, `link-frequencies = 300000000` | `data-lanes = <1>`, `link-frequencies = 104000000` | one lane only |

The regulators and `reset-gpios`/`powerdown-gpios` polarity flags are what make the GPIO levels right: the drivers
only use logical values. The MCLK is gated by SoC registers (stock `ISP_MCLK1_EN`) owned by the camsys driver, which
exports it as a clock; the sensor drivers enable it as `xclk` in their power-on sequence.

### Rear sensor (SP2508) on the MT8127 board

```dts
&i2c0 {
	camera@3c {
		compatible = "superpix,sp2508";
		reg = <0x3c>;
		clocks = <&camsys>;
		clock-names = "xclk";
		avdd-supply = <&mt6323_vcama_reg>;
		dovdd-supply = <&mt6323_vcamio_reg>;
		reset-gpios = <&pio 53 GPIO_ACTIVE_LOW>;
		powerdown-gpios = <&pio 56 GPIO_ACTIVE_HIGH>;
		orientation = <1>;	/* back */
		rotation = <0>;		/* to be checked on a picture */

		port {
			sp2508_ep: endpoint {
				remote-endpoint = <&camsys_csi0_ep>;
				data-lanes = <1>;
				link-frequencies = /bits/ 64 <300000000>;
			};
		};
	};
};
```

### Front sensor (SP0A20)

```dts
&i2c0 {
	camera@21 {
		compatible = "superpix,sp0a20";
		reg = <0x21>;
		clocks = <&camsys>;
		clock-names = "xclk";
		avdd-supply = <&mt6323_vcama_reg>;
		dovdd-supply = <&mt6323_vcamio_reg>;
		reset-gpios = <&pio 55 GPIO_ACTIVE_LOW>;
		powerdown-gpios = <&pio 54 GPIO_ACTIVE_HIGH>;
		orientation = <0>;	/* front */
		rotation = <0>;		/* to be checked on a picture */

		port {
			sp0a20_ep: endpoint {
				remote-endpoint = <&camsys_csi1_ep>;
				data-lanes = <1>;
				link-frequencies = /bits/ 64 <104000000>;
			};
		};
	};
};
```

`camsys_csi0_ep`/`camsys_csi1_ep` are the endpoints of the `ports` node of the camsys device (rear = CSI0 = port 0,
front = CSI1 = port 1, see `v4l2-driver.md`). Both sensors share the MCLK and the I2C bus; the driver of the inactive one is not powered.

## Power sequences (as implemented)

SP2508: PDN 0, 1 ms, RST asserted, 5 ms, `avdd`, 5 ms, `dovdd`, 5 ms, MCLK, 2 ms, PDN 1 for 100 ms then 0, 20 ms,
RST released, 5 ms. Power off: MCLK off, 5 ms, RST asserted, 2 ms, PDN 0 (stock leaves it low), 2 ms, `dovdd`,
2 ms, `avdd`.

SP0A20: PDN 0, 1 ms, `avdd`, 5 ms, `dovdd`, 5 ms, MCLK, 5 ms, optional RST released, PDN 1 for 100 ms then 0, 5 ms.
Power off: PDN 1, optional RST asserted, MCLK off, 5 ms, `dovdd`, 2 ms, `avdd`. The stock SP0A20 sequence does not
touch RST.

Supplies are enabled one by one (not with the bulk API) because the order matters.

## SP2508 (raw)

- Bus format `MEDIA_BUS_FMT_SBGGR10_1X10` (stock `SENSOR_OUTPUT_FORMAT_RAW_B`: first pixel B), one lane.
- Size on the bus: **1616x1216**. The stock init programs the MIPI output window registers 0x8e..0x91 (page 1) with
  0x0650 x 0x04c0 = 1616 x 1216 and the stock reports a 1600x1200 image taken at grab start (2, 2). The subdev
  therefore advertises 1616x1216 (crop = bounds = native); the 1600x1200 image area is a sub-window that the capture
  side crops.
- Timing: line length 1160 sensor clocks, frame length 1241 lines, "pll clk 60M" and 21 fps in the stock comments. One
  sensor clock carries two pixels (hblank register 0x0140 + 0x20 offset + 808 clocks of active line = 1160), so
  the driver uses pixel rate 60 Mpixel/s, HTS 2320 pixels, VTS 1241 lines (20.8 fps).
- Controls (page 1 registers, every write followed by the update latch 0x01 = 1):

| Control | Range | Register |
|---|---|---|
| `EXPOSURE` | 7..VTS-5 lines (stock clamps at 7 and VTS-5), default 777 | 0x03/0x04 |
| `ANALOGUE_GAIN` | 0x10..0x90, step 1, code/16 = multiplier: 1.0x..9.0x | 0x24 |
| `HBLANK` | 704..2110 pixels, step 2 | 0x09/0x0a = hblank/2 - 32 |
| `VBLANK` | 25..211 lines | 0x06 = vblank + 44 |
| `PIXEL_RATE` | 60 000 000 (read-only) | - |
| `LINK_FREQ` | 300 MHz (read-only) | - |
| `TEST_PATTERN` | Disabled, Color Bars | 0x0d with 0xfb and the 0xac latch |

- Gain unit: the stock converts the MTK gain (64 = 1x) to `reg = 0x10 * gain/64`, i.e. a linear 1/16 code, and
  clamps it to 0x10..0x90. A libcamera `CameraSensorHelper` for the SP2508 needs `gain = code / 16`.
- The register init is written when streaming starts (after power-on); the digital gain registers of page 2 are set
  to 0xb0 by the stock init and left there. The stock sets 0x3f = 0 (HV mirror) for all modes and the
  raw_B order is valid with it; the driver writes it after the init table and exposes no flip controls (changing
  it would change the Bayer order).

## SP0A20 (YUV)

- Bus format `MEDIA_BUS_FMT_UYVY8_1X16`: the stock output is `SENSOR_OUTPUT_FORMAT_UYVY` and MIPI CSI-2 carries
  YUV422 8-bit (data type 0x1e) as U Y V Y. In the media bus API the CSI-2 side of YUV sensors is described by the
  `_1X16` code (e.g. ov5640 on CSI-2), `_2X8` being the 8-bit parallel-bus variant.
- Size: 640x480 on the bus. The stock sizes (`IMAGE_SENSOR_VGA_WIDTH` 640-8, `HEIGHT` 480-6) give the 632x474 image
  the stock uses; there is a single stock mode (preview, video and capture are the same). Line/frame length 784x510,
  pixel clock 13 MHz (52/4) from the stock header, about 30 fps with the auto exposure of the sensor.
- The stock init table (363 writes over pages 0..2) is applied unchanged when streaming starts.
- Controls the stock supports and the driver exposes: `AUTO_N_PRESET_WHITE_BALANCE` (auto, incandescent, fluorescent,
  daylight, cloudy; the stock "tungsten" preset has no V4L2 counterpart and "off" does nothing), `COLORFX` (none,
  b&w, sepia, negative, sky blue = stock sepia blue, grass green = stock sepia green), `AUTO_EXPOSURE_BIAS` (-1.3 to
  +1.3 in the stock steps, page 1 register 0xdb), read-only `PIXEL_RATE`, `LINK_FREQ`, `HBLANK` (144) and `VBLANK`
  (30), plus orientation and rotation. Brightness, contrast, saturation and flips are not implemented in the stock
  driver. The sensor runs its own auto exposure and white balance; there are no exposure or gain controls.
- Changing the colour effect while streaming sleeps 200 ms as the stock does.

## Not verified

- Nothing was run on the hardware: no probe, no frames.
- SP2508: RAW10 (not RAW8) and the 1616x1216 output window are inferred; the lane rate (300 MHz link frequency) is
  derived from the pixel rate and may not be what the PLL produces; HBLANK/VBLANK limits beyond the stock values
  (maximum 0x03ff for hblank, 0xff for vblank, vblank offset 44) and the 0x06-only vblank write are conservative
  guesses; the MIPI data type is not configurable. The stock has no stream-on/off register: streaming starts when the
  init table is written and stops when the power is cut (`disable_streams` powers the sensor down), so a stream stop
  does not park the data lane in LP-11 by itself.
- The test pattern writes follow the stock `SetTestPatternMode` (0xfb, 0xac, 0x0d); its other registers match the init.
- SP0A20: VGA output on the bus (the stock only states 640x480 minus the ISP margins), the MIPI lane count (the stock
  never sets one; it defaults to a single lane), the link frequency (derived from 13 MHz x 16 bit) and the mapping of
  the 0xdb steps to 1/3 EV are assumptions. The vendor reset line handling is not defined.
- Orientation/rotation of the modules are not known; the stock mirrors the SP2508 in software for its own ISP.
- `v4l2_subdev_s_stream_helper` is used with `enable_streams`/`disable_streams`; runtime PM powers the sensors only
  while streaming (and during probe).
