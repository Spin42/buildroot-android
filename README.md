# Buildroot for Android devices

# Fairphone 2

## Prerequisite

Make sure lk2nd is flashed on the boot partition.

1. Download latest lk2nd image here: [lk2nd-msm8974.img](https://github.com/msm8916-mainline/lk2nd/releases/download/20.0/lk2nd-msm8974.img)
2. Reboot your fp2 in fastboot mode.
3. Run `fastboot flash boot lk2nd-msm8974.img`
4. Reboot your phone.

## Build the buildroot system

1. Run `cd buildroot && make BR2_EXTERNAL=../buildroot-external/ fairphone2_defconfig`
2. Run `make BR2_EXTERNAL=../buildroot-external/ all`
3. Reboot your fp2 in fastboot mode.
4. Run `fastboot flash userdata output/images/sdcard.img`
5. Reboot your phone
6. With your phone plugged to your computer via USB, you should see a new network interface, it should be assigned the 10.0.42.2 ip automatically (the fp2 will be 10.0.42.1)
7. Type `ssh root@10.0.42.1`, password is `root`

## Using WIFI (after flashing)

1. Make sure you can ssh to your FP2 by following the previous section
2. Use `nmcli --ask dev wifi connect <YOURSSID>`

## Using the 4G connection (after flashing)

1. Make sure you can ssh to your FP2 by following the instructions above
2. Use `nmcli connection add type gsm ifname '*' con-name gsm apn <YOUR APN>`
3. Then type `nmcli connection up gsm`
