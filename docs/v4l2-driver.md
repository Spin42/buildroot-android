# V4L2 capture driver (`mtk_camsys`)

`cam/mtk_camsys*.c` is the V4L2 + media-controller driver for the camera path of the TB3-710F. It replaces the debugfs
prototype (`mtk_cam_proto`, `cam-proto.md`, still built from the same directory; do not load both: they use the same
register windows and interrupt). The register programming is the one validated with the prototype
(`seninf-mipi.md`, `isp-capture.md`): CSI-2 `vsync` bit 13 set, RAW10 MIPI-packed, `IMGO_XSIZE` = bytes per line - 1,
FBC off, IMGO base address written before the view finder starts.

Build: `make` in the repository root produces `cam/mtk_camsys.ko`, `sensors/sp2508.ko`, `sensors/sp0a20.ko` (and the
prototype). `cam/Kconfig` documents the dependencies for an in-tree build (`CONFIG_VIDEO_MT8127_CAMSYS`).

## Media graph

```
sp2508 (rear)  --[0]--> pad 0 \
                                mtk-seninf  pad 2 --[immutable]--> mtk-camsys capture (/dev/videoN)
sp0a20 (front) --[1]--> pad 1 /
```

| Entity | Pad | Role | Format |
|---|---|---|---|
| `sp2508 0-003c` | 0 source | rear raw sensor, 1 lane, CSI0 | `SBGGR10_1X10` 1616x1216 |
| `sp0a20 0-0021` | 0 source | front YUV sensor, 1 lane, CSI1 | `UYVY8_1X16` 640x480 |
| `mtk-seninf` | 0 sink | rear sensor (CSI0) | fixed `SBGGR10_1X10` 1616x1216 |
| `mtk-seninf` | 1 sink | front sensor (CSI1) | fixed `UYVY8_1X16` 640x480 |
| `mtk-seninf` | 2 source | towards the ISP | follows the enabled sink |
| `mtk-camsys capture` | 0 sink | video node | `SBGGR10P` 1616x1216 or `UYVY` 640x480 |

Only one sink link of `mtk-seninf` can be enabled at a time (both sensors share the MCLK and the SENINF1 input): to
switch sensors disable the enabled link first, then enable the other. The first sensor that binds gets its link enabled
by default. Link validation checks the media bus code and size across the sensor, the receiver and the video node.

The sensors are bound through the fwnode graph (`ports` of the camsys node), keep their I2C addresses (0x3c, 0x21 on
`i2c0`), and own their regulators, reset and power-down GPIOs. They enable the 24 MHz `xclk` provided by the camsys
node in their power-on sequence; enabling it powers the ISP domain, opens the IMG clock gates, selects the 48 MHz
camtg source, configures the TG phase counter and gates the MCLK pin (`mtk_camsys_clk.c`). Streaming starts the sensor
through `v4l2_subdev_enable_streams()`: the receiver subdev powers the MIPI PHY, starts the enabled sensor, then routes
SENINF1; the video node then resets and configures ISP pass 1 and starts the view finder.

## Video node

`V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING`, vb2-dma-contig, `MMAP` and `DMABUF`, at least one queued buffer to
start. Formats (the size follows from the pixel format; `try_fmt`/`s_fmt` coerce it):

| Pixel format | Size | bytesperline | sizeimage | Layout |
|---|---|---|---|---|
| `V4L2_PIX_FMT_SBGGR10P` (`pBAA`) | 1616x1216 | 2020 | 2 456 320 | MIPI RAW10: 4 pixels in 5 bytes, first pixel blue |
| `V4L2_PIX_FMT_UYVY` | 640x480 | 1280 | 614 400 | YUV 4:2:2 |

The pitch rule of `isp-capture.md` is applied: RAW10 pitch is a multiple of 8 pixels times 5/4, YUV is 2 bytes per
pixel. The TG grab window starts at (0, 0) and covers the whole sensor frame (YUV: two TG samples per pixel); the module
parameters `grab_x` and `grab_y` override the start for experiments. The stock code used a start of (2, 2) for the
rear sensor and (1, 1) for the front sensor with smaller frames (1600x1200, 632x474).

Capture is continuous. At each frame-done interrupt the finished buffer is returned (timestamp from the start-of-frame
interrupt, `CLOCK_MONOTONIC`; sequence number counting every frame) and the next queued buffer is programmed into
`IMGO_BASE_ADDR`. With an empty queue the current buffer is kept and overwritten by the next frame; the frame is
counted as dropped (visible as a sequence gap on the next completed buffer, and in the stop message with `dyndbg`).
A buffer that saw a TG/IMGO/DMA error interrupt is returned with `V4L2_BUF_FLAG_ERROR`.

The sensors' controls (exposure, gain, blanking, test pattern, white balance, ...) are on their subdev nodes.

## Kernel configuration

Needed in addition to the sensor options of `sensors.md`: `CONFIG_VIDEO_DEV`, `CONFIG_MEDIA_CONTROLLER`,
`CONFIG_VIDEO_V4L2_SUBDEV_API`, `CONFIG_V4L2_FWNODE`, `CONFIG_V4L2_ASYNC`, `CONFIG_COMMON_CLK`, `CONFIG_PM` (for the
sensor runtime PM; the camsys driver counts its power references itself) and the vb2 modules
`CONFIG_VIDEOBUF2_CORE`, `CONFIG_VIDEOBUF2_V4L2`, `CONFIG_VIDEOBUF2_MEMOPS`, `CONFIG_VIDEOBUF2_DMA_CONTIG`. The last
group has no prompt: it is selected by `CONFIG_V4L_PLATFORM_DRIVERS=y` together with a driver that selects it (for
example `CONFIG_VIDEO_MEDIATEK_JPEG`, or the in-tree `VIDEO_MT8127_CAMSYS` entry of `cam/Kconfig`).
`CONFIG_CMA` (with a CMA area of at least 16 MB) is strongly recommended: each buffer is a 2.4 MB physically
contiguous allocation and the DMA address is programmed as is.

## Device tree

The prototype node is replaced (same `reg`, `reg-names` and interrupt, the sensors now own the regulators and
GPIOs). Binding: `bindings/mediatek,mt8127-camsys.yaml`, sensors: `bindings/superpix,*.yaml`.

```dts
/ {
	soc {
		camsys: camera@15000000 {
			compatible = "mediatek,mt8127-camsys";
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
			#clock-cells = <0>;

			ports {
				#address-cells = <1>;
				#size-cells = <0>;

				port@0 {
					reg = <0>;
					camsys_csi0_ep: endpoint {
						remote-endpoint = <&sp2508_ep>;
						data-lanes = <1>;
					};
				};

				port@1 {
					reg = <1>;
					camsys_csi1_ep: endpoint {
						remote-endpoint = <&sp0a20_ep>;
						data-lanes = <1>;
					};
				};
			};
		};
	};
};

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
		orientation = <1>;
		rotation = <0>;

		port {
			sp2508_ep: endpoint {
				remote-endpoint = <&camsys_csi0_ep>;
				data-lanes = <1>;
				link-frequencies = /bits/ 64 <300000000>;
			};
		};
	};

	camera@21 {
		compatible = "superpix,sp0a20";
		reg = <0x21>;
		clocks = <&camsys>;
		clock-names = "xclk";
		avdd-supply = <&mt6323_vcama_reg>;
		dovdd-supply = <&mt6323_vcamio_reg>;
		reset-gpios = <&pio 55 GPIO_ACTIVE_LOW>;
		powerdown-gpios = <&pio 54 GPIO_ACTIVE_HIGH>;
		orientation = <0>;
		rotation = <0>;

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

Remove the `mediatek,mt8127-cam-proto` node: both match the same register windows. The `camera@15000000` unit address
matches the first window; the prototype used `camera@15004000`.

## Testing on the tablet

```sh
insmod sp2508.ko; insmod sp0a20.ko; insmod mtk_camsys.ko     # or modprobe after depmod; the vb2 modules must exist
media-ctl -p                                                 # find the media node (/dev/mediaN) and the video node
dmesg | tail -30                                             # sensors probe (chip id), camsys binds both
```

Rear sensor (SP2508), raw:

```sh
media-ctl -l '"sp0a20 0-0021":0 -> "mtk-seninf":1 [0]'
media-ctl -l '"sp2508 0-003c":0 -> "mtk-seninf":0 [1]'
media-ctl -V '"sp2508 0-003c":0 [fmt:SBGGR10_1X10/1616x1216]'
media-ctl -V '"mtk-seninf":0 [fmt:SBGGR10_1X10/1616x1216]'
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1616,height=1216,pixelformat=pBAA
v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-count=30 --stream-to=/tmp/rear.raw
yavta -f SBGGR10P -s 1616x1216 -n 4 -c10 -F/tmp/rear-#.bin /dev/video0     # alternative
# colour bars from the sensor: v4l2-ctl -d /dev/v4l-subdev<N> --set-ctrl test_pattern=1
```

Front sensor (SP0A20), UYVY:

```sh
media-ctl -l '"sp2508 0-003c":0 -> "mtk-seninf":0 [0]'
media-ctl -l '"sp0a20 0-0021":0 -> "mtk-seninf":1 [1]'
media-ctl -V '"sp0a20 0-0021":0 [fmt:UYVY8_1X16/640x480]'
media-ctl -V '"mtk-seninf":1 [fmt:UYVY8_1X16/640x480]'
v4l2-ctl -d /dev/video0 --set-fmt-video=width=640,height=480,pixelformat=UYVY
v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-count=30 --stream-to=/tmp/front.raw
ffmpeg -f rawvideo -pix_fmt uyvy422 -s 640x480 -i /tmp/front.raw -frames:v 1 front.png
```

Checks: `v4l2-ctl -d /dev/video0 --list-formats-ext`, `v4l2-compliance -d /dev/video0 -s` (needs the link set up first),
the reported fps from `--stream-mmap` (about 21 fps rear, about 30 fps front), and `cat /proc/interrupts | grep camera`.
Unpack a RAW10 frame on the host (4 MSB bytes then one byte holding the four 2-bit LSB pairs, pixel 0 in the low bits):

```python
import numpy as np
b = np.fromfile("/tmp/rear.raw", np.uint8)[:2020 * 1216].reshape(1216, 505, 5)
lsb = b[:, :, 4:5] >> (2 * np.arange(4)) & 3
raw = (b[:, :, :4].astype(np.uint16) << 2 | lsb).reshape(1216, 2020 // 5 * 4)   # 1616 pixels per line
```

## Known limitations and untested points

- Not run on hardware: everything below the compile level is untested. The register programming reuses the prototype;
  what is new is the continuous mode, the buffer rotation and the full-frame grab.
- Continuous view finder (`TG_VF_CON` single-shot bit clear) was never run; the prototype captured single frames.
- The base address for the next frame is written from the frame-done interrupt during the blanking interval; whether
  the ISP latches `IMGO_BASE_ADDR` at the start of frame or immediately is not established. If frames land in the wrong
  buffer, program it from the start-of-frame interrupt instead.
- The TG grab window starts at (0, 0) and covers the full frame (1616x1216, 640x480). The proven values were an offset
  start with a smaller frame; use `grab_x`/`grab_y` and compare the frame content if the TG reports errors.
- The ISP reset clears nothing the driver relies on as far as known, but the driver rewrites the TG phase counter after
  the reset if it finds it changed.
- The ISP power domain is switched through the SPM registers (no genpd driver exists), the pad mux and drive of the
  MIPI and MCLK pins are written directly into the GPIO block. The camsys node must therefore stay the only user of
  these bits.
- DMA addresses are programmed as physical addresses (M4U and SMI pass them in the prototype). Buffers are
  contiguous; without CMA large allocations may fail.
- One sensor at a time. Only one lane per sensor is verified; `data-lanes` up to 4 are accepted for the receiver but
  untested.
- No system suspend support, no controls on the video node, no frame interval enumeration, no cropping or scaling
  (IMGO only, no ISP processing: no black level, white balance or denoise).
- Hot unbind with a stream running is not handled specially.
