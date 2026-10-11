# Samsung Galaxy S III Neo (GT-I9301I, s3ve3g)

Qualcomm MSM8228 (msm8226 family, 4x Cortex-A7, 1.5 GiB RAM, Adreno 305), PM8226 PMIC, 16 GB eMMC,
720x1280 S6E8AA0A AMOLED on MIPI DSI (video mode), SM5502 MUIC on the USB connector.
The kernel is the msm8226-mainline fork (`v6.16.12-msm8226-u2`, the one postmarketOS uses) with the
board DTS extended in `board/s3ve3g/linux-patches`.

## Boot chain

```
PBL -> sbl1 -> tz/rpm -> Samsung aboot (download mode, never written)
    -> BOOT[0 .. 512 KiB) = lk2nd (QCDT image with this phone's msm-id, SEANDROIDENFORCE)
         -> "fastboot boot boot.img"  (RAM only)
         -> BOOT[512 KiB ..) boot.img  ("fastboot flash boot" in lk2nd, 9.5 MiB max)
```

`boot.img` is an Android v0 image: `zImage` with `qcom-msm8226-samsung-s3ve3g.dtb` appended, the
initramfs, page size 2048, base 0, kernel 0x8000, ramdisk 0x2000000, tags 0x1e00000. lk2nd takes
the single appended DTB (it has no `qcom,msm-id`), patches it (memory, cmdline, ramoops) and clears
its magic so the kernel uses the patched copy. The image needs no QCDT and no Samsung footer: only
the stock aboot checks those, and it loads lk2nd, not this image.

## Configurations

| defconfig | What it is |
|---|---|
| `s3ve3g_firstlight_defconfig` | kernel + a busybox initramfs: banner and clock on the framebuffer console, USB serial + network report to the host, watchdog; mounts no storage and writes to no block device |

Build out of tree:

```
cd buildroot
make BR2_EXTERNAL=../buildroot-external O=<out> s3ve3g_firstlight_defconfig
make O=<out>
```

The defconfig fetches the kernel tag from GitHub and applies `board/s3ve3g/linux-patches`. To build
from a local kernel checkout instead, put `LINUX_OVERRIDE_SRCDIR = /path/to/linux` in `<out>/local.mk`
(patches are then not applied: the checkout must carry them).

Boot it from RAM with lk2nd in fastboot mode: `fastboot -s <serial> boot <out>/images/boot.img`.

## First light

`/init` (`board/s3ve3g/firstlight-overlay/init`) reports in three places, so success shows without
looking at the phone:

- screen (`/dev/tty0`): the banner, then `alive: <date> uptime <s>` every 5 s;
- kernel log (`/dev/kmsg`): the banner and one `alive` line a minute, kept by lk2nd's ramoops;
- USB (`usr/sbin/usb-gadget.sh`, configfs): gadget `1d6b:0104`, product `s3ve3g-firstlight`,
  serial `31cf1659`, functions ACM (`ttyGS0`, host `/dev/ttyACM*`) and NCM (`usb0`). On ttyGS0:
  banner, `/proc/cmdline`, `dmesg`, then a root shell. On `usb0`: phone `10.0.42.1/24`, busybox
  `udhcpd` leases `10.0.42.2` to the host (no router option), `telnetd -l /bin/sh` without password.

Self-recovery: busybox `watchdog -T 20 -t 5` drives the KPSS watchdog (`qcom,apss-wdt-msm8226`,
`QCOM_WDT=y`), so a kernel hang reboots the phone; `panic=10` reboots after a panic. With lk2nd on
BOOT and nothing bootable after it, both land in lk2nd fastboot (`18d1:d00d`).

## Kernel

`board/s3ve3g/linux.config` is the pmaports config (`config-postmarketos-qcom-msm8226.armv7`,
pmaports `b48d67b`) saved with `savedefconfig` after building it with gcc and these changes:

| Option | Why |
|---|---|
| `DRM_SIMPLEDRM=y` | the bootloader framebuffer must work without loading modules |
| `FRAMEBUFFER_CONSOLE_ROTATION=y`, `LOGO=y` | fbcon rotation for landscape use; the penguin shows the kernel reached fbcon |
| `USB_CONFIGFS=y` (ACM, NCM, ECM, RNDIS, mass storage functions follow) | USB gadget serial and networking from an initramfs without modules; the ChipIdea controller, its PHY and `QCOM_WDT` are already built in |
| `EXTCON_SM5502=y`, `I2C_GPIO=y` | the MUIC on bit-banged I2C tells the USB controller about the cable |
| `INPUT_EVDEV=y`, `INPUT_PM8941_PWRKEY=y` | keys usable from an initramfs |
| `DEBUG_INFO_NONE=y` | no DWARF/BTF: faster builds, no host pahole |

Modules stay on, as in pmaports. First light loads none, so everything it uses is built in, and
`scripts/firstlight_post_build.sh` drops `/lib/modules` from its initramfs. Turning `MODULES` off
instead would not shrink the kernel: Kconfig then turns every `=m` into `=y`, including `DRM_MSM`,
which would take the display over from simpledrm without a panel driver.

Kernel command line (`scripts/create_boot_image.sh`): `console=tty0 loglevel=8 clk_ignore_unused
pd_ignore_unused panic=10 lk2nd.pass-ramoops`. The simple-framebuffer node lists no clocks, so the two
`*_ignore_unused` keep the display clocks and power domain the bootloader left on.
`lk2nd.pass-ramoops` makes lk2nd add a ramoops region; after a reboot into lk2nd,
`fastboot oem ramoops console` then `fastboot get_staged console.txt` returns the previous kernel log.

## Device tree

| Node | Source |
|---|---|
| simple-framebuffer at 0x03200000, 720x1280, stride 2160, `r8g8b8` | lk2nd log on this phone: "MDP continuous splash detected: pipe RGB_0, base: 0x3200000, stride: 2160, ... format: 0x2243f (bpp: 3)"; same address as ms013g and the Tab 4 |
| SMEM at 0x0fa00000 | lk2nd `MSM_SHARED_BASE` for msm8226 (lk2nd reads SMEM on this phone); same as ms013g and matisse |
| eMMC (`sdhc_1`, L17 2.95 V, L6 1.8 V), SD (`sdhc_2`, L18/L21, card detect GPIO 38) | downstream `msm8226-sec-s3ve3geur-r04.dtsi` |
| keys: Vol Up 106, Vol Down 107, Home 108 | downstream DT, same as ms013g |
| SM5502 MUIC on i2c-gpio (SDA 4, SCL 5), 0x25, IRQ GPIO 52; USB extcon | downstream `msm8226-sm5502.dtsi`, wiring of `qcom-msm8226-samsung-matisse-common.dtsi` |
| L15 3.3 V | downstream: the panel `vdd` (ms013g uses 2.8 V) |
