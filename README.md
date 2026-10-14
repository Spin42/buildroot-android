# Buildroot for Android devices

This repository supports the following devices so far:

- Fairphone 2
- Nexus 7 (grouper E1565)
- Lenovo Tab3 7 Essential (TB3-710F, MediaTek MT8127)
- Samsung Galaxy S III Neo (GT-I9301I, s3ve3g, Qualcomm MSM8228): first light and a rescue initramfs

## Fairphone 2

Make sure lk2nd is flashed on the boot partition.

1. Download latest lk2nd image here: [lk2nd-msm8974.img](https://github.com/msm8916-mainline/lk2nd/releases/download/20.0/lk2nd-msm8974.img)
2. Reboot your fp2 in fastboot mode.
3. Run `fastboot flash boot lk2nd-msm8974.img`
4. Reboot your phone.

### Build the buildroot system

1. Run `cd buildroot && make BR2_EXTERNAL=../buildroot-external/ fairphone2_defconfig`
2. Run `make BR2_EXTERNAL=../buildroot-external/ all`
3. Reboot your fp2 in fastboot mode.
4. Run `fastboot flash userdata output/images/sdcard.img`
5. Reboot your phone
6. With your phone plugged to your computer via USB, you should see a new network interface, it should be assigned the 10.0.42.2 ip automatically (the fp2 will be 10.0.42.1)
7. Type `ssh root@10.0.42.1`, password is `root`

### With U-Boot instead of lk2nd

`fairphone2_uboot_defconfig` builds U-Boot together with the system and produces `output/images/u-boot-fp2.img` for the boot
partition. `fairphone2_uboot_rauc_defconfig` adds A/B updates with RAUC, with both root filesystems inside the userdata
partition. See [docs/fairphone2_uboot.md](./docs/fairphone2_uboot.md).

## Google Nexus 7 (grouper)

Replace the stock bootloader with Uboot. Follow the instructions [here](https://docs.u-boot.org/en/latest/board/asus/grouper.html)

You will need to recover your SBK with [fusee-tools](https://gitlab.com/grate-driver/fusee-tools#dumping-sbk) first. You will also need a stock bootloader available, you can find the full image on the [google developers website](https://developers.google.com/android/images?hl=fr#nakasi).

### Build the buildroot system

1. Run `cd buildroot && make BR2_EXTERNAL=../buildroot-external/ grouper_defconfig`
2. Run `make BR2_EXTERNAL=../buildroot-external/ all`
3. Press Volume down then press the power button to get into Uboot's boot menu.
4. Select the UMS option and the device should appear as an external drive on your host computer.
5. Flash the `buildroot/output/images/sd_card.img` with your favorite tool (Balena etcher works well).
6. Reboot your Nexus 7
7. With your phone plugged to your computer via USB, you should see a new network interface, it should be assigned the 10.0.42.2 ip automatically (the nexus 7 will be 10.0.42.1)
8. Type `ssh root@10.0.42.1`, password is `root`

## Using WIFI (after flashing)

1. Make sure you can ssh to your FP2 by following the previous section
2. Use `nmcli --ask dev wifi connect <YOURSSID>`

## Using the 4G connection (after flashing and when applicable)

1. Make sure you can ssh to your FP2 by following the instructions above
2. Use `nmcli connection add type gsm ifname '*' con-name gsm apn <YOUR APN>`
3. Then type `nmcli connection up gsm`

## Lenovo Tab3 7 Essential (TB3-710F)

The stock bootloader (LK) is used as is: it only needs to be unlocked (`fastboot oem unlock`, then `fastboot getvar unlocked` says `yes`). The kernel is mainline Linux 7.3-rc6 plus the patches in `buildroot-external/patches/linux/7.3-rc6`. More details and the list of what works are in [docs/tb3_710f.md](./docs/tb3_710f.md).

### Build the buildroot system

1. Run `cd buildroot && make BR2_EXTERNAL=../buildroot-external/ tb3_710f_defconfig`
2. Run `make BR2_EXTERNAL=../buildroot-external/ all`
3. Reboot your tablet in fastboot mode.
4. Run `../buildroot-external/board/tb3_710f/scripts/flash.sh` (flashes `output/images/boot.img` to the boot partition, only ever talks to the tablet's serial)
5. The first time only, write the root filesystem to the userdata partition, see [docs/tb3_710f.md](./docs/tb3_710f.md#writing-the-root-filesystem). Until then the tablet boots into a RAM rescue system.
6. With your tablet plugged to your computer via USB, you should see a new network interface, it should be assigned the 10.0.42.2 ip automatically (the tablet will be 10.0.42.1)
7. Type `ssh root@10.0.42.1`, password is `root`

Once a system is running, `board/tb3_710f/scripts/reflash.sh` does the whole cycle (reboot to the bootloader over ssh, flash, wait for ssh).

## Samsung Galaxy S III Neo (GT-I9301I, s3ve3g)

The stock Samsung aboot chainloads lk2nd from the BOOT partition (built with an s3ve3g entry); every image is RAM-booted from lk2nd's fastboot. Two RAM-only images exist so far: `s3ve3g_firstlight_defconfig` and `s3ve3g_rescue_defconfig` (ssh at 10.0.42.1). Details in [docs/s3ve3g.md](./docs/s3ve3g.md).

1. Run `cd buildroot && make BR2_EXTERNAL=../buildroot-external/ s3ve3g_rescue_defconfig` (or `s3ve3g_firstlight_defconfig`)
2. Run `make`
3. Start the phone into lk2nd's fastboot menu and run `fastboot -s <serial> boot output/images/boot.img`
