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

### Reflash an already running system

The board's `scripts/reflash.sh` follows the tb3_710f reflash cycle using the kit's
serial discovery, ACM return mechanism and safe-flash checks. It starts from running
Express kit Linux with working USB networking; it refuses an absent or ambiguous gadget.
It does not configure the host network. Run a host-only check first:

```sh
buildroot-external/board/samsung-expressatt/scripts/reflash.sh --dry-run /absolute/path/rootfs.ext2
```

After the orchestrator has claimed the device and approved the exact write, run from
this repository (use the owner recorded in the port's `.device-owner`):

```sh
EXPRESSATT_REFLASH_OWNER=porting-guide buildroot-external/board/samsung-expressatt/scripts/reflash.sh --execute /absolute/path/rootfs.ext2
```

`EXPRESSATT_REFLASH_PASSWORD` optionally supplies the running root password; the default
is `root`. It is passed through sshpass's environment, never printed in the command.
The same credential must work after reflashing. The helper reads serial `08a571a0` from
port.yaml, verifies the USB kit identity and the remote device-tree serial over SSH bound
to that USB interface, then uses its ACM console to return to download mode. This reuses
the ACM branch of `tools/return-to-flash.sh` without its unbound SSH fallback.

Only a 256 MiB strict ext2 system image with matching kit identity is accepted. Exactly
one Odin device must appear on the same physical USB port; other Fastboot phones can
remain connected. Every issued device command requires guard `ALLOW`. The write is
`tools/safe-flash.sh --port samsung-expressatt --mode samsung-download --execute flash userdata IMAGE`.
The existing verified userdata backup and allowlist remain required by safe-flash.
BOOT and partition tables are not written. After the no-reboot flash, the helper uses
`heimdall print-pit --resume` to reboot and waits at most 90 seconds for serial-verified SSH.

SSH host keys are stored in the port's `journal/plans/expressatt-reflash-known-hosts`.
Before flashing, an unexpected changed key fails normally. After a successful flash and
rediscovery of the expected system gadget on the same USB port, the old key file is copied
to a unique `.before-reflash.*` backup and a fresh dedicated file accepts the new key once.
This explicitly trusts the verified USB attachment for the expected reinstallation; it
does not authenticate the new key through the previous key. An unexpected key change
within the subsequent boot still fails. The helper never uses StrictHostKeyChecking=no.

On failure, inspect the ACM/reboot logs in the same journal directory. Recover through
Odin using the documented verified userdata backup; the stock bootloader and lk2nd remain
available. Runtime reflash acceptance must be recorded from the actual phone, separately
from the helper's host-only dry run.

`samsung-expressatt_rescue_defconfig` builds a RAM-only `boot.img` with the same kernel,
USB identity ending in `-rescue`, SSH and disk inspection tools. A rescue boot changes
no phone storage. The persistent image stores its root filesystem on userdata, so
ordinary filesystem writes and SSH host keys survive a reboot.

The rescue boot image exceeds the physical BOOT partition and must not be flashed
there. The orchestrator verifies a userdata backup, serial/product identity and a RAM
boot before any installation through the kit's device guard. Stock aboot and the
existing partition table remain the recovery path. Permanent installation and repeat
boot/SSH results are recorded in the port backlog, not inferred from a host build.
