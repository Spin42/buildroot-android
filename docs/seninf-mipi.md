# MT8127 sensor interface (SENINF), TG, MIPI CSI-2 receiver, MCLK: register-level notes

Source of truth: the stock userspace library `libcamdrv.so` (ARM Thumb-2, not stripped), classes `SeninfDrvImp`
and `SensorHalImp`, plus the stock 3.10.54 kernel (`arch/arm/mach-mt8127/camera_isp.c`, `mt_clkmgr.c`,
`kd_camera_hw.c`) and the mainline `pinctrl-mtk-mt8127.h` pin table.

Conventions

- Function addresses are ELF virtual addresses as shown by `nm`/radare2 (for example `SeninfDrvImp::initTg1CSI2` =
  `0x12314`). Ghidra shows the same code at +0x10000.
- Evidence tags: **[C]** = read directly from the disassembly (certain). **[K]** = confirmed by stock kernel source
  or mainline tables. **[I]** = inferred (names, meaning of bits); the register offsets and values are still
  literal, only the interpretation is a guess.
- `rd(a)`/`wr(a,v)`/`rmw(a, clr_mask, set_val)` are 32-bit accesses. `rmw` means `(rd(a) & ~clr_mask) | set_val`.
- Physical addresses are given for each block; register offsets are relative to the stated block.

---

## 1. Register-base pointers inside `SeninfDrvImp`

Set up in `SeninfDrvImp::init()` (`0x1301c`), torn down in `uninit()` (`0x12e70`). The object is opened on
`/dev/camera-isp`; every window is an `mmap` of that fd with the physical address as the file offset. [C]

| Object field | Value | Physical base, length | What it is | Offsets used |
|---|---|---|---|---|
| `this+0x10` | fd | | `/dev/camera-isp` | |
| `this+0x18` | `IspDrv::getRegAddr()` | PA 0x15000000, 0x10000 (IspDrv's own mmap, `decall.c` mmap at 0x15000000) | CAM/ISP window. Offsets are physical offsets from 0x15000000, so 0x4410 is PA 0x15004410. Call it `isp`. | 0x4098, 0x409c, 0x4410, 0x4414, 0x4418, 0x441c, 0x4420, 0x4460..0x4474 (flash) |
| `this+0x1c` | mmap | PA 0x15000000, 0x10000 | IMGSYS window containing SENINF/TG-clock regs at 0x8000+ and the MIPI-RX CSI2 regs. Call it `sen`. | 0x8000, 0x8010, 0x8014, 0x8018, 0x802c, 0x8100..0x8128, 0x8300, 0x8304, 0x8400, 0x8404, 0x8614..0x8620 |
| `this+0x20` | mmap | PA 0x10000000, 0x200 | TOPCKGEN. Never dereferenced by the functions in this document (clock mux is switched by an ioctl into the kernel). | |
| `this+0x24` | `this+0x20 + 0x50` | PA 0x10000050 | pointer to CLK_CFG_1 (camtg mux). Stored but not used by any function read here. | |
| `this+0x28` | unused | | `uninit` unmaps it but `init` never maps it | |
| `this+0x2c` | `gpio + 0xb60` | PA 0x10005b60 | GPIO drive-strength register for pins 117..120 (CMDAT0, CMDAT1, CMMCLK, CMPCLK) [K: `pinctrl-mt8127.c` `MTK_PIN_DRV_GRP(117..120, 0xb60, 12, 1)`] | bits 15:12 |
| `this+0x3c` | mmap | PA **0x10010000**, 0x1000 | MIPI config page (stock kernel names it `MIPI_CONFIG_BASE` = 0xF0010000). | |
| `this+0x40` | `this+0x3c + 0x800` | PA **0x10010800** | **MIPI RX analog (CSI0/CSI1 PHY)**, call it `ana`. | 0x00, 0x04, 0x08, 0x0c, 0x10, 0x20, 0x24, 0x48, 0x4c, 0x50 |
| `this+0x44` | mmap | PA 0x1500C000, 0x100 | MIPI RX digital config, call it `rx`. | 0x24, 0x38, 0x3c, 0x44, 0x48 |
| `this+0x48` | mmap | PA 0x10005000, 0x1000 | GPIO block, call it `gpio`. | 0x740, 0x750, 0x760, 0x770, 0x780, 0x910, 0xb60 |
| `this+0x50` | mmap | PA 0x10206000, 0x400 | eFuse, call it `efuse`. | 0x170 |
| `this+0x54` | int | | **sensor-device mask** set by `SeninfDrv::sendCommand(0x1000, mask)` (called from `SensorHalImp::sendCommand`): bit0 = main, bit1 = sub. Selects which MIPI port is configured (section 3.1). | |
| `this+0x58`, `+0x5c` | int | | thresholds compared by `checkSeninf1Input`. Never written anywhere in libcamdrv (stay 0). | |

`SeninfDrvImp::sendCommand` (`0x11d44`): cmd 0x1000 stores `arg` at `this+0x54`; cmd 0x2001 returns `this+0x1c`
(the SENINF virtual base); anything else returns -1. [C]

### 1.1 Which physical base is the MIPI analog block? (answer)

`init()` maps PA **0x10010000** (literal at 0x133a4 = 0x10010000) and every analog access uses `base+0x800`, so the
analog registers live at **PA 0x10010800..0x10010850**. The stock kernel's `MIPIRX_ANALOG_ADDR 0x10010000`
(`camera_isp.c:119`) matches. The `MIPI_RX_ANA_BASE 0xF0215000` in `mt_reg_base.h:73` is a stale definition: it is
referenced nowhere else in the tree and does not match anything libcamdrv touches. Use 0x10010800. [C][K]

Other windows the kernel allows (`camera_isp.c:109-135`): IMGSYS 0x15000000, SENINF 0x15008000, PLL
0x10000000, MIPI RX config 0x1500C000, MIPI RX analog 0x10010000, GPIO 0x10005000, EFUSE 0x10206000. [K]

### 1.2 Naming used below (inferred stock-style names) [I]

| Address | Name |
|---|---|
| 0x15008000 | SENINF_TOP_CTRL |
| 0x15008010 | SENINF1_CTRL |
| 0x15008014 / 0x15008018 | SENINF1_INTEN / SENINF1_INTSTA (the stock kernel labels them "SENINF1_INT_STA", `camera_isp.c:944`) |
| 0x1500802c | SENINF1 detected size register (checked by `checkSeninf1Input`) |
| 0x15008100 | SENINF1_CSI2_CTRL |
| 0x15008104 | SENINF1_CSI2_DELAY |
| 0x15008108 / 0x1500810c | SENINF1_CSI2_INTEN? / SENINF1_CSI2_INTSTA? (ambiguous, see 8) |
| 0x15008128 | SENINF1_CSI2_LNMUX (lane map, values 0xe4 / 0x1b) |
| 0x15008300 | SENINF_TG1_PH_CNT |
| 0x15008304 | SENINF_TG1_SEN_CK |
| 0x15004410 | CAM_TG_SEN_MODE |
| 0x15004414 | CAM_TG_VF_CON (kernel `ISP_REG_ADDR_TG_VF_CON = ISP_ADDR+0x414`, tests bit0 as "VF enable") [K] |
| 0x15004418 / 0x1500441c | CAM_TG_SEN_GRAB_PXL / CAM_TG_SEN_GRAB_LIN |
| 0x15004420 | CAM_TG_PATH_CFG |
| 0x15004098 / 0x1500409c | SET / CLR companion pair carrying the TG format (CAM_CTL_FMT_SEL-style) |

---

## 2. Function-by-function register behaviour

`this` is the `SeninfDrvImp*`. Argument names are mine; the stock names are in the demangled symbol.

### 2.1 `init()` (0x1301c) [C]

```
fd = open("/dev/camera-isp", O_RDWR)
isp = IspDrv::createInstance(0)->getRegAddr()
sen = mmap(PA 0x15000000, 0x10000)      // this+0x1c
top = mmap(PA 0x10000000, 0x200)        // this+0x20 ; this+0x24 = top+0x50
rx  = mmap(PA 0x1500C000, 0x100)        // this+0x44
anaBase = mmap(PA 0x10010000, 0x1000)   // this+0x3c ; ana = anaBase+0x800 (this+0x40)
gpio = mmap(PA 0x10005000, 0x1000)      // this+0x48 ; this+0x2c = gpio+0xb60
efuse = mmap(PA 0x10206000, 0x400)      // this+0x50

rmw(gpio+0x770, clr 0xf000, set 0x1000)           // pin 119 (CMMCLK) -> mode 1 (see 4.5)
rmw(sen+0x8010, clr 0xf000, set 0x8000)           // SENINF1_CTRL[15:12] = 8
if (once-per-process flag == 0) {                  // eFuse trim of the MIPI analog block
    e = rd(efuse+0x170)
    if (e & 0x78000000) {                          // bits 30:27 non-zero
        v = rd(ana+0x24)
        n = (((v >> 12) & 0xf) + (e >> 27)) & 0xf
        wr(ana+0x24, (v & 0xffff0fff) | (n << 12))  // ana+0x24 [15:12] += efuse[30:27] (mod 16)
    }
    if (e & 0x07800000) {                          // bits 26:23 non-zero
        v = rd(ana+0x24)
        n = (((v >> 8) & 0xf) + (e >> 23)) & 0xf
        wr(ana+0x24, (v & 0xfffff0ff) | (n << 8))   // ana+0x24 [11:8] += efuse[26:23] (mod 16)
    }
    flag++
}
refcount++
```

The `gpio+0x770` write is `(v & 0xfff) | 0x1000`: it keeps the fields of pins 115..118 (bits 11:0), clears bits
15:12 (the 3-bit field of pin 119 is bits 14:12, bit 15 is unused) and writes mode 1 for pin 119. [C]
`ana+0x24` [15:12] and [11:8] are analog trim fields that are offset by factory eFuse data [I]. If the eFuse
window is unreadable on mainline, skipping this step leaves the reset value; the effect on image quality is unknown.

### 2.2 `uninit()` (0x12e70) [C]

On the last reference:

```
setTg1CSI2(0,0,0,0,0,0,0,0)               // no-op, see 2.5
setTg1PhaseCounter(0,0,0,0,0,0,0)         // see 2.8 for the resulting register values
rmw(ana+0x24, clr 0x3, 0)
rmw(ana+0x20, clr 0x43, 0)                // 0xffffffbc
rmw(ana+0x00, clr 0x1f, 0)
rmw(ana+0x04, clr 0x1, 0); rmw(ana+0x08, ...); rmw(ana+0x0c, ...); rmw(ana+0x10, ...)   // each clr bit0
rmw(gpio+0x770, clr 0xf000, set 0x1000)   // keep pin 119 in mode 1 (value (v & 0xfff) | 0x1000)
munmap all windows; close(fd)
```

### 2.3 `setTg1IODrivingCurrent(unsigned long v)` (0x11c2c) [C]

```
rmw(gpio+0xb60, clr 0xf000, set v)         // v is pre-shifted
```

`SensorHalImp::setSensorIODrivingCurrent` (`0x15f34`) passes, from `SensorDrivingCurrent` of the sensor info:
0 -> 0x0000, 1 -> 0x2000, 2 -> 0x4000, 3 -> 0x6000, anything else -> 8 (a stock bug: bit 3 is outside the cleared
field). The mainline pinctrl `MTK_PIN_DRV_GRP(117..120, 0xb60, 12, 1)` (2/4/6/8 mA) confirms the field. [C][K]

### 2.4 `setTg1MCLKEn(bool en)` (0x11c46) [C]

```
rmw(sen+0x8300, clr 0x20000000, set en << 29)    // SENINF_TG1_PH_CNT bit 29 = MCLK (ADCLK) enable
```

Same bit as the kernel's `ISP_MCLK1_EN()` (`camera_isp.c:5588`, `ISP_ADDR+0x4300`, ISP_ADDR = 0x15004000, so PA
0x15008300). Within libcamdrv only `SensorHalImp::setATVStart` calls it (value 0). The kernel's `kd_camera_hw.c`
calls `ISP_MCLK1_EN(1)` during sensor power-on and `ISP_MCLK1_EN(0)` during power-down. [C][K]

### 2.5 `setTg1CSI2(...)` (0x11e18)

Signature (mangled order): `setTg1CSI2(unsigned long dataTermDelay, unsigned long dataSettleDelay,
unsigned long clkTermDelay, unsigned long vsyncType, unsigned long dlaneNum, unsigned long csi2En,
unsigned long dataHeaderOrder, unsigned long dataFlow)`. Names are the MediaTek names that fit the arguments
`SensorHalImp::setCSI2Config` passes (see 3.2); the register behaviour below is read from the code. [C]

```
if (csi2En != 1) return 0;                   // csi2En==0 is a complete no-op (uninit/setCSI2Config(0) do nothing)

rmw(sen+0x8010, clr 0xf000, 0)               // SENINF1_CTRL[15:12] = 0 (CSI-2 source)
wr (sen+0x8104, (dataSettleDelay & 0xff) << 16)          // CSI2_DELAY[23:16]; whole register written
// dataTermDelay and clkTermDelay are only logged; they never reach a register in this library
rmw(sen+0x8108, 0, 0x7)                       // superseded by the next write (stock quirk)
wr (sen+0x8108, rd(sen+0x810c) | 0x10)        // writes (value read from 0x810c) | 0x10 into 0x8108
wr (sen+0x8100,
      (dataFlow << 17)
    | 0x411
    | (dataHeaderOrder << 5)
    | (vsyncType << 13)
    | (((1 << dlaneNum) - 1) << 1))           // dlaneNum is the lane-count enum: 0=1 lane, 1=2, 2=3, 3=4
v = rd(sen+0x8010) & 0xfffffbfc               // also clears bit 10 and bits 1:0
wr(sen+0x8010, v | 3)
wr(sen+0x8010, v)                             // reset pulse on SENINF1_CTRL[1:0]
```

Exact decode of the 0x8100 write: `0x411 = bit0 | bit4 | bit10`. Bits [3:1] receive `(1<<n)-1` shifted by one, so a
1-lane sensor writes 0 there, a 2-lane sensor 0x2 (bit1), a 3-lane 0x6, a 4-lane 0xe. [C]
Interpretation [I]: bit0 = CSI2 enable, bits 3:1 = extra data-lane enables (lane 0 is always on), bit4 = ECC
enable, bit10 = CRC enable (both always set), bit5 = `dataHeaderOrder` (stock passes `SensorPacketECCOrder`),
bit13 = `vsyncType` (stock passes `SensorVsyncPolarity`), bit17 = `dataFlow` (stock passes 0).
Settle delay lands in a single byte field [23:16]. The stock `MIPIDataLowPwr2HighSpeedTermDelayCount` and
`MIPICLKLowPwr2HighSpeedTermDelayCount` are therefore ignored by this library build (term delay fields stay 0). [C]

### 2.6 `initTg1CSI2(bool en)` (0x12314)

The sensor-device mask `this+0x54` selects the port: bit0 = **CSI0** (main; physical pins RDN0/RDP0/RDN1/RDP1/RCN/RCP/
RDN2/RDP2/RDN3/RDP3, 4 data lanes capable), bit1 = **CSI1** (sub; pins RCN_A/RCP_A/RDN1_A/RDP1_A/RDN0_A/RDP0_A, 2 data
lanes). Mapping of the GPIO pin numbers in section 4.5 and 3.3. [C][K]

Disable (`en == 0`): [C]

```
rmw(ana+0x24, clr 0x1, 0)
rmw(gpio+0x910, 0, set 0x80)                         // IES of pins 117..120 on [K]
rmw(gpio+0x750, keep 0x3f, set 0x2480)               // (v & 0x3f) | 0x2480
wr (gpio+0x760, 0x2492)
rmw(gpio+0x770, (v & 0xffc0) | 0x12);  then  (v & 0x0e3f) | 0x1000
rmw(gpio+0x780, (v & 0xfff8) | 1)
rmw(ana+0x48, 0, set 0x3c0)
rmw(ana+0x4c, 0, set 0x01041041)                     // literal at 0x124fc
rmw(ana+0x50, 0, set 0x1)
rmw(ana+0x00, clr 0x18, 0)
rmw(ana+0x04, clr 0x08, 0)
rmw(ana+0x08, clr 0x08, 0)
rmw(ana+0x0c, clr 0x18, 0)
rmw(ana+0x10, clr 0x18, 0)
return
```

Enable (`en != 0`): [C]

```
rmw(ana+0x48, clr 0x3c0, 0)
rmw(ana+0x4c, and 0xfefbefbe)                        // literal at 0x12500; clears bits 0,6,12,18,24
rmw(ana+0x50, clr 0x1, 0)
if (devmask & 1) {                                   // ---- CSI0 ----
    rmw(gpio+0x910, clr 0x80, 0)
    rmw(gpio+0x740, (v & 0x7) | 0x1248)              // pins 101..104 -> mode 1 (pin 100 field preserved)
    wr (gpio+0x750, 0x1249)                          // pins 105..109 -> mode 1
    rmw(gpio+0x760, (v & 0xfff8) | 1)                // pin 110 -> mode 1
    rmw(ana+0x00, 0, 0x08); rmw(ana+0x04, 0, 0x08); rmw(ana+0x08, 0, 0x08)
    rmw(ana+0x0c, 0, 0x08); rmw(ana+0x10, 0, 0x08)
    rmw(ana+0x00, clr 0x10, 0); rmw(ana+0x0c, clr 0x10, 0); rmw(ana+0x10, clr 0x10, 0)
    rmw(ana+0x04, 0, 0x01); rmw(ana+0x08, 0, 0x01); rmw(ana+0x0c, 0, 0x01); rmw(ana+0x10, 0, 0x01)
    rmw(ana+0x20, clr 0x0c, 0)
    rmw(rx+0x24, (v & 0x00ffffff) | 0xe4000000)      // lane map = identity
} else if (devmask & 2) {                            // ---- CSI1 ----
    rmw(gpio+0x910, clr 0x80, 0)
    rmw(gpio+0x760, (v & 0x7) | 0x1248)              // pins 111..114 -> mode 1 (pin 110 field preserved)
    rmw(gpio+0x770, (v & 0xffc0) | 0x9)              // pins 115,116 -> mode 1
    rmw(ana+0x00, 0, 0x10); rmw(ana+0x0c, 0, 0x10); rmw(ana+0x10, 0, 0x10)
    rmw(ana+0x00, clr 0x08, 0); rmw(ana+0x04, clr 0x08, 0); rmw(ana+0x08, clr 0x08, 0)
    rmw(ana+0x0c, clr 0x08, 0); rmw(ana+0x10, clr 0x08, 0)
    rmw(ana+0x0c, 0, 0x01); rmw(ana+0x10, 0, 0x01)
    rmw(ana+0x20, 0, 0x0c)
    usleep(1)
    rmw(rx+0x24, (v & 0x00ffffff) | 0x1b000000)      // reversed lane map
}
// common tail (always executed)
rmw(ana+0x24, 0, 0x3);          usleep(30)
rmw(ana+0x20, 0, 0x1);          usleep(1)
rmw(ana+0x00, 0, 0x1)                                // clock-lane receiver enable
rmw(rx+0x24, 0, 0x80000)                             // bit 19
rmw(rx+0x38, 0, 0x1)
wr (rx+0x3c, 0x1541)
rmw(rx+0x38, 0, 0x4)
usleep(500)
s44 = rd(rx+0x44); s48 = rd(rx+0x48)                 // status; stock only logs an error if
   // !((s44 & 0x10101) && (s48 & 0x101))              // it is "not ready" (does not abort)
rmw(rx+0x38, clr 0x1, 0)
rmw(ana+0x20, clr 0x20, 0)
rmw(ana+0x04, clr 0x400000, 0); rmw(ana+0x08, ...); rmw(ana+0x0c, ...); rmw(ana+0x10, ...)   // bit 22
rmw(ana+0x20, 0, 0x2)
if (devmask & 1)      wr(sen+0x8128, 0xe4)           // SENINF1_CSI2_LNMUX
else if (devmask & 2) wr(sen+0x8128, 0x1b)
```

Lane enables do **not** depend on the sensor's lane count: the CSI0 path enables all four data-lane receivers
(ana+0x04/08/0c/10 bit0) plus the clock lane (ana+0x00 bit0). The number of lanes the digital side actually
decodes is chosen only by `SENINF1_CSI2_CTRL[3:1]` in `setTg1CSI2`. [C]

Meaning of the analog bits [I] (offsets and values are certain, names are guesses): per-lane CON registers
`ana+0x00` (clock lane), `+0x04`, `+0x08`, `+0x0c`, `+0x10` (data lanes). Bit0 = lane HS receiver enable, bit3 =
"routed to CSI0" select, bit4 = "routed to CSI1" select (only meaningful in `+0x00/+0x0c/+0x10`, which the CSI1
port shares with CSI0), bit22 and bit24+ belong to the deskew calibration (see 2.14). `ana+0x48/4c/50` are
power-down / clamp bits asserted when the PHY is off and deasserted before it is enabled. `ana+0x20/0x24` hold
the bias/LDO sequencing bits (turned on in the order shown above, with the 30 us and 1 us waits).

### 2.7 `setTg1InputCfg(pad, src, fmt, bits)` (0x12078) [C]

Only `pad` (r1), `src` (r2) and `fmt` (r3) are read; the 4th argument (stack) is ignored.

```
v = rd(sen+0x8010) & ~0xf000f000
v |= 0x80000000 | ((pad & 7) << 28)
if (src != 8) v |= (src << 12) & 0xffff
wr(sen+0x8010, v)
rmw(isp+0x4420, clr 0x3, 0)
rmw(isp+0x409c, 0, 0x70000)                    // clear field via the CLR companion
rmw(isp+0x4098, 0, (fmt & 7) << 16)            // set field via the SET companion

if (src == 8) {
    rmw(isp+0x4410, clr 0x300, 0)
    v = rd(sen+0x8010) & ~0x0ff00000 & ~0x000f0000
    if (fmt != 7) wr(sen+0x8010, v | 0x6d80000 | 0x70000)            // 0x06df0000
    else {        wr(sen+0x8010, v | 0x6180000 | 0x60000)             // 0x061e0000
                  rmw(sen+0x803c, clr 0x3e00, set 0x2c00) }
} else {
    rmw(isp+0x4410, 0, 0x100)
    v = rd(sen+0x8010) & ~0x0ff00000 & ~0x000f0000
    wr(sen+0x8010, v | 0x6d80000 | 0x70000)                           // 0x06df0000
}
if (fmt == 7) {                                // JPEG path
    rmw(sen+0x8010, 0, 0x100); rmw(isp+0x4410, 0, 0x2)
    rmw(isp+0x409c, 0, 0x1000000); rmw(isp+0x4098, 0, 0x1000000); rmw(isp+0x4420, 0, 0x10)
} else {                                       // every other format
    rmw(sen+0x8010, clr 0x100, 0); rmw(isp+0x4410, clr 0x2, 0)
    rmw(isp+0x409c, 0, 0x1000000); rmw(isp+0x4098, clr 0x1000000, 0); rmw(isp+0x4420, clr 0x10, 0)
}
```

Values passed by `SensorHalImp::setConf` (0x25560) per sensor type (type = `getCurrentSensorType`, derived from
`SensorOutputDataFormat`: 0..3 RAW10 -> 0, 12..15 RAW8 -> 6, 4..7 UYVY/VYUY/YUYV/YVYU -> 1, 8..11 CbYCrY.. -> 2): [C]

| Sensor type | pad | src | fmt | meaning of fmt [I] |
|---|---|---|---|---|
| 0 RAW10 | 0 | 3 | 1 | RAW 10-bit |
| 6 RAW8 | 4 (or 3 if driver says "8-bit on 9:2") | 1 | 0 | RAW 8-bit |
| 1 YUV422 | 4 (or 3) | 8 | 3 | YUV422 |
| 2 YCbCr | 4 (or 3) | 4 | 3 | YUV422 |
| 3 / 4 / 5 | 4 (or 3) | 1 | 5 / 6 / 7 | RGB565 / RGB888 / JPEG |

The final contents of `SENINF1_CTRL` after the full MIPI configuration for a non-JPEG sensor: bit31 = 1, [30:28]
= pad, [27:16] = 0x6df, [15:12] = 0 (cleared again by `setTg1CSI2`), bit8 = 0, bit10 = 0, bit9 = value passed to
`setTg1SensorModeCfg`, [1:0] = 0. TG format ends in `isp+0x4098[18:16]`; for 10-bit RAW that is 1, for YUV422 3.

### 2.8 `setTg1PhaseCounter(pcEn, mclkSel, cnt, clkPol, fallCnt, riseCnt, padPclkInv)` (0x12c2c)

All seven are `unsigned long`. `fallCnt` and `riseCnt` are accepted but never used (only logged). [C]

```
if (mclkSel == 1 || mclkSel == 2)
    ioctl(fd, ISP_SENSOR_FREQ_CTRL = 0x40046b0e, &mclkSel)       // kernel: clkmux_sel(MT_MUX_CAMTG, mclkSel)
rmw(sen+0x8000, 0, 0x400)                                         // SENINF_TOP_CTRL bit10 [I: TG1 clock gate]
fall = (cnt < 2) ? 1 : ((cnt + 1) >> 1)
wr (sen+0x8304, (fall & 0x3f) | ((cnt & 0x3f) << 16))             // CLKFL = fall, CLKCNT = cnt, CLKRS = 0
v  = rd(sen+0x8300) & ~0xb0000047                                 // clears bits 31,29,28,6,2,1,0
v |= 0x20000001                                                   // bit29 MCLK enable, bit0
v |= (padPclkInv & 1) << 6
v |= (pcEn << 31)
v |= (clkPol & 1) << 28
v |= ((cnt & 1) ^ 1) << 2
wr(sen+0x8300, v)
rmw(isp+0x4410, 0, 0x1)                                            // CAM_TG_SEN_MODE.bit0
usleep(1000)
```

Field table for SENINF_TG1_PH_CNT (0x15008300) [C for the bit positions, I for the names]:

| Bits | Set by | Meaning |
|---|---|---|
| 31 | `pcEn` | PCEN: phase counter enable |
| 30 | preserved | (not touched by the library) |
| 29 | constant 1 here; also `setTg1MCLKEn` and kernel `ISP_MCLK1_EN` | ADCLK_EN: gate of the CMMCLK output |
| 28 | `clkPol & 1` | CLKPOL (inverts the generated clock) |
| 6 | `padPclkInv & 1` | PAD_PCLK_INV |
| 2 | `(cnt & 1) ^ 1` | CLKFL_POL (odd/even duty compensation) |
| 0 | constant 1 | TGCLK_SEL: use the phase-counter clock |

SENINF_TG1_SEN_CK (0x15008304): `[5:0]` CLKFL (fall point), `[13:8]` CLKRS (rise point, always 0), `[21:16]` CLKCNT
(period minus 1). The output frequency is `f_src / (CLKCNT + 1)` with a 50 percent duty cycle. [C][I]

`uninit()` calls this with all zeros: result `PH_CNT = (old & ~0xb0000047) | 0x20000005` (PCEN off, but bit29 and
bit0 left set), `SEN_CK = 0x00000001`. [C]

The mux argument is the kernel `MT_MUX_CAMTG` input index, see section 4.

### 2.9 `setTg1GrabRange(xStart, xEnd, yStart, yEnd)` (0x11fd4) [C]

```
wr(isp+0x4418, (xStart & 0x7fff) | ((xEnd & 0x7fff) << 16))    // CAM_TG_SEN_GRAB_PXL
wr(isp+0x441c, (yStart & 0x7fff) | ((yEnd & 0x7fff) << 16))    // CAM_TG_SEN_GRAB_LIN
```

`setConf` computes `xStart = SensorGrabStartX + (srcW - cropW)/2`, `xEnd = xStart + cropW`, same for Y, and for
YUV types (1, 2, 3, 4, 5) the X span is doubled (two bytes per pixel): `xStart = (SensorGrabStartX - cropW) + srcW`,
`xEnd = xStart + 2*cropW`. [C]

### 2.10 `setTg1SensorModeCfg(a, b)` (0x12024) [C]

```
rmw(sen+0x8010, clr 0x600, set ((b & 1) << 9) | ((a & 1) << 10))   // SENINF1_CTRL bit10 = a, bit9 = b
rmw(isp+0x4410, 0, 0x5)                                              // CAM_TG_SEN_MODE bits 0 and 2
```

`setConf` passes `(SensorHsyncPolarity, SensorVsyncPolarity)` (each forced to 0 if greater than 1) [I: order
guessed from which global byte (+0x14, +0x15 of the info struct) goes to which argument]. Bit10 is cleared again by
`setTg1CSI2`, so for MIPI only bit9 survives.

### 2.11 `setTg1ViewFinderMode(spMode, spDelay)` (0x121bc) [C]

```
rmw(isp+0x4414, clr 0x1f02, set 0x1000 | ((spDelay & 7) << 8) | ((spMode & 1) << 1))
```

`CAM_TG_VF_CON`: bit1 = single-shot mode (`spMode = 1 - u4IsContinuous`), [10:8] = single-mode delay (stock: sensor
`SensorInterruptDelayLines`, or 0 when bypass), bit12 set. Bit0 (VFDATA_EN) is not touched. [C]/[I]

### 2.12 `resetSeninf()` (0x12714) [C]

```
v = rd(sen+0x8010)
wr(sen+0x8010, v | 3)
usleep(10)
wr(sen+0x8010, v & ~3)
```

`SensorHalImp::setConf` calls it (through vtable +0x70) right after `setScenario` and before programming grab/format.

### 2.13 `resetCSI2()` (0x11f8c) [C]

```
wr(sen+0x810c, 0)
x = rd(sen+0x810c)
wr(sen+0x810c, x | 0x10)
v = rd(sen+0x8100)
wr(sen+0x8100, v | 0x1000)        // bit 12 pulse = CSI2 soft reset [I]
wr(sen+0x8100, v & ~0x1000)
```

Called only from `SensorHalImp::reset()` (0x14478), after the sensor driver `close()` and before `open()`.

### 2.14 `autoDeskewCalibration()` (0x12754) [C for the register accesses, I for the meaning]

Never called anywhere in libcamdrv (no vtable +0x6c call site in the whole library; no other library imports
`SeninfDrv`). Documented for completeness; **not part of the stock bring-up path**.

```
rmw(ana+0x00, clr 1<<20, 0); rmw(ana+0x04/08/0c/10, clr 1<<24, 0)
wr(sen+0x861c, 0x12)                       // debug select
wr(sen+0x8614, 0x80007fff)                 // literal at 0x12978
if (rd(sen+0x8618) & 0xfb8) log_error
wr(sen+0x8618, 0x7fff)                     // clear errors
for each data lane L in {ana+0x04, 0x08, 0x0c, 0x10}:
    rmw(L, 0, 1<<24)                       // lane deskew enable
    for (t = 0; t < 16; t++) {
        rmw(L, clr 0xe000000 .. i.e. bits 28:25, set t << 25)     // delay tap
        p = rd(sen+0x8620); while (rd(sen+0x8620) == p) ;         // wait for the debug counter to move
        if (rd(sen+0x8618) & 0xfb8) { wr(sen+0x8618, 0x7fff); usleep(5);
                                       if (rd(sen+0x8618) & 0xfb8) break; }
    }
    tap[L] = t at break (or 15); rmw(L, clr 1<<24, 0)
min = min(tap[0..3])
for each lane: rmw(L, clr bits 28:25, set ((tap[L] - min) & 0xf) << 25)
rmw(ana+0x00, 0, 1<<20); rmw(ana+0x04/08/0c/10, 0, 1<<24)
for (t = 0; t < 16; t++) { rmw(ana+0x00, clr bits 24:21, set t << 21); ...same error test... }   // clock lane sweep
centre the clock tap against the data-lane taps (writes bits 24:21 of ana+0x00 and re-adjusts bits 28:25 of the lanes)
```

It sweeps 4-bit delay taps and watches the CSI-2 error flags at `0x15008618 & 0xfb8`; it requires a streaming
sensor. The 0x8614/0x8618/0x861c/0x8620 registers behave like INT_EN / INT_STATUS / DEBUG_SEL / DEBUG_PORT of
the newer "NCSI2" block seen in the Kobo `mtk_mipicsi0.c` (offsets +0xb0..+0xbc there). Since nothing calls it
the PHY runs with the reset values of the tap fields. **Deskew calibration is not required** for the stock sensors
(gc0310, gc2355, sp2508, sp0a20). [C][I]

### 2.15 `checkSeninf1Input()` (0x11f44) [C]

```
x = rd(sen+0x802c)
return ((x >> 16) < this->+0x58) || ((x & 0xffff) < this->+0x5c)     // thresholds are 0 -> always false
```

`0x802c` is a frame-size style status register (high half / low half, [I]: line count and pixel count seen by
SENINF1). The stock kernel prints it on a TG error (`camera_isp.c:4000+` commented block).

### 2.16 `waitSeninf1Irq(int mask)` (0x12274) [C]

```
for (i = 40; i > 0; i--) {
    s = rd(sen+0x8018)                      // SENINF1_INTSTA
    if (s & mask) return 0
    usleep(100000)
}
return -1                                   // 4 s timeout; status is not acknowledged here
```

This is a **polled** wait on SENINF1_INTSTA; the hardware IRQ line (SPI 143) is the ISP's, not SENINF's.

### 2.17 Other functions in the class [C]

- `setCCIR656Cfg(pol1, pol2, a, b)` (0x11ec8): `rmw(sen+0x8400, ...)` bit1 = `pol2`, bit3 = `pol1`; `wr(sen+0x8404, (a & 0xfff) | ((b & 0xfff) << 16))`. Parallel only, not used for MIPI.
- `setFlashA/B/En` (0x11c5c, 0x11cba, 0x11d08): flash timing at `isp+0x4460..0x4474`. Not needed.
- `readReg/writeReg/readRegs/writeRegs/holdReg/dumpReg`: wrappers around the kernel ioctls (hold = `0x40016b05`, dump = `0x6b09`).

---

## 3. Complete MIPI CSI-2 receive configuration

### 3.1 Port selection and pins

| Sensor device (mask) | Port | Pins (mainline pinctrl names, mode 1 = MIPI) | Analog regs touched | Lane map written to `rx+0x24[31:24]` and `sen+0x8128` |
|---|---|---|---|---|
| main (bit0) | CSI0, up to 4 data lanes | GPIO101 RDN0, 102 RDP0, 103 RDN1, 104 RDP1, 105 RCN, 106 RCP, 107 RDN2, 108 RDP2, 109 RDN3, 110 RDP3 | ana+0x00 (clock), +0x04, +0x08, +0x0c, +0x10 (data) | 0xe4 |
| sub (bit1) | CSI1 ("_A" pads), up to 2 data lanes | GPIO111 RCN_A, 112 RCP_A, 113 RDN1_A, 114 RDP1_A, 115 RDN0_A, 116 RDP0_A | ana+0x00 (clock), +0x0c, +0x10 (data) | 0x1b |

Pin evidence: GPIO mode register `n` is at `gpio + 0x600 + 0x10*n`, five 3-bit pin fields per register, pin `p` is
register `p/5`, field `p%5` (stock `mt_gpio_base.h`: `mode[29]` at 0x0600, 5 pins per register, 3 bits per pin) [K].
The values written by `initTg1CSI2` select mode 1 on pins 101..110 (CSI0) or 111..116 (CSI1), which are the
`RDN/RDP/RCN/RCP` functions in `pinctrl-mtk-mt8127.h`. The disable path puts the shared pins back to mode 2 (the
parallel `CMDATx`/`CMHSYNC`/`CMVSYNC` function) and pin 117 to GPIO. [K][C]

Which port the physical sensors of the Tab3 use (rear = main / front = sub is the stock convention) is not known
from the library alone. See 8.

### 3.2 Parameters given to `setTg1CSI2` (`SensorHalImp::setCSI2Config`, 0x15364) [C]

`setCSI2Config(1)` runs, for the main sensor (when `this+4 & 5` and its `SensroInterfaceType == 1` = MIPI), and for
the sub sensor (when `this+4 & 2`, same test on its own info struct):

```
setTg1CSI2(info->MIPIDataLowPwr2HighSpeedTermDelayCount,   // +0x66
           info->MIPIDataLowPwr2HighSpeedSettleDelayCount, // +0x67
           info->MIPICLKLowPwr2HighSpeedTermDelayCount,    // +0x68
           info->SensorVsyncPolarity,                      // +0x15
           info->SensorMIPILaneNumber,                     // +0x28 (0=1 lane, 1=2 lanes, 2=3, 3=4)
           1,                                              // csi2En
           info->SensorPacketECCOrder,                     // +0x6b
           0)                                              // dataFlow
```

Struct offsets match `ACDK_SENSOR_INFO_STRUCT` in the stock `kd_imgsensor_define.h` (verified: `SensroInterfaceType`
at +0x20, `SensorMIPILaneNumber` at +0x28, term/settle/clk-term bytes at +0x66/+0x67/+0x68, ECC order at +0x6b).
`setCSI2Config(0)` calls `setTg1CSI2` with all zeros (no-op). The stock sensors pass term 0, settle 14, clk-term 0,
`SensorPacketECCOrder = 1`, and `SensorVsyncPolarity = SENSOR_CLOCK_POLARITY_LOW = 1`
(gc0310 as shown in `gc0310mipi_yuv_Sensor.c:1578-1584, 1554`).

### 3.3 Resulting register values (settle 14, term 0, ECC order 1, dataFlow 0)

| Item | 1-lane | 2-lane |
|---|---|---|
| `sen+0x8104` CSI2_DELAY | 0x000e0000 | 0x000e0000 |
| `sen+0x8100` CSI2_CTRL, `vsyncType = 0` | 0x00000431 | 0x00000433 |
| `sen+0x8100` CSI2_CTRL, `vsyncType = 1` (stock gc0310) | 0x00002431 | 0x00002433 |
| `sen+0x8108` | `rd(0x810c) | 0x10` | same |
| `sen+0x8010` after the call | `[15:12]=0`, bit10=0, bits1:0 pulsed 3 -> 0 | same |
| `sen+0x8128` | 0xe4 (CSI0) / 0x1b (CSI1) | same |
| `rx+0x24[31:24]` | 0xe4 (CSI0) / 0x1b (CSI1) | same |

3-lane / 4-lane would be 0x437 / 0x43f (bits 3:1 = 0x3 / 0x7 shifted by one, i.e. 0x6 / 0xe) by the same formula; not used by the
stock sensors. [C]

### 3.4 Items you asked about

- **Lane count**: only `CSI2_CTRL[3:1]` (digital) plus the physical pin selection above (analog). [C]
- **Data termination / settle**: only the settle count (stock 14) is written, to `CSI2_DELAY[23:16]`; the two term
  counts (stock 0) are ignored. [C]
- **Packet / data type selection**: no data-type filter register is written in this library; the sensor format is
  selected on the TG side (`setTg1InputCfg` fmt/pad, section 2.7). [C]
- **ECC / CRC enable**: `CSI2_CTRL` constant 0x411 sets bit4 and bit10 always (inferred ECC and CRC enables);
  bit5 follows `SensorPacketECCOrder`. [C][I]
- **`autoDeskewCalibration`**: never called, not required (2.14). [C]
- **Analog PHY bias/LDO/termination enables and power-on order**: section 2.6 (enable path). Summary order:
  1. `ana+0x48 &= ~0x3c0`, `ana+0x4c &= 0xfefbefbe`, `ana+0x50 &= ~1` (release power-down bits)
  2. pin mux to mode 1, `gpio+0x910 &= ~0x80`
  3. lane control bits (bit3 or bit4 select, bit0 enables), `ana+0x20` bits 3:2 select CSI1 (set) or CSI0 (clear)
  4. `rx+0x24` lane-map byte
  5. `ana+0x24 |= 3`; wait 30 us
  6. `ana+0x20 |= 1`; wait 1 us
  7. `ana+0x00 |= 1`
  8. `rx+0x24 |= 0x80000`, then the `rx+0x38/0x3c` calibration pulse (`rx+0x38 |= 1`, `rx+0x3c = 0x1541`,
     `rx+0x38 |= 4`, wait 500 us, read `rx+0x44/0x48`, `rx+0x38 &= ~1`)
  9. `ana+0x20 &= ~0x20`, `ana+0x04/08/0c/10 &= ~(1<<22)`, `ana+0x20 |= 2`
  10. `sen+0x8128 = lane map`
- Status read after the 500 us wait: ready if `(rx+0x44 & 0x10101) != 0` and `(rx+0x48 & 0x101) != 0`
  (stock logs an error but continues otherwise). [C]
- Stock sequence position: `initCSI2Peripheral(1)` (PHY on) runs **before** `setCSI2Config(1)`
  (`SensorHalImp::init`, 0x2605c), then `setConf` runs `setTg1CSI2` again after `setTg1InputCfg`. [C]

---

## 4. MCLK (24 MHz sensor clock)

### 4.1 Where the frequency is chosen

`SensorHalImp::setTgPhase` (0x14bac), called from `SensorHalImp::init` after `initSensor` and `getSensorInfo` and
before `setSensorIODrivingCurrent` and `initCSI2Peripheral`. For each active sensor it reads `SensorClockFreq` (byte
at struct +0x08, MHz). The kernel never selects the frequency; there is no `SENSOR_FEATURE_SET_MCLK` handling in
`kd_sensorlist.c` of this kernel, and the kernel only toggles the gate bit (`ISP_MCLK1_EN`). [C][K]

```
f   = SensorClockFreq * 1000            // kHz
if (f - 3250 > 100749) error -EINVAL    // accepts 3250..103999 kHz
if (f % 48 == 0) { num = 48000;  mux = 1; }        // source: univpll_d26 = 48 MHz
else             { num = 208000; mux = 2; }        // source: univpll2_d2 = 208 MHz  (literal at 0x14e08 = 0x32c80)
div = (num + f/2) / f                              // integer, round to nearest
cnt = (div <= 15) ? div - 1 : 15                    // max divide-by-16
pol = (ClockPolarity <= 1) ? 1 - SensorClockPolarity : 0        // (+0x0d)
padPclkInv = <ImgSensorDrv::sendCommand(dev, 0x2002) result>    // from the sensor driver
setTg1PhaseCounter(1 /*pcEn*/, mux, cnt, pol, SensorClockFallingCount, SensorClockRisingCount, padPclkInv)
```

Note `SENSOR_CLOCK_POLARITY_HIGH = 0` and `SENSOR_CLOCK_POLARITY_LOW = 1` in the stock `kd_imgsensor_define.h`, so
the stock sensors (polarity LOW) give `clkPol = 0`.

Sensor probing uses a fixed call, `SensorHalImp::searchSensor` (0x26498): `setTg1PhaseCounter(1, 1, 1, 1, 1, 0, 0)`,
i.e. 48 MHz / 2 = 24 MHz with CLKPOL = 1 (0xb0000001 style value below). [C]

### 4.2 Top-level mux (CLK_CFG_1)

`setTg1PhaseCounter` ioctl `ISP_SENSOR_FREQ_CTRL` = `_IOW('k', 14, unsigned long)` = **0x40046b0e** with value 1 or 2.
Kernel (`camera_isp.c:4420`): `clkmux_sel(MT_MUX_CAMTG, senFreq, "CAMERA_SENSOR")`. [C][K]

`MT_MUX_CAMTG` (`mt_clkmgr.c:2113`): `CLK_CFG_1` = PA 0x10000050, select mask 0x07000000 (bits 26:24), power-down
bit 0x80000000 (bit 31), 7 inputs, PLL parent UNIVPLL (must be enabled). `clkmux_sel` writes only the select field;
`enable_mux` clears the pdn bit (done by the IMG `SEN_TG` / `SEN_CAM` clock gates in the stock clock manager). [K]

| `CLK_CFG_1[26:24]` | Source (stock `mt_clkmgr.c:4934`) | Frequency |
|---|---|---|
| 0 | clk26m | 26 MHz |
| **1** | **univpll_d26** | **48 MHz** (UNIVPLL 1248 MHz / 26) |
| 2 | univpll2_d2 | 208 MHz (UNIVPLL / 3 / 2) |
| 3 | syspll3_d2 | |
| 4 | syspll3_d4 | |
| 5 | msdcpll_d2 | |
| 6 | mmpll_d2 | |

The same mux, register offset, shift, width and gate bit appear in mainline `clk-mt2701.c`
(`MUX_GATE(CLK_TOP_CAMTG_SEL, "camtg_sel", camtg_parents, 0x0050, 24, 3, 31)`, parents identical), which is the
most likely template for a mainline MT8127 top clock driver. [K]

### 4.3 TG phase counter for 24 MHz

Source 48 MHz: `f % 48 == 0` (24000 % 48 = 0), `mux = 1`, `div = (48000 + 12000) / 24000 = 2`, `cnt = 1`,
`fall = 1`:

```
rmw(0x10000050, clr 0x87000000, set 0x01000000)     // camtg mux = 1, pdn = 0
rmw(0x15008000, 0, 0x400)                            // SENINF_TOP_CTRL bit10
wr (0x15008304, 0x00010001)                          // CLKFL=1, CLKRS=0, CLKCNT=1
v = rd(0x15008300) & ~0xb0000047
wr (0x15008300, v | 0xa0000001 | (clkPol << 28))     // PCEN=1, ADCLK_EN=1, bit0, bit2 = 0 (cnt odd)
                                                     // clkPol=0 (stock sensors: polarity LOW) -> 0xa0000001
                                                     // clkPol=1 (probe)                        -> 0xb0000001
rmw(0x15004410, 0, 0x1)                              // CAM_TG_SEN_MODE.bit0
usleep(1000)
```

CMMCLK pad: GPIO 119 function **1** (`CMMCLK` in `pinctrl-mtk-mt8127.h`); stock `init()` writes
`gpio+0x770 = (v & 0xfff) | 0x1000`, i.e. field 4 (bits 14:12) = 1 for pin 119. [C][K]

Other frequencies produced by the same rule: 12 MHz -> mux 1, cnt 3 (CLKFL 2); 13 MHz and 26 MHz -> mux 2,
div 16 -> cnt 15 / div 8 -> cnt 7 (CLKFL 8 / 4) (13 MHz is 208/16); 48 MHz -> mux 1, div 1 -> cnt 0 (CLKFL 1).

Per-sensor values in the sources: gc0310 `SensorClockFreq = 24`, `ClockDividCount 3`, `RisingCount 0`, `FallingCount 2`,
`PixelClockCount 3`. Of these only `SensorClockFreq` and `SensorClockPolarity` affect the registers; rising/falling
counts are logged and ignored. [C]

### 4.4 Gate and enable ordering around MCLK

1. IMG clock gates for SEN_TG / SEN_CAM (and CAM_CAM, CAM_SMI, LARB2): `IMG_CG_CLR` = PA 0x15000008, bits 0, 5, 6, 7,
   8 (stock `MT_CG_IMAGE_*` masks in `mt_clkmgr.c:3246-3280`; IMG_CG_CON 0x15000000, SET 0x15000004, CLR 0x15000008).
2. CAMTG mux selected and ungated (above).
3. Sensor rails/reset sequencing is board code (`kd_camera_hw.c`); stock calls `ISP_MCLK1_EN(1)` (bit29 of
   0x15008300) at the right point of the power-up sequence and `ISP_MCLK1_EN(0)` on power-down.

### 4.5 GPIO details used by the interface

GPIO block PA 0x10005000. Mode registers: `0x600 + 0x10*(pin/5)`, field `(pin%5)*3`. [K]

| Reg | Pins covered | Bits written by libcamdrv |
|---|---|---|
| 0x740 | 100..104 | field1..4 = 1 for CSI0 enable |
| 0x750 | 105..109 | all = 1 (CSI0) ; fields 2..4 = 2 (disable) |
| 0x760 | 110..114 | field0 = 1 (CSI0) / fields 1..4 = 1 (CSI1) / all = 2 (disable) |
| 0x770 | 115..119 | fields 0,1 = 1 (CSI1) / 2 (disable) ; field 4 = 1 (CMMCLK) always |
| 0x780 | 120..124 | field 0 = 1 (CMPCLK) in disable path |
| 0x910 | IES for pins 117..120 [K] | bit7 cleared on enable, set on disable |
| 0xb60 | drive strength for 117..120 | bits 15:12 (`setTg1IODrivingCurrent`) |

---

## 5. TG / sensor-interface routing (summary of section 2.7 to 2.11)

- Only SENINF1 and TG1 are used; no mux/route register is programmed, so the SENINF1 -> TG1 connection is the
  hardware default. [C]
- `SENINF1_CTRL` (0x15008010): [31] EN, [30:28] pad select, [27:16] constant 0x6df, [15:12] source select
  (8 at init, 0 for MIPI after `setTg1CSI2`), [10:9] sync polarity bits from `setTg1SensorModeCfg`, [8] JPEG,
  [1:0] reset pulse. [C]
- `CAM_TG_SEN_MODE` (0x15004410): bit0 (always set), bit2 (set by `SensorModeCfg`), bit8 (set for src != 8, i.e. RAW
  types), bit1 (JPEG only), bits 9:8 cleared when src == 8. [C]
- `CAM_TG_PATH_CFG` (0x15004420): bits 1:0 always cleared (no input LSB shift), bit4 set only for JPEG. [C]
- Format select: `0x15004098/0x1500409c` bits [18:16] = TG format (1 = RAW10, 0 = RAW8, 3 = YUV422, 5/6/7 =
  RGB565/RGB888/JPEG), bit 24 = JPEG. [C] meaning of format codes [I]
- Grab window: `0x15004418`/`0x1500441c`, 15-bit start/end for pixels (bytes for YUV) and lines. [C]
- View-finder mode: `0x15004414`. VF enable is bit0 of the same register (set by the CAM pipe, outside this class;
  kernel tests it as "VF enabled"). [C][K]
- Pixel mode: no register for 1/2 pixel mode is touched by libcamdrv; the TG/CAM defaults apply. [C]

---

## 6. Reset, clear, interrupt and status

| What | Register | Notes |
|---|---|---|
| SENINF1 soft reset | `sen+0x8010[1:0]` = 3 for 10 us (`resetSeninf`); also pulsed at the end of `setTg1CSI2` | [C] |
| CSI2 soft reset | `sen+0x8100[12]` pulse (`resetCSI2`), preceded by a write/read/OR-0x10 on `sen+0x810c` | [C] |
| SENINF1 INTEN / INTSTA | `0x15008014` / `0x15008018` | stock kernel dumps both as "SENINF1_INT_STA"; bit0 = FIFO overrun/underrun [K] (`camera_isp.c:3991`: on `TG1_ERR` it checks `INTSTA & 1`). Other bits not decoded in either source. |
| Polling for SENINF events | `waitSeninf1Irq(mask)` polls `INTSTA & mask`, 40 x 100 ms | [C] |
| Detected size | `0x1500802c`: `[31:16]` and `[15:0]` compared with expected size values | [C]/[I] |
| CSI2 status | `0x1500810c`, `0x15008110..` (dumped by the stock error path, `0x8024` and `0x802c` too) | content undecoded |
| CSI-2 error flags (deskew) | `0x15008618 & 0xfb8` (bits 3,4,5,7,8,9,10,11), clear with `wr(0x15008618, 0x7fff)`; enable word 0x80007fff at `0x15008614` | [C]; belongs to the NCSI2-style block at 0x8600 [I] |
| MIPI RX lock/ready | `rx+0x44 & 0x10101`, `rx+0x48 & 0x101` after the calibration pulse | [C] |
| ISP IRQ (SPI 143) | `CAM_CTL_INT_STATUS` = 0x15004024 (read clears, kernel comment); 0x15004020 is dumped next to it | bits [K]: 0 VS1, 1 TG1_ST1, 2 TG1_ST2, 3 EXPDON1, **4 TG1_ERR**, 10 PASS1_TG1_DON, **12 SOF1_INT**, 13 CQ_ERR, 14 PASS2_DON, 20 IMGO_ERR, 23 IMG2O_ERR, 30 DMA_ERR (`camera_isp.h:15-45`) |
| Frame start / done | SOF1 (bit 12) and PASS1_TG1_DON (bit 10) of 0x15004024 | [K] |
| Overrun diagnosis | on TG1_ERR the kernel reads 0x15008018, 0x15004414 bit0, 0x15008014, 0x15004004/08/0c | [K] |

---

## 7. Minimal bring-up recipe (IMG clock gates and ISP power domain already on)

Legend: **[C]** copied from `SensorHalImp::init`/`setConf` order and the functions above, **[I]** my choice
or interpretation.

1. Pin mux: GPIO119 CMMCLK function 1; set the MIPI pins of the chosen port to function 1 (step 5 does it). [C]
2. MCLK source and divider (24 MHz): CAMTG mux = 1 and ungated; `0x15008000 |= 0x400`; `0x15008304 = 0x00010001`;
   `0x15008300 = (v & ~0xb0000047) | 0xa0000001` (or `0xb0000001`); `0x15004410 |= 1`; wait 1 ms. [C]
3. Sensor power rails, reset, MCLK gate on (`0x15008300 |= 0x20000000`) as in the board power sequence; I2C probe of
   the sensor. In the stock flow the ID probe runs in `searchSensor` (24 MHz via `setTg1PhaseCounter(1,1,1,1,1,0,0)`),
   and `initSensor` runs before `setTgPhase` in `SensorHalImp::init`; how the clock is present during `initSensor`
   (the divider write comes after it, and `Seninf::uninit` clears PCEN) was not traced (see 9). [C/I]
4. `SENINF1_CTRL[15:12] = 8` (reset value after `init()`), optional eFuse trim of `ana+0x24` (2.1). [C]
5. PHY on, `initTg1CSI2(1)` (section 2.6): for CSI0 or CSI1, includes the pin mux, the analog sequence, the lane-map
   byte, the `rx+0x38/3c` pulse and `sen+0x8128`. [C]
6. CSI-2 digital: `0x15008104 = settle << 16` (14), `0x15008108 = rd(0x1500810c) | 0x10`,
   `0x15008100 = 0x411 | ECC_order<<5 | vsyncType<<13 | ((1<<lanes_enum)-1)<<1`
   (1-lane 0x2431, 2-lane 0x2433 for the stock gc0310-style parameters), then `SENINF1_CTRL`: clear `[15:12]`,
   bit10 and bits 1:0 with a 3-then-0 pulse on bits 1:0. [C]
7. Open the sensor driver (I2C init, still not streaming). [C: `SensorDrv::open` is the last step of `init`]
8. Scenario (`setConf`): `resetSeninf` (0x15008010 |= 3, 10 us, &= ~3); grab window (0x15004418/0x1500441c);
   `setTg1SensorModeCfg` (SENINF1_CTRL bits 10/9, `0x15004410 |= 5`); `setTg1InputCfg`
   (SENINF1_CTRL EN + pad + 0x6df<<16; TG format in `0x15004098/0x1500409c`; `0x15004410`/`0x15004420` bits);
   `setTg1ViewFinderMode`. [C]
9. Repeat step 6 (`setCSI2Config(1)` runs again at the end of `setConf` and clears `SENINF1_CTRL[15:12]` and bit 10
   which `setTg1InputCfg` had set). [C]
10. Start the sensor stream (I2C), then enable the TG view finder: `0x15004414 |= 1` (VFDATA_EN) together with the
    ISP pass-1 DMA/enable programming (outside this document). Check `0x15004024` for SOF1 (bit12) / PASS1_DONE (bit10)
    and for TG1_ERR (bit4); on error read `0x15008018` (bit0 overrun), `0x1500810c`, `0x1500802c`. [I for the VF bit being
    the start trigger: the library class only configures VF mode; the kernel treats bit0 as VF enable]

Items that are certain: 2, 4, 5, 6, 8, 9 as register sequences; the order 5 before 6 before 8 before 9.
Items inferred: the meaning of the bits; the precise point where VF is enabled; that no further SENINF mux
register is needed.

---

## 8. Cross-check against other MediaTek sources on this machine

- `Kobo/kernel/linux/v4.9/drivers/media/platform/soc_camera/mtk_mipicsi0.c` (newer SENINF with NCSI2 at 0xA0+, MT2712-style):
  - MIPI RX analog init uses the same register offsets (CON00/04/08/0c/10 at 0x00..0x10, 0x20, 0x24, 0x4c, 0x50) and
    the same mask `0xFEFBEFBE` on 0x4c/0x50 before enabling, then ORs bit3 into the five lane CON registers, then
    bit0, with a 1 ms delay. This agrees with the shape of `initTg1CSI2(1)`: mask 0x4c, lane CONs |= 8, then |= 1.
    Differences: MT8127 ORs 0x01041041 into 0x4c when disabling and ORs 0x3c0 into 0x48; Kobo ORs 0x11 into 0x24 and
    0xFF030003 into 0x20, MT8127 ORs 3 and 1.
  - MIPI RX config `CON24 = 0xE4000000` (lane map), `CON38 = 1 -> 5 -> 4`, `CON3C = 0x51545`: MT8127 uses `CON24` top
    byte 0xe4/0x1b, `CON38 |= 1`, `CON3C = 0x1541`, `CON38 |= 4`, then `&= ~1`. Same registers, slightly different
    values.
  - The NCSI2 block (CTL 0xA0, TIMING 0xA4/A8, INT_EN 0xB0, INT_STATUS 0xB4, DBG_SEL 0xB8, DBG_PORT 0xBC) matches the
    four registers MT8127 touches at 0x15008614..0x15008620 in the deskew routine (INT_EN 0x80007fff vs 0x7fffffff,
    status clear 0x7fff in both, DBG_SEL, DBG_PORT). The MT8127's CSI2 control (0x8100..0x8128) is the older style
    (`SENINF1_CSI2_CTRL/DELAY/INTEN/INTSTA/.../LNMUX`), not the NCSI2 layout of the Kobo driver.
  - `SENINF_TOP_CTRL = 0x00010C00`, `SENINF1_CTRL = 0x8001` etc. in Kobo use the newer register map; no direct
    equivalent in MT8127.
- `Kobo/.../mediatek/cameraisp/src/mt8518` and `imgsensor/src/mt8518`: new-generation drivers, no matching
  `SENINF1_CTRL` / `TG1_PH_CNT` definitions found; not applicable.
- No MT6582/MT6589 `seninf_reg.h` or `mipi_rx_reg.h` was found on this machine. Register names in this
  document that are not from the stock kernel are therefore recalled from the MT6589-era naming convention
  and flagged [I].
- Mainline `clk-mt2701.c` confirms the CAMTG mux layout (0x0050, shift 24, width 3, gate bit 31, parent order).
- Mainline `pinctrl-mtk-mt8127.h` / `.c` confirms pin names/functions, the 0x910 bit 7 IES group and the 0xb60 drive group.

---

## 9. Open questions and things not determined

1. **Port per sensor on the Tab3**: the library maps main -> CSI0 and sub -> CSI1 by device mask, but the board
   wiring of rear/front sensors (gc2355/sp2508 vs gc0310/sp0a20) and their lane counts is not known from the library;
   only gc0310 has `SENSOR_MIPI_1_LANE` visible in the kernel source (the other sensor drivers' info structures
   live in userspace libs `libcameracustom.so`/`libacdk.so` and were not decoded here).
2. **Exact bit names** of `SENINF1_CTRL`, `CSI2_CTRL`, `TG1_PH_CNT`, the `ana+0x20/0x24/0x48/0x4c/0x50` bits and
   `rx+0x24/0x38/0x3c/0x44/0x48`: offsets and values are exact, meanings are inferred.
3. **0x8108 / 0x810c**: `setTg1CSI2` writes `rd(0x810c) | 0x10` into 0x8108 (after an `|= 7` that is overwritten), and
   `resetCSI2` writes 0 then `|0x10` into 0x810c. Likely a stock bug, so I cannot say with certainty which of the
   two is the CSI2 interrupt-enable and which the status register. The stock kernel only reads 0x810c.
4. **SENINF1_INTSTA bits** other than bit0 (overrun) are undecoded; INTEN (0x8014) reset value and whether INTSTA is
   write-1-to-clear are unknown.
5. **Term delay fields** (`dataTermDelay`, `clkTermDelay`): not programmed by this library version. Whether the hardware
   has separate registers for them (and whether the default is correct) is unknown.
6. **`SENINF_TOP_CTRL` bit 10** (`0x15008000 |= 0x400`, set by `setTg1PhaseCounter`): effect assumed to be the TG1 clock
   enable, unverified. Its reset value is unknown.
7. **VF start**: the exact point where `CAM_TG_VF_CON[0]` is set lives in the CAM pipe (`libcam.campipe.so` / `IspDrv`),
   not decoded here, nor the CAM_CTL_EN registers that route TG -> ISP.
8. **eFuse trim**: `ana+0x24[15:12]` and `[11:8]` are adjusted from `efuse[0x170]` bits 30:27 and 26:23 only once per
   process. Whether the hardware works without it is untested.
9. **Which IMG gates and power domain are required**: only the gates named by the stock clock manager are listed
   (bits 0, 5, 6, 7, 8 of `IMG_CG_CON`); no register read/write in libcamdrv touches them.
10. The `0x4098/0x409c` pair is treated as SET/CLR companions because the stock code clears with one and sets with
    the other; the register's official name and the exact meaning of its fields were not verified.
11. `autoDeskewCalibration` is documented from the disassembly only; since it is never called there is no evidence of
    when (or whether) it would be needed. The stock sensors work without it.
12. Clock presence during `initSensor`: `SensorHalImp::init` calls `initSensor` before `setTgPhase`, and
    `searchSensor` ends with `SeninfDrv::uninit` (which zeroes PCEN). I did not find where MCLK is generated for the
    first I2C accesses of `initSensor`; for a mainline driver the safe order is mux + phase counter first, then
    sensor power/reset release.
13. Parallel (CMOS/CCIR656) sensors: `setCCIR656Cfg` registers (0x8400/0x8404) and the SENINF1 source-select values other
    than 0 and 8 are out of scope and not verified.

---

## Appendix A: literal constants read from `libcamdrv.so`

| Address | Value | Used by |
|---|---|---|
| 0x133a0 | 0x1500c000 | `init`: MIPI RX config window |
| 0x133a4 | 0x10010000 | `init`: MIPI config page (analog at +0x800) |
| 0x133a8 | 0x10005000 | `init`: GPIO |
| 0x133ac | 0x10206000 | `init`: eFuse |
| 0x124fc | 0x01041041 | `initTg1CSI2(0)`: OR into `ana+0x4c` |
| 0x12500 | 0xfefbefbe | `initTg1CSI2(1)`: AND on `ana+0x4c` |
| 0x12978 | 0x80007fff | `autoDeskewCalibration` write to 0x8614 |
| 0x12d40 | 0x40046b0e | `ISP_SENSOR_FREQ_CTRL` ioctl |
| 0x12e0c | 0x40016b05 | hold-register ioctl |
| 0x14e04 | 0x0001898d | `setTgPhase` range limit (100749) |
| 0x14e08 | 0x00032c80 | 208000, numerator for the 208 MHz source |
| 0x122ec | 0x000186a0 | `waitSeninf1Irq` poll interval (100 ms) |

## Appendix B: key call order in the stock flow

`SensorHalImp::init` (0x2605c): `SeninfDrv::init` -> `initSensor` (sensor power, probe) -> `getSensorInfo` ->
`setTgPhase` (-> `setTg1PhaseCounter`) -> `setSensorIODrivingCurrent` -> `initCSI2Peripheral(1)` (-> `initTg1CSI2(1)`)
-> `setCSI2Config(1)` (-> `setTg1CSI2`) -> sensor `open()`.

`SensorHalImp::setConf` (0x25560): `getSensorInfo` -> `setScenario` -> `resetSeninf` -> per sensor:
`setTg1GrabRange`, `setTg1SensorModeCfg`, `setTg1InputCfg`, `setTg1ViewFinderMode` -> `setCSI2Config(0)` (no-op) ->
`setCSI2Config(1)`.

`SensorHalImp::reset` (0x14478): `checkSeninf1Input` -> sensor `close` -> 100 us -> `resetCSI2` -> sensor `open` -> 10 us
-> `setScenario`.

`SensorHalImp::uninit` (0x25274): `initCSI2Peripheral(0)` (-> `initTg1CSI2(0)`) -> sensor `uninit` -> `SeninfDrv::uninit`.

`SensorHalImp::searchSensor` (0x26498): `SeninfDrv::init` -> `setTg1PhaseCounter(1,1,1,1,1,0,0)` -> `SensorDrv::searchSensor`
-> `SeninfDrv::uninit`.
