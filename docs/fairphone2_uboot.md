# Fairphone 2 with U-Boot

U-Boot replaces lk2nd as the second stage bootloader of the Fairphone 2 (MSM8974AB, 32-bit). The stock LK (or lk2nd while
testing) loads it from the `boot` partition as an Android boot image, and U-Boot boots Linux from the `userdata` partition.
U-Boot is built by Buildroot together with the system, from upstream U-Boot plus the patches in
`buildroot-external/board/fairphone2/patches/uboot` (the Fairphone 3 series they build on, then the MSM8974 support and the
Fairphone 2 board).

Two configurations use it:

| Configuration | userdata | Updates |
|---|---|---|
| `fairphone2_uboot_defconfig` | the root filesystem, ext3, written directly (`sdcard.img`), as with lk2nd | reflash |
| `fairphone2_uboot_rauc_defconfig` | a disk image with its own partition table: boot script, root filesystems A and B, RAUC state, data | RAUC A/B bundles |

## What U-Boot does on this phone

It reuses what LK leaves running and drives:

- the display: the console goes to the framebuffer LK scans out (MDP5 pipe), command mode panel included, cleared at start;
- the eMMC and the microSD slot, GPT, ext2/3/4 and FAT;
- the keys: Volume Up/Down (PM8941 GPIOs) and Power (PON);
- USB in device mode: fastboot, mass storage (userdata only) and a serial console (CDC ACM);
- reset, power off and reboot to the bootloader's fastboot mode.

Boot: during the first five seconds, Volume Up opens the boot menu and Volume Down starts U-Boot fastboot. Otherwise U-Boot boots
the first system it finds (standard boot on the eMMC partitions, then the image in userdata) and opens the menu if there is none.
The menu offers Boot, USB disk (userdata), USB fastboot, USB serial console, Reboot to bootloader fastboot, Reboot and Power
off. Volume Up/Down move, Power selects, and any key leaves the USB modes.

## Build

```
cd buildroot
make BR2_EXTERNAL=../buildroot-external/ fairphone2_uboot_defconfig    # or fairphone2_uboot_rauc_defconfig
make BR2_EXTERNAL=../buildroot-external/ all
```

Outputs in `output/images`:

- `u-boot-fp2.img`: U-Boot as an Android boot image, for the `boot` partition;
- `sdcard.img` (`fairphone2_uboot`) or `userdata-rauc.img` and `update.raucb` (`fairphone2_uboot_rauc`): for `userdata`.

## Install

From the fastboot mode of the bootloader (stock LK or lk2nd), always with the serial of the phone:

```
fastboot -s <serial> flash userdata output/images/sdcard.img           # or userdata-rauc.img
fastboot -s <serial> boot output/images/u-boot-fp2.img                 # try U-Boot from RAM
fastboot -s <serial> flash boot output/images/u-boot-fp2.img           # then install it
```

U-Boot reports the same fastboot serial as LK (the eMMC serial number).

Like the lk2nd setup, the system brings up USB networking: the phone is `10.0.42.1` and gives the computer an address,
`ssh root@10.0.42.1` (password `root`).

## RAUC (`fairphone2_uboot_rauc_defconfig`)

The userdata image holds a GPT with:

| # | Name | Content |
|---|---|---|
| 1 | `boot` | FAT, `boot.scr`: the RAUC slot selection (`board/fairphone2/rauc/boot.cmd`) |
| 2 | `rootfs-a` | root filesystem of slot A, ext4, with `/boot/zImage`, the DTB and the initramfs |
| 3 | `rootfs-b` | root filesystem of slot B |
| 4 | `rauc-env` | RAUC state: a 16 KiB U-Boot environment blob (`BOOT_ORDER`, `BOOT_A_LEFT`, `BOOT_B_LEFT`) |
| 5 | `data` | persistent data, mounted on `/data` (RAUC status file) |

U-Boot maps userdata as a block device (`blkmap`) and runs `boot.scr`, RAUC's U-Boot bootchooser: it picks the first slot of
`BOOT_ORDER` with attempts left, counts the attempt down in partition 4 and boots the kernel, DTB and initramfs of that slot
with `rauc.slot=A|B`. When no slot has attempts left, they are reset to 3.

Linux does not see partitions inside a partition, so the initramfs (`board/fairphone2/rauc/initramfs-init`, built into
`/boot` of each slot by `post-build.sh`) attaches userdata to `/dev/loop0` with partition scanning and switches to
`/dev/loop0p2` or `/dev/loop0p3`. RAUC then manages these slots with its U-Boot backend: `fw_printenv`/`fw_setenv` reach the
state in `/dev/loop0p4` (`/etc/fw_env.config`). `S99rauc-mark-good` marks the booted slot good, which restores its attempts.

Update:

```
scp output/images/update.raucb root@10.0.42.1:/data/
ssh root@10.0.42.1 rauc install /data/update.raucb
ssh root@10.0.42.1 reboot
```

`rauc install` writes the other slot and puts it first in `BOOT_ORDER`. If it fails to boot three times, U-Boot goes back to
the previous slot.

Bundles are signed with a development key created on the first build in `output/rauc-keys` (or `$RAUC_KEY_DIR`); its
certificate is the keyring of the image, so only bundles from the same key install. Keep the key to update systems already
flashed, and use your own key and certificate (`RAUC_KEY_DIR`) for anything beyond development.

## Debugging U-Boot without a UART

`board/qualcomm/debug-msm8974.config` (U-Boot) keeps the console in RAM at `0x68000000` across a warm reset. With lk2nd in the
boot partition, `board/fairphone2/scripts/uboot-ramlog.sh <serial>` reads it back through `fastboot oem debug readl`.
