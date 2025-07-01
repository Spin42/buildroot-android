# Buildroot for Citronics boards

This repository will allow you to build a buildroot image for the Citronics supported devices:
* [Fairphone 2](./docs/fairphone2.md)
* Lemon

These systems don't include a bootloader by default. You can find some important information about it in the [bootloader documentation](./docs/bootloader.md).

If you want to understand how partitioning is used, or you plan on implementing a more complex partition table, then head out to the [partitioning documentation](./docs/partitioning.md)

## Build the buildroot system

1. Run `cd buildroot && make BR2_EXTERNAL=../buildroot-external/ fairphone2_defconfig` # Or another defconfig
2. Run `make BR2_EXTERNAL=../buildroot-external/ all`
3. Reboot your device in fastboot mode.
4. Run `fastboot flash userdata output/images/sdcard.img`
5. Reboot your device
6. With your device plugged to your computer via USB, you should see a new network interface, it should be assigned the 10.0.42.2 ip automatically (the fp2 will be 10.0.42.1)
7. Type `ssh root@10.0.42.1`, password is `root`

## Standard buildroot README

Buildroot is a simple, efficient and easy-to-use tool to generate embedded
Linux systems through cross-compilation.

The documentation can be found in docs/manual. You can generate a text
document with 'make manual-text' and read output/docs/manual/manual.text.
Online documentation can be found at http://buildroot.org/docs.html

To build and use the buildroot stuff, do the following:

1) run 'make menuconfig'
2) select the target architecture and the packages you wish to compile
3) run 'make'
4) wait while it compiles
5) find the kernel, bootloader, root filesystem, etc. in output/images

You do not need to be root to build or run buildroot.  Have fun!

Buildroot comes with a basic configuration for a number of boards. Run
'make list-defconfigs' to view the list of provided configurations.

Please feed suggestions, bug reports, insults, and bribes back to the
buildroot mailing list: buildroot@buildroot.org
You can also find us on #buildroot on OFTC IRC.

If you would like to contribute patches, please read
https://buildroot.org/manual.html#submitting-patches
