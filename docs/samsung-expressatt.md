# Samsung SGH-I437 Express

The board uses Linux 7.2.9, pinned to commit
`5fce161649b4d779d1b76d9fcd52dc77779774b8`, with the reboot-mode and battery-support patches
in `buildroot-external/board/samsung-expressatt/linux-patches/`.

## Build

From the repository root:

```sh
make -C buildroot BR2_EXTERNAL="$PWD/buildroot-external" O="$PWD/output/expressatt" samsung-expressatt_defconfig
make -C buildroot O="$PWD/output/expressatt" -j4
```

The persistent output is `images/rootfs.ext2`: one 256 MiB ext2 filesystem containing
`/boot/zImage`, the Express DTB and `/extlinux/extlinux.conf`. Buildroot's filesystem
backend directly creates the raw userdata image; a genimage partition-table wrapper
is unnecessary because the existing phone partition table is preserved.

The only enabled filesystem features are `filetype`, `sparse_super` and `large_file`,
which lk2nd's ext2 reader supports. The post-image check rejects additional features,
verifies the fixed image size against the 5,498,732,544-byte userdata partition and
runs a read-only filesystem check. There is no first-boot resize or partitioning script.

lk2nd reads extlinux and loads the kernel and DTB from userdata. Linux mounts
`root=PARTLABEL=userdata rootfstype=ext2 rootwait rw`. The DTB installed in the filesystem
has the empty reserved-memory parent needed for lk2nd to add framebuffer and ramoops
reservations. The bootloader must use the proven Express RAM base 0x80200000.

## Startup and access

BusyBox `/sbin/init` runs the standard inittab and rcS sequence. `S40kit` mounts configfs
and pstore, starts the watchdog feeder, selects the verified BQ24157 USB host charging
mode (500 mA input), then starts the existing kit ACM+NCM USB and SSH services. A missing
charger mode or a failed mode write is logged and does not prevent USB startup.
The gadget serial is `08a571a0`, product `samsung-expressatt-system`; the phone uses
10.0.42.1 and its DHCP server offers 10.0.42.2 to the host. Root's initial password is
`root`. Dropbear host keys are generated in the writable `/etc/dropbear` directory and
survive reboot. Errors are retained in `/tmp/dropbear.log` during that boot.

The kernel watchdog driver is enabled; the persistent command line has
`watchdog.open_timeout=60` and no diagnostic initcall blacklist or fixed 45-second boot
guard. GPIO I2C adapter13, the MAX17048 fuel gauge and the BQ24157 charger driver are built
in, with their board device-tree descriptions. The Express charger node disables charge
termination from the first driver probe, preserving the vendor policy. The PM8xxx XOADC
path is disabled after its runtime readings proved unusable. Charging mode and current
limits must be verified on the device; compiling these drivers does not establish working charging.

## Rescue and installation

`samsung-expressatt_rescue_defconfig` builds a RAM-only `boot.img` with the same kernel,
USB identity ending in `-rescue`, SSH and disk inspection tools. A rescue boot changes
no phone storage. The persistent image stores its root filesystem on userdata, so
ordinary filesystem writes and SSH host keys survive a reboot.

The rescue boot image exceeds the physical BOOT partition and must not be flashed
there. The orchestrator verifies a userdata backup, serial/product identity and a RAM
boot before any installation through the kit's device guard. Stock aboot and the
existing partition table remain the recovery path. Permanent installation and repeat
boot/SSH results are recorded in the port backlog, not inferred from a host build.
