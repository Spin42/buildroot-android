# Bootloader

Since Citronics boards rely on old android devices, it's not always possible to replace the stock bootloader. In most cases, it won't be possible. So we use the stock bootloader as a first level bootloader, that then launches a second level bootloader which in turn will boot the provided linux kernels.

This second level bootloader is flashed on the Android `boot` partition. This is where the stock bootloader checks for a kernel to boot.

## Fairphone 2

For the Fairphone 2, the second level bootloader that is used is called [lk2nd](https://github.com/msm8916-mainline/lk2nd). On a barebone Fairphone 2 with screen attached, you can simply use one of the releases on their github page.

1. Download latest lk2nd image here: [lk2nd-msm8974.img](https://github.com/msm8916-mainline/lk2nd/releases/download/20.0/lk2nd-msm8974.img)
2. Reboot your fp2 in fastboot mode.
3. Run `fastboot flash boot lk2nd-msm8974.img`
4. Reboot your phone.

If you don't have a screen, you'll need to use a [Citronics forked version](https://github.com/Citronics/lk2nd-noscreen) which will work without screen.

## Lemon

The Lemon board is powered by a Fairphone 2, follow the above instructions about using the [Citronics forked version](https://github.com/Citronics/lk2nd-noscreen)