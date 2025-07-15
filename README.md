# Buildroot for Android devices

This repository supports the following devices so far:

- Fairphone 2
- Nexus 7 (grouper E1565)

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
