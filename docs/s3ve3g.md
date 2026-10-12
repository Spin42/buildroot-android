# Samsung Galaxy S III Neo (GT-I9301I, s3ve3g)

Qualcomm MSM8228 (msm8226 family, 4x Cortex-A7, 1.5 GiB RAM, Adreno 305), PM8226 PMIC, 16 GB eMMC,
720x1280 S6E8AA0A AMOLED on MIPI DSI (video mode), SM5502 MUIC on the USB connector.
The kernel is the msm8226-mainline fork (`v6.16.12-msm8226-u2`, the one postmarketOS uses) with the
board DTS extended in `board/s3ve3g/linux-patches`.

## Boot chain

```
PBL -> sbl1 -> tz/rpm -> Samsung aboot (download mode, never written)
    -> BOOT[0 .. 512 KiB) = lk2nd (QCDT image with this phone's msm-id, SEANDROIDENFORCE)
         -> /extlinux/extlinux.conf on userdata  (the persistent system, default)
         -> "fastboot boot boot.img"  (RAM only, from the lk2nd menu)
         -> BOOT[512 KiB ..) boot.img  ("fastboot flash boot" in lk2nd, 9.5 MiB max)
```

Samsung aboot runs first on every boot and decides from the keys and the restart reason whether
to load lk2nd at all. lk2nd then boots the first ext2 filesystem with `/extlinux/extlinux.conf`
(userdata), unless Volume Down is held.

`boot.img` is an Android v0 image: `zImage` with `qcom-msm8226-samsung-s3ve3g.dtb` appended, the
initramfs, page size 2048, base 0, kernel 0x8000, ramdisk 0x2000000, tags 0x1e00000. lk2nd takes
the single appended DTB (it has no `qcom,msm-id`), patches it (memory, cmdline, ramoops) and clears
its magic so the kernel uses the patched copy. The image needs no QCDT and no Samsung footer: only
the stock aboot checks those, and it loads lk2nd, not this image.

### Restart reasons

Linux leaves the reason in IMEM at `0xfe80565c` (`syscon-reboot-mode`, `reboot-mode <mode>`).
Samsung aboot reads the word and clears it on every boot:

| `reboot-mode` | Magic | Samsung aboot does |
|---|---|---|
| `download`, `bootloader` | `0x12345671`, `0x77665500` | download mode (Odin, USB `04e8:685d`) |
| `recovery` | `0x77665502` | boots the `recovery` partition, and keeps doing so (sticky, stored in `param`) |
| `normal` | `0x12345678` | normal boot (lk2nd), and clears a sticky recovery boot |
| none / anything else | | normal boot (lk2nd) |

Because aboot clears the word before lk2nd runs, lk2nd never sees `bootloader`: lk2nd fastboot
is reachable only with Volume Down at the Samsung logo. Avoid `reboot-mode recovery`: the
recovery partition holds TWRP, and aboot returns to it until a `normal` reboot.

**Why `reboot=warm`.** The kernel's default restart is a PMIC hard reset, which clears IMEM, so
aboot never saw the magic. `reboot=warm` (on the extlinux and boot.img command lines) makes the
restart a warm reset that keeps IMEM. `/sys/kernel/reboot/mode` shows `warm` on a running system.

### Keys

| Keys | Result |
|---|---|
| Volume Down held from power-on (Volume Down + Power) | Samsung download mode warning; Volume Up enters Odin (`04e8:685d`), Volume Down cancels and reboots |
| Power, release at the Samsung logo, then hold Volume Down | lk2nd menu and fastboot (`18d1:d00d`) |
| Power alone | aboot -> lk2nd -> extlinux on userdata |
| Volume Up from power-on | aboot boots the `recovery` partition |

Never hold both volume keys while lk2nd starts: lk2nd's source then reboots into Qualcomm
emergency download (`05c6:9008`; untested on this phone). Nothing is written in that state;
pulling the battery clears it. A forced restart is Power held about 10 s, or a battery pull.

## Configurations

| defconfig | What it is |
|---|---|
| `s3ve3g_firstlight_defconfig` | kernel + a busybox initramfs: banner and clock on the framebuffer console, USB serial + network report to the host, watchdog; mounts no storage and writes to no block device |
| `s3ve3g_rescue_defconfig` | first light + ssh, kmod with a subset of the modules, and inventory tools (util-linux, e2fsprogs, f2fs-tools, dosfstools, mmc-utils, i2c-tools, evtest, ethtool, strace, htop, dtc); still RAM-only |
| `s3ve3g_defconfig` | the persistent system: one ext2 image for `userdata` holding `/extlinux/extlinux.conf`, the kernel, the DTB and the root filesystem; BusyBox init, the rescue tools, every kernel module |

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
| `EXT2_FS` off, so `EXT4_USE_FOR_EXT2=y` | the persistent root is ext2 (lk2nd reads nothing else); the built-in ext4 driver mounts it as `ext2` and refuses ext4-only features on it |
| `SYSCON_REBOOT_MODE=y` | `reboot-mode <mode>` writes the restart reason to IMEM `0xfe80565c` for Samsung aboot (see Boot chain); built in so it works without loading modules |
| `CHARGER_QCOM_SMBB=y` | the PM8226 charger (patched, see Charging and fuel gauge); built in so the phone charges from the first second, with or without userspace |
| `QCOM_SPMI_VADC=m`, `BATTERY_QCOM_BMS=m` | the battery ADC and the BMS1 fuel gauge (`linux-patches` 0017-0020); loaded by `S12battery` |

Modules stay on, as in pmaports. First light loads none, so everything it uses is built in, and
`scripts/firstlight_post_build.sh` drops `/lib/modules` from its initramfs. Turning `MODULES` off
instead would not shrink the kernel: Kconfig then turns every `=m` into `=y`, including `DRM_MSM`,
which would take the display over from simpledrm without a panel driver.

Kernel command line (`scripts/create_boot_image.sh`): `console=tty0 loglevel=8 clk_ignore_unused
pd_ignore_unused panic=10 reboot=warm qcom_smbb.trace=0 lk2nd.pass-ramoops`. The simple-framebuffer node lists no clocks, so the two
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
| `pronto` + `iris` (the dtsi default `qcom,wcn3660a`), L3 `vddmx`, L6 `vddpx`, L10 XO, L16 PA, L24 RFA/dig, wlan pins GPIO 40-44 | regulator constraints and pins from the stock kernel; dual-band IRIS (802.11a/b/g/n, no 11ac) |
| L8 (Pronto PLL), L10, L16, L24 always on, with the firmware's loads | as the vendor kernel and the other msm8226 Samsungs; switched on demand, the radio start stalled (8 s calibration upload) and the first scans locked the phone up |
| `smbb`: 4.35 V, 1 A fast charge, 500 mA USB input, DC input off, `qcom,battery-presence-source = "id"` | downstream PMIC description (`qcom,bpd-detection = "bpd_id"`: the battery ID pin, not the thermistor) |
| `pm8226_bms` (BMS1 at 0x4000) + `simple-battery` (2100 mAh, 3.4-4.35 V, OCV tables), 10 mOhm sense resistor | vendor BMS profile `samsung_2100mAH_data` and PMIC description |

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

## Persistent system

`s3ve3g_defconfig` builds one ext2 filesystem image (`images/rootfs.ext2`, 512 MiB, and its sparse
form `images/userdata.simg`) that goes to `userdata` (`mmcblk0p26`, 11.70 GiB). lk2nd scans every
partition of 16 MiB or more for an ext2 filesystem with `/extlinux/extlinux.conf` and boots its
default label before falling back to its `boot` partition:

```
default s3ve3g
label s3ve3g
	linux /boot/zImage
	fdt /boot/qcom-msm8226-samsung-s3ve3g.dtb
	append console=tty0 loglevel=8 clk_ignore_unused pd_ignore_unused panic=10 reboot=warm qcom_smbb.trace=0 lk2nd.pass-ramoops root=PARTLABEL=userdata rootfstype=ext2 rootwait ro
```

- **Real ext2.** lk2nd's LK ext2 driver refuses any read-only-compatible feature other than
  `sparse_super` and `large_file`, and maps file blocks through indirect blocks only. The image is
  ext2 rev 1 made with `-O ^64bit,^has_journal,^extent,^flex_bg,^huge_file,^dir_nlink,^extra_isize,^metadata_csum,^uninit_bg`;
  `scripts/create_userdata_image.sh` fails the build if `dumpe2fs` shows any feature outside
  `ext_attr resize_inode dir_index filetype sparse_super large_file`, if `e2fsck -fn` complains, or if
  the extlinux files are missing.
- **The kernel and DTB are separate files** (`BR2_LINUX_KERNEL_INSTALL_TARGET`): lk2nd's extlinux
  boot requires `fdt`/`fdtdir`, loads the DTB itself and patches memory, command line and ramoops
  into it. The appended-DTB `zImage-dtb` is only for `boot.img`.
- **Root by GPT name.** The GPT names are lower case (`PARTNAME=userdata` in sysfs) and the kernel
  compares `PARTLABEL=` case-sensitively. If it ever fails, `root=/dev/mmcblk0p26` is the same
  partition (numbering is identical to Android, see the port dossier).
- **Read-only, checked, then read-write.** ext2 has no journal. The kernel mounts root `ro`;
  `/usr/sbin/rootfs-check` (from `/etc/inittab`) runs `e2fsck -p`, reboots if it repaired the
  mounted root, leaves root read-only if errors remain, otherwise remounts it read-write.
  `/etc/e2fsck.conf` sets `broken_system_clock`, since the clock may be unset at boot.
  `/tmp` and `/run` are tmpfs (`/etc/fstab`); `reboot` and `poweroff` go through BusyBox init,
  which stops the services and remounts root read-only.

Services (`/etc/init.d`, in order): watchdog pinger (`S05watchdog`, `-T 20 -t 5`), the firmware
mount (`S08firmware`, see WiFi below), `mdev` as the
hotplug helper (`S10mdev`; module autoload off unless `s3ve3g.modules=auto` is on the command line
or `/etc/s3ve3g/modules-autoload` exists, then `modules-coldplug` loads what matches the hardware,
with `msm` and the panels blacklisted as in rescue), the fuel gauge (`S12battery`, see Charging and
fuel gauge), the WCNSS drivers (`S30wcnss`), the USB gadget (`S45usb-gadget`: `1d6b:0104`,
product `s3ve3g`, serial `31cf1659`, ACM + NCM, `10.0.42.1`, `unudhcpd` gives the host `10.0.42.2`,
`telnetd` bound to `10.0.42.1:23` without a password) and `dropbear` (`S50dropbear`). init respawns `/usr/sbin/ttygs0-console`: banner, `dmesg`, then a root shell on
`ttyGS0`. Logs go to `/run/s3ve3g/` and syslog (`/var/log/messages`).

ssh: the same authorized keys and the same fixed host key as the rescue image
(`scripts/post_build.sh` reads `<output>/../rescue-ssh/`), so the `known_hosts` there fits both:

```
ssh -o UserKnownHostsFile=<output>/../rescue-ssh/known_hosts root@10.0.42.1
```

### WiFi and Bluetooth (WCNSS)

The Pronto core inside the SoC runs Qualcomm firmware and talks to Linux over SMD:
`qcom_wcnss_pil` (remoteproc) loads and boots it, `wcn36xx` is the mac80211 driver (`wlan0`,
2.4 and 5 GHz, no 11ac) and `btqcomsmd` the Bluetooth HCI (`hci0`). All three are modules.

**Firmware comes from the phone, not from the image.** `wcnss.mdt` and its segments `wcnss.b00`
.. `wcnss.b09` sit in `/image` of the `apnhlos` partition (vfat, `mmcblk0p1`). `S08firmware` finds the
partition by `PARTNAME=apnhlos` in `/sys/class/block/*/uevent`, mounts it read-only
(`ro,nodev,nosuid,noexec,shortname=lower`) on `/run/firmware/apnhlos`, and writes
`/run/firmware/apnhlos/image` to `/sys/module/firmware_class/parameters/path`. The kernel searches
that directory first, then `/lib/firmware`, so no file list or symlink is tied to one firmware
version. The path holds one directory only: firmware from a second partition (the modem's, for
example) needs symlinks from `/lib/firmware` instead.

The image ships what is not on `apnhlos`:

| File | From |
|---|---|
| `/lib/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin` | `package/s3ve3g-firmware`: LineageOS `android_device_samsung_msm8226-common` `702c16ad`, `prima/WCNSS_qcom_wlan_nv.bin` (byte-identical to the phone's stock copy, sha256 in the package hash file) |
| `/lib/firmware/regulatory.db`, `regulatory.db.p7s` | `wireless-regdb`; the kernel checks the signature with its built-in keys (`CFG80211_REQUIRE_SIGNED_REGDB`, `CFG80211_USE_KERNEL_REGDB_KEYS`) |

**Drivers load by name.** Module autoload is off by default on this system, so `S30wcnss` runs
`modprobe -a qcom_wcnss_pil wcn36xx btqcomsmd` once `wcnss.mdt` is in the firmware path (with
`s3ve3g.modules=auto` the modalias path would load them too; loading twice is harmless).
`s3ve3g.wcnss=off` on the kernel command line or `/etc/s3ve3g/wcnss-off` skips it.

**Nothing connects by itself.** `wlan0` stays down. `iw`, `wpa_supplicant` (nl80211, WPA3, WPS,
EAP, `wpa_cli`, `wpa_passphrase`) and `udhcpc` are there; `/etc/wpa_supplicant.conf` holds only
the control socket and a commented example:

```
wpa_passphrase "<ssid>" "<passphrase>" >> /etc/wpa_supplicant.conf
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
udhcpc -i wlan0
```

`iw dev wlan0 scan` works without any of that once `ip link set wlan0 up` has run.

**No debug output on the console.** Leave `wcn36xx`'s `debug_mask` at 0. With debug enabled, every
DXE interrupt logs at `KERN_DEBUG`, and with `loglevel=8` on the command line each line is drawn on
the framebuffer console: during a scan that is enough to stall the system. If you need it, lower
the console level first (`dmesg -n 5`) and read the log with `dmesg -w` over ssh.

### Charging and fuel gauge

The charger is the PM8226 SMBB block (`qcom_smbb`, built in), carrying the Fairphone 2 charger
series plus fixes found on this phone (`linux-patches` 0007-0016, 0023, 0025-0029). It is the
same kernel series the Nerves system runs. Power supplies in `/sys/class/power_supply/`:

| Name | What |
|---|---|
| `smbb-usbin` | the USB input (`online`, `charge_control_limit` = input limit) |
| `smbb-bif` | the battery interface: `status` (`Charging`, `Discharging`, `Full`), `charge_type` (`Fast`, `Trickle`), `present`, `health` |
| `qcom-bms` | the fuel gauge: `capacity`, `voltage_now`, `current_now` (positive = charging), `voltage_ocv`, `charge_now`, `charge_counter`, `temp`, `charge_full_design` |

- **The BATFET low-power trap.** `BATFET_CTRL4` low-power mode (bits 7:6 = `0x40`) is meant for
  running on the battery alone, and it survives every reset short of a battery removal. Whatever set
  it last (Android, or a power-off on battery) leaves it in force under the next charger, and the
  phone then never charges. The driver clears it whenever an input is present, as the vendor driver
  does, and sets it again only when no input remains. With a charger in, `dmesg` shows
  `BATFET low-power mode off (batfet_ctrl4 40 -> 00)` once if it was set.
- **The input floor.** When the USB input is there but not qualified (`CHG_GONE` latched after a
  warm reboot), the driver resets the input arbiter, which re-qualifies at 100 mA. The limit then
  goes back to the floor (500 mA here) as soon as the input qualifies: only a running charge cycle
  ramps the limit, and at 100 mA, below the system load, no cycle would ever start.
  `dmesg`: `input re-qualified after arbiter reset, input limit 500000 uA`.
- **Battery presence by the ID pin** (`qcom,battery-presence-source = "id"`), as the vendor
  configures this phone; the thermistor default gives a wrong answer here.
- **The gauge** (`qcom_bms`) reads the open circuit voltage the BMS1 latches at power-on and when
  the current stays low, and its coulomb counter since then. The state of charge comes from the
  stock 2100 mAh profile (`samsung_2100mAH_data`: OCV tables per temperature, design capacity,
  177 mOhm resistance) in the `simple-battery` node. `qcom_spmi_vadc` provides the battery voltage
  and temperature. No FCC learning and no unusable-charge correction: `capacity` is a reasonable
  estimate, not Android's number.
- `qcom_smbb.trace=1` (command line, or `/sys/module/qcom_smbb/parameters/trace` at run time)
  logs each supervisor decision with the status registers, on change and once a minute. It is a
  diagnostic and is off on this system. The state is also in `/sys/kernel/debug/<device>/state`
  when debugfs is mounted.

Charging is limited to a 500 mA input for every cable; Samsung's software temperature policy is
not implemented.

### Updating the system

`board/s3ve3g/scripts/reflash.sh` writes a new image to `userdata` and nothing else. It is a dry
run by default (read-only checks, then the plan); `--execute` runs the plan and reports PASS or
FAIL.

Default, `--mode odin` (Samsung download mode, heimdall, raw `images/rootfs.ext2`):

```
board/s3ve3g/scripts/reflash.sh <serial>                       # dry run
board/s3ve3g/scripts/reflash.sh --execute <serial>             # write
board/s3ve3g/scripts/reflash.sh --execute --image <rootfs.ext2> <serial>
```

Checks: the image is raw ext2 whose features lk2nd reads (no ro_compat beyond `sparse_super` and
`large_file`, no extents, journal or 64bit) and its sha256 is in `SHA256SUMS` next to it. If the
running system answers ssh on 10.0.42.1 (host key from `rescue-ssh/known_hosts`, or
`--known-hosts`), the script confirms the serial (device tree or gadget), its gadget on this
host's USB, `/sys/kernel/reboot/mode` and that the image fits `userdata`. The plan:

1. ssh: `sync; mount -o remount,ro /; echo warm > /sys/kernel/reboot/mode; reboot-mode download`;
2. wait for exactly one `04e8:685d`, `heimdall detect`;
3. `heimdall flash --USERDATA images/rootfs.ext2`; heimdall reboots the phone;
4. aboot -> lk2nd -> extlinux; wait for the gadget `1d6b:0104` with the serial, then ssh.

If the phone already shows download mode, step 1 is skipped (download mode reports no serial:
only that phone may be in it). `heimdall print-pit` (read-only) also leaves download mode and
reboots into Linux. The script never uses `--repartition`, `--pit` or an AP tar. `--boot <img>`
adds BOOT to the same heimdall call with a separate warning: BOOT starts with lk2nd, so `<img>`
must be a full lk2nd image; it is refused if it does not contain lk2nd.

`--mode fastboot` writes through lk2nd's fastboot (sparse `images/userdata.simg` by default) when
the phone is already in lk2nd via Volume Down at the logo: it checks the serial alone in fastboot,
`lk2nd:version`, `lk2nd:compatible` = `samsung,s3ve3g`, `partition-size:userdata`, then runs
`fastboot flash userdata` and `fastboot reboot`.

`userdata` keeps no backup (nothing on it is kept).

### Recovery from a bad image

aboot and lk2nd are not touched by a userdata update, so both ways back stay available:

- **Download mode:** Volume Down + Power until the phone restarts into the download warning, Volume
  Up, then `reflash.sh --execute --image <known good rootfs.ext2> <serial>` (the ssh step is skipped).
- **lk2nd:** Power, hold Volume Down at the Samsung logo, then
  `fastboot -s <serial> boot <rescue>/images/boot.img` boots the RAM rescue system, which mounts
  nothing; from there inspect or `e2fsck` userdata, or reflash with `--mode fastboot`.

The filesystem is 512 MiB in an 11.70 GiB partition. Growing it is a deliberate step
(`resize2fs` from rescue with the root unmounted), not something the image does at boot.
