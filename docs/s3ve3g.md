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
| `s3ve3g_rescue_defconfig` | first light + ssh, kmod with a subset of the modules, and inventory tools (util-linux, e2fsprogs, f2fs-tools, dosfstools, mmc-utils, i2c-tools, evtest, ethtool, strace, htop, dtc); still RAM-only |

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

## Rescue

Same kernel, DTB, command line and boot.img packing as first light; the initramfs is xz-compressed
(`BR2_TARGET_ROOTFS_CPIO_XZ`) to fit lk2nd's 9.5 MiB `boot`. `/init`
(`board/s3ve3g/rescue-overlay/init`) mounts no storage and writes to no block device.

- Screen and `/dev/kmsg`: banner, then `rescue alive: <date> uptime <s>` every 5 s.
- USB: gadget `1d6b:0104`, product `s3ve3g-rescue`, serial `31cf1659`, ACM (`ttyGS0`: banner,
  `dmesg`, root shell) + NCM (`usb0`, phone `10.0.42.1/24`; `unudhcpd` gives the host `10.0.42.2`).
- `dropbear` on port 22: root with the build host's keys or password `root`; `telnetd` on port 23
  without a password as a fallback. Logs in `/run/rescue/`.
- Watchdog pinger and `panic=10`, as in first light.

The build (`scripts/rescue_post_build.sh`) takes root's `authorized_keys` from `~/.ssh/*.pub` (or
`$S3VE3G_RESCUE_AUTHORIZED_KEYS`; with no key at all it generates `id_ed25519` in the ssh directory)
and gives the image a fixed ed25519 host key, generated once in `<output>/../rescue-ssh/`
(`$S3VE3G_RESCUE_SSH_DIR`), converted to dropbear's format by `scripts/dropbear-hostkey.py`. The
same directory holds a `known_hosts` line for 10.0.42.1:

```
ssh -o UserKnownHostsFile=<output>/../rescue-ssh/known_hosts root@10.0.42.1
```

Kernel modules: all of them do not fit, so `board/s3ve3g/rescue-modules.txt` lists the ones kept
(touchscreens, sensors, charger, LEDs, remoteprocs, f2fs, squashfs, and `msm` for deliberate tests)
with their dependencies, uncompressed so xz packs them together. Nothing loads by itself: run
`modprobe <name>`, or `modules-coldplug` to load what matches the hardware and keep loading on
hotplug (mdev runs `/usr/sbin/modalias-load`). `rescue.modules=auto` on the kernel command line does
the same at boot. `/etc/modprobe.d/s3ve3g-display.conf` blacklists `msm` and the panel and bridge
drivers, so alias loading never takes the display from simpledrm; `modprobe msm` by name still
loads it. Firmware goes to `/lib/firmware` (empty for now); the kernel loads it without a helper.

`lsusb` is busybox's: usbutils needs eudev and its hwdb, several MiB that do not fit.
