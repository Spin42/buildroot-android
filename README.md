# mt8127-consys

Out-of-tree WiFi support for the MediaTek MT8127 (Lenovo Tab3 7 Essential, TB3-710F) on mainline Linux.
The WiFi is the on-die CONSYS block, driven by the MediaTek WMT stack. The code starts as the stock 3.10.54 vendor
sources (https://github.com/szazs89/kernel_lenovo_TB3_710F) and is ported step by step; the first commit is the
unmodified import, so `git diff <first commit>` shows every change.

| Directory | Origin (vendor tree) | Content |
|---|---|---|
| `wmt/` | `drivers/misc/mediatek/connectivity/conn_soc/common` | WMT core, STP, platform glue |
| `btif/` | `drivers/misc/mediatek/btif` | BTIF (the UART-like link to the CONSYS MCU) |
| `wlan/` | `drivers/misc/mediatek/connectivity/conn_soc/drv_wlan` | gen2 WiFi driver |
| `fm/` | `drivers/misc/mediatek/fmradio` (rewritten) | FM receiver as a V4L2 radio device, see `docs/fm.md` |
| `fw/` | stock `/system/etc/firmware` | firmware blobs, not redistributable, git-ignored |

Milestones: 1 power CONSYS and read the chip id (done, see `docs/bringup-notes.md`), 2 BTIF/STP, 3 patch download,
4 WMT function on, 5 WiFi MAC up (wlan0), 6 scan and associate.
