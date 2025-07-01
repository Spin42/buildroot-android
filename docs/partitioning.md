# Notes on partitioning

On Android devices, you can’t always reshape the partition table. Sometimes, the stock bootloader will not even boot if you mess with it. I have no idea if that’s the case on the Fairphone2 but for the sake of safety, we won't mess up with it's internal partition table.

The systems you build will therefore need to be flashed inside an existing Android partition. The biggest partition available on the device is the `userdata`.


### My setup doesn't require multiple partitions

In cas you don't need to split your rootfs from your boot partition, or have a dedicated application partition mounted in read-write and your roots in read only, then you can condigure your 'genimage.cfg' as such:

```bash
image userdata.img {
    hdimage {
        partition-table-type="none"
    }

    partition rootfs {
        image = "rootfs.ext3"
    }
}
```

Since you want to simply "replace" the content on the userdata partition, your output image `userdata.img` in this example, should not have a partition table of its own. You can then use the regular kernel params, such as `root=/dev/mmcblk0p20` to have the kernel automatically mount the rootfs. There is no need for an initramfs in this case. This configuration is what you will find in the basic fairphone 2 buildroot configuration [here](../buildroot-external/board/fairphone2/).

### I need to split my boot partition from my root partition or have a more complex setup

In most cases, you want something more elaborate that the previously described setup. Which means that you will create an image that has more than one partitions in it. But if there are multiple partitions, it means we will need a dedicated partition table. It therefore means that we will flash an image containing a partition table inside an existing partition, creating a set of "subpartitions". The issue it that the kernel can't read in subpartitions by default and it won't boot and mount our rootfs like previously.

In order to boot, the system then relies on a custom initramfs to map the subpartitions to “real” device files thanks to `kpartx` and then proceeds to mount them before passing the ball to Nerves.

In order to know what partitions to look for, 2 command line parameters need to be passed to the kernel. The parameters `rootfs=` and `bootpart=` allow us to tell the initramfs where to look for the boot partition and the root file system partition.

When building a firmware for the fairphone 2 or a fairphone 2 powered board like the Lemon, you need to take this specificity into consideration. For instance, let's say we want to create the following image:

| Image partition table              |
|------------------------------------|
| MBR                                |
| p0*: boot partition (ext2)         |
| p1*: rootfs (ext4)                 |
| p2: Application (ext4)             |

When we flash this entire image on the userdata partition, our boot partition will be at `/dev/mmcblk0p20p1`, our rootfs at `/dev/mmcblk0p20p2` and the application partition at `/dev/mmcblk0p20p3`.

Mounting such "subpartitions" is not something the kernel supports out of the box, and we need a tool like `kpartx` to map them to device files we can use. That's what the initramfs does. It is included in this system thanks to the [citronics-initramfs](https://github.com/Citronics/initramfs) package present in this repository.

### I want to have rootfs redundency and A/B partitioning

Let's say we want to be as close as possible to an A/B partitioning setup. We will want to have the following image:

| Image partition table              |
|------------------------------------|
| MBR                                |
| p0*: boot a partition (ext2)       |
| p0*: boot b partition (ext2)       |
| p1*: rootfs a (ext4)               |
| p1*: rootfs b (ext4)               |
| p2: Application (ext4)             |

Usually, a userspace program would tag the appropriate boot partition, A or B, to be booted next, then the bootloader would try it and if it indeed succeeds in booting, a userspace program will mark the partition as safe to boot. If it didn't boot, a watchdog is triggered during boot, or anything else happens, then the next time the device starts, the bootloader will know and revert back to the previous partition. This is a common behaviour in UBoot for instance, where you can rely on uboot environment variables to do this.

However, we can't use UBoot in our case. We have to use [lk2nd]("./lk2nd.md"). Which doesn't support environment variables and A/B partitioning logic. Moreover, remember that we are in a "subpartition" situation since we flash an entire disk on an existing device partition.

What lk2nd will do if look for an `extlinux.conf` file on the first ext2 partition it finds. Since there is no support from the booloader for more advanced features, the only viable strategy is to use the partition table as a way to "hide" the previous partitions and update it from userspace when there is an update. But there is a caveat, *any update to the kernel or rootfs partition that won't boot cannot be recovered from*. You will have to physically access the device to reflash it.