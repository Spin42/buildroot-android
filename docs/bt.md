# Bluetooth (module `mtk_consys_bt`)

The CONSYS Bluetooth function is exposed to the mainline Bluetooth stack as a regular HCI device (`hci0`), so BlueZ
(`bluetoothd`, `bluetoothctl`, `btmgmt`) drives it directly. There is no user-space helper, no `/dev/stpbt` and no
`hciattach`.

## Architecture

```
BlueZ (bluetoothd / mgmt)
   |  HCI core (bluetooth.ko)
mtk_consys_bt   hci_dev: open/close/setup/send/set_bdaddr
   |  mtk_wcn_stp_send_data / mtk_wcn_stp_receive_data (BT_TASK_INDX)
   |  mtk_wcn_wmt_func_on/off(WMTDRV_TYPE_BT)
mtk_stp_wmt_soc   WMT core + STP (channel mux, seq/ack, PSM)
mtk_btif          BTIF, the UART-type link to the CONSYS MCU
mt8127_consys     power domain, clocks, VCN18/VCN28/VCN33_BT rails
```

The driver replaces the stock `common/linux/pub/stp_chrdev_bt.c` and follows it step by step:

| Step | Stock `/dev/stpbt` | `mtk_consys_bt` |
| --- | --- | --- |
| power | `open()`: `mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT)` | `hdev->open` |
| reset notification | `mtk_wcn_wmt_msgcb_reg(WMTDRV_TYPE_BT, ...)`, `read()`/`write()` return -88/-99 | same registration; reset start blocks TX, reset end calls `hci_reset_dev()` (the HCI core closes and reopens the device) |
| STP mode | `mtk_wcn_stp_set_bluez(0)` (ring buffer mode) | same |
| RX | `mtk_wcn_stp_register_event_cb(BT_TASK_INDX, ...)`, `read()` drains the ring | same callback, a work item drains the ring into an H:4 reassembler and `hci_recv_frame()` |
| TX | `write()` -> `mtk_wcn_stp_send_data(buf, len, BT_TASK_INDX)`, 0 means no STP window | `hdev->send` queues, a work item sends; on 0 it waits for the STP TX resume callback (`mtk_wcn_stp_register_tx_event_cb`), up to 2 s |
| power off | `close()`: unregister callbacks, `mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT)` | `hdev->close` |

**Framing.** The STP BT channel carries H:4 packets: the payload of every STP BT frame starts with the H:4 packet
type byte (`01` command, `02` ACL, `03` SCO, `04` event). The stock user space wrote `01 03 0c 00` for HCI_Reset and
the STP core itself matches these bytes (`stp_core.c`: `rst_buf[] = {0x01, 0x03, 0x0c, 0x00}` on TX,
`{0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00}` on RX). RX frames land in a byte ring (`MTKSTP_BUFFER_SIZE`, 16 KiB)
whose reads can merge or split packets, so the driver reassembles packets from the H:4 headers rather than relying on
frame boundaries. The STP length field is 12 bits: packets above 4095 bytes are refused (the controller ACL MTU is
far below that).

The STP core only queues BT frames while a BT event callback is registered (`mtk_wcn_sys_check_function_status`);
registering it also flushes the ring, so stale data from a previous session is dropped at open.

The WMT core needs no change for BT: every API above is already exported by `mtk_stp_wmt_soc`, and the BT function
handlers (`wmt_func_bt_on/off`: VCN33_BT through `wmt_plat_soc_paldo_ctrl(BT_PALDO)`, then the WMT function control
command) are built in (`CFG_FUNC_BT_SUPPORT`).

**WiFi and BT together.** WMT keeps a state per function: `opfunc_func_on` powers the chip only if no function is on
yet, and `opfunc_func_off` powers it down only when BT, FM, GPS and WiFi are all off. BT can be switched on and off
while `wlan0` runs. Antenna sharing is handled by the firmware with the `coex_wmt_ant_mode` setting of
`WMT_SOC.cfg`.

## Controller setup (stock init sequence)

The stock Android 5.0 image (`system.img`, TB3-710F S000024) runs Bluedroid with the MediaTek vendor library
`/system/lib/libbluetooth_mtk.so` (built from `hardware/mediatek/bluetooth/driver/combo/mtk.c`). After opening
`/dev/stpbt` it runs `GORM_Init()` with the chip-specific script `bt_init_script_consys`, chosen for chip id 0x8127.
Recovered by disassembly (ARM Thumb-2, `objdump -M force-thumb`, `rabin2`) of the stripped library:

| Address in `libbluetooth_mtk.so` | Content |
| --- | --- |
| `.data` 0x5004 `bt_init_script_consys` | table of 7 function pointers, NULL terminated: 0x1dbd, 0x1a51, 0x19ed, 0x197d, 0x1931, 0x1be9, 0x1abd |
| 0x2204 `GORM_Init` | copies the 64-byte NVRAM record into the global at 0x5130, then calls each entry; a return value of 18 skips the entry, otherwise it waits for Command Complete (event 0x0e) with the same opcode and status 0 (`BT_ReadExpectedEvent`), any failure aborts |
| 0x13d8 `BT_SendHciCommand` | writes `01`, opcode (LE), parameter length, parameters to `/dev/stpbt` |

The script, in order (NVRAM field offsets are relative to `ap_nvram_btradio_struct`):

| # | Function (address) | Opcode | Parameters | Source |
| --- | --- | --- | --- | --- |
| 1 | `GORMcmd_HCC_Set_Local_BD_Addr` (0x1dbc) | 0xfc1a | 6 bytes, BD address little-endian | see below |
| 2 | `GORMcmd_HCC_Set_Radio` (0x1a50) | 0xfc79 | `07 80 00 06 05 07` | NVRAM `Radio[6]` (offset 12) |
| 3 | `GORMcmd_HCC_Set_TX_Power_Offset` (0x19ec) | 0xfc93 | `ff ff ff` | NVRAM `TxPWOffset[3]` (offset 27) |
| 4 | `GORMcmd_HCC_Set_Sleep_Timeout` (0x197c) | 0xfc7a | `03 40 1f 40 1f 00 04` | NVRAM `Sleep[7]` (offset 18) |
| 5 | `GORMcmd_HCC_RESET` (0x1930) | 0x0c03 | none | |
| 6 | `GORMcmd_HCC_Set_Controllor_SysLmp_Log` (0x1be8) | 0xfcbe | `05` | only if `persist.bt.contdebuglog` is set and not `0`; skipped |
| 7 | `GORMcmd_HCC_Set_Controllor_Log_Mask` (0x1abc) | 0xfc5f | 42 bytes of trace mask | same condition; skipped |

The NVRAM file is lost, so the values come from the stock default record in `/system/lib/libcustom_nvram.so`, file
offset 0xa6e0 (64 bytes):

```
00 00 46 81 27 01   BDAddr       (chip default, MSB first)
60 00               Voice
23 10 00 00         Codec
07 80 00 06 05 07   Radio
03 40 1f 40 1f 00 04  Sleep
80 00               BtFTR
ff ff ff            TxPWOffset
00 00 00 00 00 00   CoexAdjust
```

`hdev->setup` sends steps 1 to 4. Step 5 is the HCI_Reset that the HCI core sends first in its own init right after
`setup`. Voice/Codec (PCM/SCO audio) and CoexAdjust are not used by the CONSYS script. The setup runs at every power
on (`HCI_QUIRK_NON_PERSISTENT_SETUP`): powering the BT function off loses the vendor settings.

Confidence: high for opcodes, order, parameter layout and the expected Command Complete (straight-line code, no
indirection); medium for the parameter values, which are the factory defaults and not a calibrated per-device NVRAM
record (`Radio` and `TxPWOffset` may have been tuned per device at the factory; `ff ff ff` means "no offset").

## BD address

Stock behaviour (`GORMcmd_HCC_Set_Local_BD_Addr`, 0x1dbc): each chip has a default address in the library (0x8127:
`00:00:46:81:27:01`, at 0x3323). If the NVRAM address is the default, the library reads the controller eFUSE address
with HCI_Read_BD_ADDR (0x1009, function at 0x1c9c); if that is also the default, it generates a random address
(`rand()` seeded from time and `/dev/urandom`, first byte `(x & 0xfc) | 0x02`, locally administered) and writes it
back to NVRAM and `/data/BT_Addr`. The chosen address is then sent with 0xfc1a.

`mtk_consys_bt` picks, in this order:

1. an address set through mgmt (`btmgmt public-addr`, kept in `hdev->public_addr`; `hdev->set_bdaddr` is 0xfc1a,
   the same command as mainline `btmtk_set_bdaddr`);
2. the `bdaddr=xx:xx:xx:xx:xx:xx` module parameter (most significant byte first, as printed by `bluetoothctl`);
3. the controller eFUSE address (HCI_Read_BD_ADDR) unless it is `00:00:00:00:00:00` or the chip default;
4. a random locally administered address chosen at module load (warning in the log). It is stable while the module
   stays loaded but changes at every boot.

A stable per-device address therefore comes from the module parameter. The Buildroot integration derives it from the
eMMC CID, which is unique per device:

```sh
cid=$(cat /sys/block/mmcblk0/device/cid)
h=$(echo -n "tb3-bt-$cid" | sha256sum | cut -c1-12)
b0=$(( (0x${h%??????????} & 0xfc) | 0x02 ))
addr=$(printf '%02x' $b0)$(echo ${h#??} | sed 's/../:&/g')
modprobe mtk_consys_bt bdaddr=$addr
```

The MT8127 kernel exports no other per-device serial: `system_serial` is all zeros on this board.

## Test

Load order (module dependencies pull `mtk_stp_wmt_soc`, `mtk_btif`, `mt8127_consys` and `bluetooth`; the
firmware `WMT_SOC.cfg` and `ROMv2_patch_1_*_hdr.bin` must be in `/lib/firmware`):

```sh
modprobe mtk_consys_bt bdaddr=02:11:22:33:44:55
dmesg | tail
hciconfig -a              # hci0, Bus: UART, DOWN, BD Address 00:00:00:00:00:00 before the first power on
btmgmt info               # hci0 listed, manufacturer 70 (MediaTek)
```

Power on, then check the address and the version reported by the firmware:

```sh
bluetoothctl power on     # or: btmgmt power on / hciconfig hci0 up
hciconfig -a              # UP RUNNING, BD Address 02:11:22:33:44:55, HCI/LMP version from the firmware
bluetoothctl show         # Powered: yes, Address 02:11:22:33:44:55
bluetoothctl scan on      # [NEW] Device xx:xx:... lines for nearby devices (BR/EDR and LE)
bluetoothctl devices
bluetoothctl pair <addr>  # with an agent: bluetoothctl agent on / default-agent first
bluetoothctl power off    # WMT log: BT function off; chip powered down if WiFi is off too
```

Expected kernel log on power on: the WMT `BT ON` timestamps, then `Bluetooth: hci0: BD address 02:11:22:33:44:55`.
With `mtk_stp_wmt_soc stp_dbg_level=...`/`btif_trace=1` the STP frames show `type=< BT>` headers. A failing vendor
command logs `hci0: opcode 0xfcXX failed, status 0xYY` and the power on fails. For a HCI trace use `btmon` (needs
`BR2_PACKAGE_BLUEZ5_UTILS_MONITOR`).

Things to look at on the first run:

- each of 0xfc1a/0xfc79/0xfc93/0xfc7a gets a Command Complete with status 0 (`btmon`);
- `bluetoothctl show` lists the address from the parameter after a power cycle (`power off`, `power on`);
- WiFi keeps working while BT scans (`iw dev wlan0 scan` during `bluetoothctl scan on`);
- BT off/on with WiFi up, and WiFi down/up with BT up.

## Known gaps

- Audio: SCO over HCI is not configured, and the CONSYS BT PCM path to the audio codec (Voice/Codec NVRAM fields,
  used by the stock audio HAL) is not set up. A2DP works over ACL and needs no driver support; HFP/HSP voice does.
- The radio and TX power values are factory defaults, not the per-device calibration from the lost NVRAM.
- Firmware coredump and the FW assert ioctl (`COMBO_IOCTL_FW_ASSERT`) are not exposed.
- The controller firmware is from 2015 (Bluetooth 4.x). If the HCI core init hits unsupported commands, the
  matching `HCI_QUIRK_BROKEN_*` quirk has to be set in `consys_bt_init()`.
- No runtime PM: the STP PSM handles controller sleep (the sleep timeout command enables it); the host side has no
  wake requirements.
