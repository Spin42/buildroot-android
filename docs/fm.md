# FM radio (`fm/`, module `mtk_consys_fm`)

The MT8127 CONSYS contains an FM receiver of the MT6627 family (stock kernel config
`CONFIG_MTK_FM_CHIP="MT6627_FM"`; the vendor driver picks the MT6627 ops for WMT chip id 0x8127). It has no
registers of its own on the AP side: the host talks to it through the CONSYS firmware, over the FM channel of
STP (`FM_TASK_INDX` = 1), once WMT has switched the FM function on. `mtk_consys_fm` is a rewrite of the vendor
driver (`drivers/misc/mediatek/fmradio`: `core/fm_link.c`, `mt6627/pub/mt6627_fm_cmd.c`,
`mt6627/pub/mt6627_fm_lib.c`) as a V4L2 radio device, `/dev/radioN`.

| File | Content | Vendor source |
|---|---|---|
| `fm/fm_link.c` | STP transport, event parser, command/flag handshake | `core/fm_link.c` |
| `fm/fm_mt6627.c` | register access, power, patch download, tune, RSSI, channel check, RDS on/off | `mt6627/pub/mt6627_fm_{cmd,lib,rds}.c` |
| `fm/fm_v4l2.c` | V4L2 device, controls, seek, RDS `read()`, WMT reset message, debugfs | `core/fm_main.c` (behaviour) |

Dependencies: `mtk_stp_wmt_soc` (STP/WMT exports), `mt8127_consys` (`mtk_wcn_cmb_stub_aif_ctrl`), `videodev`.
No WMT change was needed: the FM channel, `mtk_wcn_stp_{send,receive}_data`, `mtk_wcn_stp_register_event_cb`,
`mtk_wcn_wmt_func_on/off`, `mtk_wcn_wmt_msgcb_reg` and `mtk_wcn_wmt_hwver_get` are already exported, and
`WMT_init()` puts STP in `STP_BTIF_FULL | WMT_FM_COMM << 4` mode (FM over STP).

## Protocol

Every command is one STP packet on the FM channel:

    [0x01] [opcode] [len lo] [len hi] [payload ...]

Every answer is an event `[0x04] [opcode] [len lo] [len hi] [payload ...]`. The STP core calls the registered
event callback (no argument, any context); the driver queues a work item that drains
`mtk_wcn_stp_receive_data()` and parses events across reads. Each event sets completion flags; a command waits
until all flags of its mask are set (`fm_cmd_tx()`/`fm_event_parser()` in `core/fm_link.c`):

| Opcode | Command | Event payload | Flag |
|---|---|---|---|
| 0x03 FSPI_READ | `[addr]` | `[val lo, val hi]` | bit 3 |
| 0x04 FSPI_WRITE | `[addr, val lo, val hi]` | none | bit 4 |
| 0x07 ENABLE | basic-operation script | none | bit 7 |
| 0x09 TUNE | script | empty, then `[1]` when tuned | bit 9 and bit 28 |
| 0x0d RDS_DATA | (event only) | sin, cos, then 12-byte groups (A, B, C, D, cbc, crc) | bit 13 |
| 0x0e RAMPDOWN | script | none | bit 14 |
| 0x11 (modify) | one modify basic operation | none | bit 17 |
| 0x12 PATCH / 0x13 COEFF | `[seg count, seg index, up to 512 bytes]` | none | bits 18 / 19 |
| 0x17 SOFT_MUTE_TUNE | `[1, freq lo, freq hi]` | `[size, ?, 11 x u16 CQI]` | bit 23 |
| 0x18 HOST_READ | `[addr32]` | `[val32]` | bit 24 |
| 0x19 HOST_WRITE | `[addr32, val32]` | none | bit 25 |
| 0x20 CSPI_WRITE | `[0x04, addr16, val32]` | none | bit 26 |
| 0x21 CSPI_READ | `[0x04, addr16]` | `[val32]` | bit 22 |

Three register spaces: FM registers (8-bit address, 16-bit value), A-die "top" registers through CSPI
(16-bit address, 32-bit value) and CONSYS host registers (32-bit). Basic operations (`0x80` write, `0x81` udelay,
`0x82` read-until, `0x83` modify, `0x85` top write, `0x86` top read-until; each `[op, size, args]`, little
endian) let one command run a whole register sequence in the firmware. All values are little endian.
Frequencies are in 10 kHz units (8750 = 87.5 MHz); the channel register 0x65[9:0] holds
`(f - 6400) * 2 / 10`, i.e. 50 kHz steps from 64 MHz.

Confidence: the packet format, opcodes, flags and sequences are copied from the stock source, which drove this
exact CONSYS (the stock kernel is built with `MT6627_FM` and selects it for chip id 0x8127). Not verified on
hardware yet.

## Power-up sequence

`mtk_fm_power_on()` (vendor `fm_open()` + `fm_powerup()` + `mt6627_PowerUp()`):

1. register the STP event callback for `FM_TASK_INDX` (this flushes the FM receive queue);
2. `mtk_wcn_wmt_func_on(WMTDRV_TYPE_FM)` (powers CONSYS if needed, downloads the WMT patch on first use,
   VCN28 on in co-clock mode, `wmt_func_fm_on()`);
3. host write `0x8013000c = 0x801f` (CSPI parameters), set bit 1 of host `0x80101030` (A-die top clock);
4. ENABLE script `mt6627_pwrup_clock_on()`: band-gap LDO, MTCMOS on (top 0x60 sequence + poll), digital
   oscillator (top 0x50 = 1, 3, 7), DSP auto clock gating, de-emphasis (FM 0x61 bit 12);
5. read FM 0x62: must be 0x6627 or 0x6625;
6. DSP ROM version (`mt6627_get_rom_version()`): FM 0x61 bit 15 on, release ASIP, poll 0x84 == 1, version is
   0x83[15:8];
7. patch and coefficients download (below), then FM 0x92 = 0, 0x90 = 0x40, 0x90 = 0;
8. ENABLE script `mt6627_pwrup_digital_init()` (ADPLL/SDM, interrupt masks 0x21, ASIP start, poll 0x64 == 2);
9. ENABLE script `mt6627_pwrup_fine_tune()` (RF registers 0x01, 0x03, 0x15-0x17, 0x34, 0x35);
10. I2S out of the FM core: FM 0x9b = 0xf9ab; CONSYS 2-wire receiver: host `0x80101054 = 0x3f35`;
11. CONSYS audio pins in FM I2S mode: `mtk_wcn_cmb_stub_aif_ctrl(CMB_STUB_AIF_2, EN)` → WMT pin control →
    CONSYS register `0x80050078[27:16] = 0x111` (stock `AUDDRV_SET_FM_I2S_GPIO` ioctl of the audio HAL);
12. ramp down (`mt6627_RampDown()`), tune, mono/stereo, volume (FM 0x7d), mute (FM 0x9c[1:0]), RDS on
    (FM 0x80 = 6, FM 0x63 bit 4).

Tune (`mt6627_SetFreq()`): top 0x50 = 7, 0x0f |= 0x455, 0x30[3:2] = TDD channel table, top 0x50 = 0xf,
0x65[9:0] = channel, 0x65[15:12] = HL/FA/ATJ table, then TUNE script (0x63[2:0] = 1) and wait for the tune-done
event. Power-down (`mt6627_PowerDown()`): clear pending STC interrupt, CONSYS I2S receiver off, ENABLE script
`mt6627_pwrdown()` (I2S TX off, resets, clocks off, MTCMOS off), external interrupt mask 0, A-die top clock off,
then AIF_0 and `mtk_wcn_wmt_func_off(WMTDRV_TYPE_FM)`.

Seek: the MT6627 ops of the stock driver leave the firmware seek unused (`#if 0`); the stock FM service scans
with the soft-mute tune command. `VIDIOC_S_HW_FREQ_SEEK` does the same: audio off (ramp down, I2S off), one
SOFT_MUTE_TUNE per channel, a channel is valid when RSSI >= -296, PAMD <= -12, ATDC <= 3496, MR >= -67,
PRX >= 64, ATDEV >= ATDC and soft-mute gain >= 16421 (stock `mt6627_fm_cust.cfg` and `mt6627_fm_cust_cfg.h`),
and on the desense channels (69.10, 76.80, 78.00, 92.10-92.30, 96.00, 99.80, 99.90, 104.00, 107.50, 107.60 MHz)
RSSI >= -240; then audio on and tune. A full band sweep takes about 205 commands.

## Firmware

`request_firmware()` names `mediatek/mt6627_fm_v<N>_patch.bin` and `mediatek/mt6627_fm_v<N>_coeff.bin` with
`N = ROM version + 1`. The stock system ships v1..v5, but only v1 has content (patch 1028 bytes, coefficients
1092 bytes; v2 coeff is 2 bytes, the rest are empty), so the TB3-710F CONSYS reports DSP ROM version 0 and
uses v1. If the files for the reported ROM are missing, the driver takes the newest non-empty pair and clears
bytes 4-5 of the coefficients, as the stock driver does (patch disabled). The download sends 512-byte segments
(`len / 512 + 1` segments, the last one `len % 512` bytes).

Install (from the stock `/system/etc/firmware/mt6627/`):

    /lib/firmware/mediatek/mt6627_fm_v1_patch.bin
    /lib/firmware/mediatek/mt6627_fm_v1_coeff.bin

The stock `mt6627_fm_cust.cfg` is not read; its values are compiled in.

## V4L2 interface

- tuner 0, 87.5-108 MHz, `V4L2_TUNER_CAP_LOW | STEREO | RDS | RDS_BLOCK_IO | HWSEEK_BOUNDED | HWSEEK_WRAP`;
  frequencies are rounded to the 50 kHz grid of the chip;
- `signal`: RSSI register 0x6c[9:0] (two's complement, dBm = raw × 6 / 16) mapped linearly from -120 dBm (0) to
  -40 dBm (65535); `rxsubchans`: stereo indicator 0x6c bit 12;
- `audmode`: `MONO` forces mono (FM 0x75 bit 3), `STEREO` is automatic;
- controls: `mute` (FM 0x9c), `volume` 0..15 (stock 16-step table on FM 0x7d, default 15), `deemphasis`
  50 µs (default) / 75 µs (FM 0x61 bit 12);
- `VIDIOC_S_HW_FREQ_SEEK`: spacing 50/100 (default)/200 kHz, bounded range and wrap-around supported,
  `-ENODATA` when nothing is found;
- `read()`: `struct v4l2_rds_data` blocks (block C' for version B groups, `V4L2_RDS_BLOCK_ERROR` from the
  good-block flags, `CORRECTED` from the corrected bit counts), `poll()` supported.

Power: the FM core powers up at the first open of `/dev/radioN`. At the last close it powers down only if
`mute` is set, otherwise it keeps playing; one-shot tools such as `v4l2-ctl` can therefore tune it, and
`--set-ctrl mute=1` turns the radio off (at that close). Removing the module powers it off.

Debugfs (`/sys/kernel/debug/mtk_consys_fm/reg`, needs the radio on): write `r <fm addr>`, `w <fm addr> <val>`,
`tr`/`tw` (top), `hr`/`hw` (host), hex values; `i2s` dumps the I2S output setup (FM 0x9b, 0x9c, 0x7d, host
0x80101054, 0x80050078, 0x80101030), `i2s_on` writes it again, `aif [state]` calls the WMT audio interface
control (default 2, FM I2S); read the file for the result of the last dump or read.

FM 0x9b reads back only its low bits: after writing 0xf9ab it reads 0x000b (bit 0 I2S on, bit 1 I2S
format, bit 3 slave, as in the MT6630 `I2s_Setting()`), which is the expected state. The CONSYS pin mode
0x80050078[27:16] must read 0x111 (FM I2S); after the WMT pin control the driver reads it back and, if WMT did
not set it, writes it through the FM host interface. Packet traces:
`echo 'file fm_*.c +p' > /sys/kernel/debug/dynamic_debug/control`.

## Audio path

The stock audio HAL (`audio.primary.mt8127.so`: `AudioFMResourceManager`, `WCNChipController`) refuses the
analog FM line-in path ("Not Support FM Analog Line In Path Anymore. Please Use Merge_I2S / 2nd_I2S_In"), the
merge interface is off (`CONFIG_MTK_MERGE_INTERFACE_SUPPORT` unset, asserted by the HAL), and the FM config of the
driver is `aud_path = FM_AUD_I2S`, `i2s mode = MASTER`, `rate = 32K`, `i2s_pad = FM_I2S_PAD_CONN`. So FM audio
is digital: the FM core is I2S master at 32 kHz, CONSYS forwards it over an internal pad to the "2nd I2S input"
of the AFE (`AFE_I2S_CON`), which the HAL configures slave, 16-bit, I2S format, pad = CONSYS, phase-shift fix on
(`Set2ndI2SInConfig()`, vendor `sound/soc/mediatek/mt_soc_audio_v1/mt_soc_afe_control.c`). Its samples are
interconnect inputs I00/I01. Both are confirmed in the MT8127 HAL binary: `AudioFMResourceManager::SetFmSourceModuleEnable()`
builds the `AudioDigtalI2S` with slave = 1 and pad select = !IsFmChipPadSelConnSys() = 0, so `AFE_I2S_CON`
bit 28 stays 0 (0x8000000c, then enable); and its interconnection table (`AudioInterConnection`, 19 outputs
per input) has I00→O05 = 0x28 bit 16, I01→O06 = 0x28 bit 22, I05→O03 = 0x24 bit 21. When the FM rate differs
from the playback rate the HAL also enables the I2S-in ASRC (`SetI2SASRCConfig`, AFE 0x550-0x56c); capturing at
32 kHz needs no ASRC. Its direct mode routes I00/I01 → O15/O16 → hardware gain 1 → I12/I13 → O03/O04.

Mainline (kernel branch `6.18/test/fm-audio`):

- `ASoC: mediatek: mt8127: allocate the capture interrupt on open` (IRQ2 is shared by the capture memifs);
- `ASoC: mediatek: mt8127: add the I2S input and the AWB capture interface`: BE DAI `I2S` (capture, 32 kHz,
  `AFE_I2S_CON = 0x8000000d`, `AUDIO_TOP_CON0` bit 6 = I2S clock), FE DAI `AWB` (memif 0x70-0x7c, rate in
  `AFE_DAC_CON1[15:12]`, enable `AFE_DAC_CON0` bit 6), interconnect switches `O05 I00` (`AFE_CONN2` bit 16) and
  `O06 I01` (`AFE_CONN2` bit 22) from the vendor interconnection table;
- `ASoC: mediatek: mt8127-mt6323: add the FM capture links`: PCM device 2 "FM Capture" (FE on AWB) and the
  back-end "FM I2S" (dummy codec).

The FM audio is therefore a 32 kHz stereo capture device that is looped to the playback device (the stock
"indirect" FM mode). Mixer settings, once:

    amixer -c 0 cset name='O05 I00 Switch' on
    amixer -c 0 cset name='O06 I01 Switch' on
    # playback path, as for normal audio (already in tb3-audio's asound.state)
    amixer -c 0 cset name='O03 I05 Switch' on
    amixer -c 0 cset name='O04 I06 Switch' on
    amixer -c 0 cset name='Headphone Switch' on      # or 'Speaker Switch'
    amixer -c 0 cset name='Headphone Playback Volume' 4

Loop (32 kHz end to end, no resampling; the DL path supports 32 kHz):

    arecord -D hw:0,2 -f S16_LE -r 32000 -c 2 -t raw -B 100000 | \
        aplay -D hw:0,0 -f S16_LE -r 32000 -c 2 -t raw -B 100000

(`alsaloop -C hw:0,2 -P hw:0,0 -f S16_LE -r 32000 -c 2` does the same if alsa-utils is built with alsaloop.)
AWB and VUL share IRQ2: recording the microphone and the FM at the same time fails with `-EBUSY`.

## Test plan (headphones plugged in: the headphone cable is the antenna)

    modprobe mtk_consys_fm                     # or insmod; WMT stack loaded (wlan not needed)
    dmesg | grep -i fm
      # mtk-consys-fm mtk-consys-fm: registered radio0
    v4l2-ctl -d /dev/radio0 --set-freq=98.0    # first open powers the FM core up
    dmesg | tail
      # FM core 0x6627 (or 0x6625), ECO 0x8a0x, DSP ROM v0
      # firmware v1 loaded (patch version 0x....)
      # FM on, 98.00 MHz
    v4l2-ctl -d /dev/radio0 --get-tuner        # signal, stereo/mono, RDS capability
    v4l2-ctl -d /dev/radio0 --freq-seek=dir=1,wrap=1,spacing=100000
    v4l2-ctl -d /dev/radio0 --get-freq
    v4l2-ctl -d /dev/radio0 --set-ctrl volume=12
    v4l2-ctl -d /dev/radio0 --list-ctrls
    fm-radio tune 98.0 / fm-radio seek up / fm-radio status / fm-radio rds 10   # tools/fm-radio.c
    # audio: mixer settings and the arecord | aplay loop above
    v4l2-ctl -d /dev/radio0 --set-ctrl mute=1  # powers the FM core off at this close

`tools/test-fm.sh [MHz]` copies the module, `fm-radio` and the firmware, loads the module, tunes, reads the
tuner and RDS (every tablet command killed after a few seconds) and saves the log in `/tmp/fm-test.log`.

What to look for if it fails: `op 0xNN timed out` names the first command the firmware did not answer;
`unexpected FM core id` means FSPI reads work but the A-die is not clocked; a seek that never finds a station
with good reception points at the thresholds (watch the `rssi/pamd/...` debug lines of the soft-mute tune).
If the PCM device delivers silence: check `AFE_I2S_CON` (expect 0x8000000d while capturing: slave, the FM core
clocks the I2S at 32 kHz) and `AFE_ASRC_CON0` bit 0 (without it the slave input delivers only zeros; it reads back 0), the CONSYS pin mux
(`echo 'hr 80050078' > .../reg`), FM 0x9b (0xf9ab) and host 0x80101054 (0x3f35).

## Known gaps and risks

- Nothing has run on hardware; every value comes from the stock sources.
- Seek is a software sweep (stock behaviour), about 205 commands for a full band; there is no scan or AF.
- The audio needs the user-space loop: there is no hostless "direct" path (stock: I00/I01 → O03/O04 with the
  ADDA DL at 32 kHz) because the ADDA back-end and the MT6323 codec rate are only set up by a PCM stream.
- The FM core keeps playing after close when unmuted (by design); the I2S capture runs only while the PCM
  device is open.
- Coexistence: FM shares the CONSYS with WiFi and BT. WMT reference-counts the chip power, so FM on/off does not
  disturb WiFi. AIF_2 reprograms the CONSYS audio pins for FM I2S; BT PCM audio (AIF_1/3) and FM I2S at the same
  time is untested. The stock desense handling (MCU/GPS clock hopping, I2S hopping for MT6592/MT6752) is not
  ported; the TDD and HL/FA/ATJ channel tables are.
- A WMT whole-chip reset marks the FM state lost: commands fail with -EIO until the device is closed and opened
  again.
- Loudness: volume 15 is the stock default of the FM core; the analog gain is the codec's
  `Headphone Playback Volume`. The AFE has no digital gain on this path.
- RSSI-to-signal mapping (-120..-40 dBm) is a guess to be tuned with real readings.
