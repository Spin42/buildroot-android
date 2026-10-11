# mt8127-camera

Camera support for the MediaTek MT8127 (Lenovo Tab3 7 Essential, TB3-710F) on mainline Linux.

Goal: a standard V4L2 + media-controller driver set so that libcamera (the `simple` pipeline handler with the
software ISP) handles stills and plain V4L2 applications handle video.

Layout:

- `docs/` register-level documents recovered from the stock kernel and from the stock userspace camera library
- `cam/`: `mtk_camsys` (V4L2 + media-controller capture driver, `docs/v4l2-driver.md`) and `mtk_cam_proto` (debugfs
  prototype, `docs/cam-proto.md`)
- `sensors/`: SP2508 and SP0A20 sensor subdev drivers (`docs/sensors.md`)
- `docs/bindings/`: device-tree bindings
- `data/sp2508/`: stock MediaTek tuning of the SP2508 (black level, shading, AWB, CCM, gamma, AE), decoded by
  `tools/mtk_tuning_extract.py` (`docs/tuning.md`)
- `tools/raw2rgb.py`: host processor for SP2508 RAW10 frames using the stock tuning
- `ipa/simple/sp2508.yaml`, `libcamera-patches/`: libcamera v0.5.0 software ISP tuning, SP2508 sensor helper and
  properties

Starting points (local): the stock kernel source in `~/Projects/lenovo_tab3_7/vendor-kernel/szazs89-tb3`, the stock
`libcamdrv.so` in `~/Projects/lenovo_tab3_7/dump/bootloader_analysis/system_extract/lib/`, the hardware notes in
`~/Projects/lenovo_tab3_7/dump/`.
