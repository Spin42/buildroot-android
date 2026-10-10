# mt8127-camera

Camera support for the MediaTek MT8127 (Lenovo Tab3 7 Essential, TB3-710F) on mainline Linux.

Goal: a standard V4L2 + media-controller driver set so that libcamera (the `simple` pipeline handler with the
software ISP) handles stills and plain V4L2 applications handle video.

Layout:

- `docs/` register-level documents recovered from the stock kernel and from the stock userspace camera library
- later: kernel drivers (sensor interface and capture, sensor drivers) and the device-tree changes

Starting points (local): the stock kernel source in `~/Projects/lenovo_tab3_7/vendor-kernel/szazs89-tb3`, the stock
`libcamdrv.so` in `~/Projects/lenovo_tab3_7/dump/bootloader_analysis/system_extract/lib/`, the hardware notes in
`~/Projects/lenovo_tab3_7/dump/`.
