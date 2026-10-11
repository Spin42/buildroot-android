# MT8127 ISP pass-1 capture: TG to IMGO (RAW) and YUV pass-through

Register-level notes recovered from the stock Android 5.0.1 camera stack of the Lenovo TB3-710F, for the mainline
V4L2 capture driver. Scope: everything after the sensor interface (SENINF / MIPI RX / TG clock are covered
by the sibling document). The TG registers that live inside the ISP window (+0x410..) are included because the
ISP-side pass-1 programming cannot be separated from them.

## 0. Conventions, sources, confidence

* All register offsets are **relative to the ISP window base 0x15004000** unless written `vendor 0x4xxx`.
  The vendor userspace maps the whole IMGSYS block (physical 0x15000000, 0x10000 bytes, `IspDrvImp::init`
  libcamdrv 0xc9c4) and addresses the ISP registers as `base + 0x4000 + off`; the doc offset is `vendor - 0x4000`.
  Example: vendor 0x4414 is ISP+0x414 (TG_VF_CON).
* Confidence tags: **[C]** certain, read directly from code or a literal constant; **[I]** inferred from code
  structure, naming or a second source; **[?]** unresolved.
* Library function addresses are ELF virtual addresses of the stock binaries (`r2` addresses). The Ghidra
  decompilations kept in the scratch directory use the same addresses plus 0x10000.
* **Provenance note.** The pass-1 programming is not in the five libraries named in the task. `libcamdrv.so`
  only holds `IspDrv` (register access, command queue) and the SENINF/TG setters; `libcam.campipe.so` is a thin
  wrapper. The real register programming lives in `libimageio.so` (`CamIOPipe`, `PipeImp`) and
  `libimageio_plat_drv.so` (`ISP_TOP_CTRL`, `DMAO_B`, `CamPath_B`, `CamPathPass1`, `ISP_BUF_CTRL`), which are
  **not** in `dump/bootloader_analysis/system_extract/lib/` but are in the firmware image
  `Lenovo_TB3-710F_MT8127_S000024_160506_5.0.1/Firmware/system.img` (`/lib/libimageio.so`, 91364 bytes;
  `/lib/libimageio_plat_drv.so`, 239496 bytes; also `libdpframework.so`, `libm4u.so`). `libcamdrv.so` and
  `libcam.campipe.so` in the dump are byte-identical to the image copies where checked (`libcamdrv.so`).
  These two libraries are the main source below.
* Stock kernel sources: `vendor-kernel/szazs89-tb3/arch/arm/mach-mt8127/camera_isp.c` (cited by line number
  of the file as stored, CRLF counted as one line) and `include/mach/camera_isp.h`.
* Cross-check against other trees: no MT6582/MT6589 `isp_reg.h` exists on this machine. The only
  `CAM_CTL_*`/`TG_VF_CON` register headers found (`Kobo/kernel/.../cameraisp/src/mt8518/camera_isp.c`) belong to a
  different ISP generation (SW_CTL at +0x40, TG_VF_CON at +0x504, IMGO_BASE_ADDR at +0x220) and do **not** apply.
  Register names below that are not in the stock kernel come from the vendor log strings and from the structure of
  the code; names that I only remember from MT6589 headers are marked [I].

## 1. Access model

1. The ISP is a pure MMIO block, IRQ GIC SPI 143 (INTID 175, `CAMERA_ISP_IRQ0_ID = 32 + 143`), level, active low
   (`request_irq(..., IRQF_TRIGGER_LOW)`, camera_isp.c:4933, polarity set at :5058) [C]. SENINF has its own IRQ
   (INTID 174, SPI 142) [C].
2. Userspace writes registers directly through the mmap; the kernel driver exposes only `ISP_WRITE_REGISTER`
   (with an optional "hold until VD" queue), IRQ wait/read/clear, a real-time buffer ring, and reset [C].
   The kernel never programs the capture path itself.
3. **SET/CLR register pairs.** The enable/select registers have write-only helper registers
   (vendor-confirmed in `gIspCQModuleInfo`, libcamdrv .data 0x53160, module 1 = 0x4080 x10 regs, module 2 =
   0x40C0 x8 regs): writing 1-bits to a SET register sets those bits in the base register, writing 1-bits to the
   CLR register clears them [I: inferred from how `ISP_TOP_CTRL::_config` computes the CLR operand as
   "mask & ~wanted"]. The base registers are also directly writable (the vendor does so once, see 3.2).
4. **Two vendor programming modes.** Every `_config()` has a "direct" branch (`IspDrv::IsReadOnlyMode() == 0`,
   writes to the mmap) and a "virtual" branch (writes to a DRAM register image that the hardware command queue
   (CQ) copies into the ISP). `CamIOPipe` uses the direct branch when the CQ channel (`CamIOPipe+0x10d4`,
   set with `sendCommand(0x1103, cq)`) is -1, which is the constructor default (libimageio 0xb348).
   **Caveat:** the stock camera HAL does *not* run the path configuration in direct mode. `NSCamPipe::CamIOPipe::configPipe`
   (libcam.campipe 0x67c0) sends `0x1103 = 1` (CQ channel 1, plus `0x1104` trigger mode) before configuring, so the
   path registers of 5.2 reach the ISP through the command queue; `NSCamPipe::CamIOPipe::start` (0x5be8) then sends
   `0x1103 = -1` and the enable steps of section 8 (FBC init, VF enable) run direct. libacdk does the same
   (CQ 1 or 4 during config, -1 at start, libacdk `AcdkMhalPure`). The direct branches of `_config()` exist and compute
   identical values, so programming the registers directly is expected to work, but that exact combination (direct
   config of the pass-1 path) is not exercised by the stock flows [I].
5. INT_STATUS (+0x024) and the other status registers are **read-to-clear**: the stock IRQ handler only reads
   them (camera_isp.c:3939-3960 never writes a clear) and the dump code calls the registers at +0x24/+0x28 "read
   clear" (:628-633) [C].

## 2. Register map (ISP-relative)

| Offset | Name | Notes | Conf. |
|---|---|---|---|
| +0x000 | CTL_START | pass-2 CQ trigger (written 0x08/0x20/0x40 by `ISP_TOP_CTRL::_enable` for the CQ cases); unused in pass 1 | I |
| +0x004 | CTL_EN1 | main block enables; bit0 TG1_EN, bit1 TG2_EN, bit9 HRZ_EN (see 5.3) | C/I |
| +0x008 | CTL_EN2 | more enables | C |
| +0x00C | CTL_DMA_EN | bit0 IMGO_EN, bit10 IMG2O_EN, bit7 IMGI_EN | C/I |
| +0x010 | CTL_FMT_SEL | [2:0] SCENARIO, [6:4] SUB_MODE, [15:12] pack code, [18:16] TG1_FMT, bit24 TG1 JPEG | C/I |
| +0x018 | CTL_SEL | CQ trigger source/mode bits (`IspDrv::setCQTriggerMode`, libcamdrv 0xabf0) and other selects; set via SET at +0x0A0 | I |
| +0x020 | CTL_INT_EN | enable for INT_STATUS | C |
| +0x024 | CTL_INT_STATUS | read-clear status | C |
| +0x028 | CTL_DMA_INT | read-clear (kernel view); also written |= by pass-1 config, see 7.3 | C/? |
| +0x02C | CTL_INTB_EN | second IRQ domain (pass-2 CQ), written by config | I |
| +0x030/+0x034 | CTL_INTB_STATUS / DMAB_INT | read-clear | C |
| +0x038 | CTL_INTC_EN | third domain | I |
| +0x03C/+0x040 | CTL_INTC_STATUS / DMAC_INT | read-clear | C |
| +0x044/+0x048 | CTL_INT_STATUSX / DMA_INTX | union view | C |
| +0x050/+0x054 | TCM_? | written only by the pass-2 path (0x7A03001F / tcm_en) | C |
| +0x05C | CTL_SW_CTL | reset register, see 3.4 | C |
| +0x06C | (unnamed) | pass-1 config writes 0x80 | C value, ? meaning |
| +0x080/+0x084 | EN1_SET / EN1_CLR | | I |
| +0x088/+0x08C | EN2_SET / EN2_CLR | | I |
| +0x090/+0x094 | DMA_EN_SET / CLR | | I |
| +0x098/+0x09C | FMT_SEL_SET / CLR | | I |
| +0x0A0/+0x0A4 | CTL_SEL_SET / CLR | | I |
| +0x0BC | CQ0C_BASE_ADDR | PA of the CQ0C descriptor ring (real-time buffer ring) | C (name from kernel macro `ISP_REG_ADDR_CQ0C_BASE_ARRR`) |
| +0x0C0/+0x0C4 | MUX_SEL_SET / CLR | | I |
| +0x0C8/+0x0CC | MUX_SEL2_SET / CLR | | I |
| +0x0D0/+0x0D4 | MUX_CONF_SET / CLR | | I |
| +0x0D8/+0x0DC | PIX_ID SET / CLR (bits [1:0] Bayer start colour; SET also gets bit 20) | | I |
| +0x0F4 | IMGO_FBC | frame-buffer controller, see 6.3 | C |
| +0x0F8 | IMG2O_FBC | | C |
| +0x0FC | FBC interrupt status | read by IRQ handler (`IrqStatus_fbc_int`) | I |
| +0x150 | (unnamed) | pass-1 config writes 0x0001FFFF (twice) | C value, ? meaning |
| +0x160/+0x164 | CTL_DBG_SET / CTL_DBG_PORT | debug mux; vendor sets 0x6000 (CQ status) | C |
| +0x280/+0x284 | DMA-side regs | written 0x08000010 / 0 once | C value, ? meaning |
| +0x300 | IMGO_BASE_ADDR | | C |
| +0x304 | IMGO_OFST_ADDR | | I |
| +0x308 | IMGO_XSIZE | bytes per line - 1 | C |
| +0x30C | IMGO_YSIZE | lines - 1 | C |
| +0x310 | IMGO_STRIDE | bytes | C |
| +0x314 | IMGO_CON | | C |
| +0x318 | IMGO_CON2 | | C |
| +0x31C | IMGO_CROP | x \| y<<16 | C |
| +0x320..+0x33C | IMG2O_* | same layout | C |
| +0x380/+0x384, +0x39C/+0x3A0 | DMA-side regs | written 0x04000008/0, 0x08000020/0 once | C value, ? meaning |
| +0x3A4 | DMA_ERR_CTRL | bit per DMA (IMGO = bit9); per-DMA status at +0x3A8.. (IMGO at +0x3C4) | C (camera_isp.c:3884-3925) |
| +0x410 | TG_SEN_MODE | | C |
| +0x414 | TG_VF_CON | | C |
| +0x418 | TG_SEN_GRAB_PXL | | C |
| +0x41C | TG_SEN_GRAB_LIN | | C |
| +0x420 | TG_PATH_CFG | | C |
| +0x44C | TG_INTER_ST | state, field [13:8] (1 = idle) | I |
| +0x4F4 | (TG group tail) | pass-1 config writes 0 | C |
| +0xF50 | HRZ_RES | 0x800 default, written by `ISP_RAW_PIPE::_config` | C |
| +0x1600 | (CQ module 39) | written 0 by `CamIOPipe::init` | C |

The command-queue module table `gIspCQModuleInfo` (libcamdrv file offset 0x53160, triples id/offset/count,
vendor offsets) gives the authoritative register groups: IMGO = 0x4304 x7 (module 6), IMG2O = 0x4324 x7 (7),
TG = 0x4410 x30 (module 12, i.e. TG occupies +0x410..+0x484), HRZ = 0x4F50 x1 (35), set/clear block
0x4080 x10 and 0x40C0 x8. IMGO_BASE_ADDR (0x4300) is deliberately outside the IMGO group: it is owned by the
buffer ring [C].

## 3. Power-up, reset, one-time initialisation

### 3.1 Clocks and power (kernel `ISP_EnableClock`, camera_isp.c:1616) [C]

Order: MMSYS CG0 SMI_COMMON, IMG CG CAM_SMI, CAM_CAM, SEN_TG, SEN_CAM, LARB2_SMI (bits 5, 6, 7, 8, 0 of the
IMG gate register at 0x15000000). The ISP power domain (SPM +0x238) must already be on; the clock enable
helper implies it on the stock kernel. After enabling, the kernel writes `ISP+0x160 = 0x6000` when it is not already
0x6000 (CQ status debug select); `ISP_TOP_CTRL::_enable` writes the same value again (libimageio_plat_drv 0x12964)
[C]. The IMG_SW_RST register (IMG+0x0C: bits 17:16 SENINF, 14:12 CAM fsmi/fmem/faxi) is only referenced in a
commented-out block (camera_isp.c:1683-1700) and never used [C].

### 3.2 Reset (`ISP_Reset`, camera_isp.c:1669) [C]

```
EMI[0x120] |= 0x3f;            // physical 0x10203120, "bandwidth limitor for TG"
ISP[0x05C] = 0x1;              // SW_RST_TRIG
do { r = ISP[0x05C]; } while ((!r) & 0x2);   // as written this never waits (see below)
ISP[0x05C] = 0x5;              // TRIG | HW_RST
ISP[0x05C] = 0x4;              // HW_RST
ISP[0x05C] = 0x0;
```

Bits: 0 SW_RST_TRIG, 1 SW_RST_STATUS (read), 2 HW_RST. The stock wait loop is `while ((!Reg) & STATUS)`, which
evaluates to 0 on the first iteration, so the vendor never actually waits for bit1. A mainline driver should poll for
bit1 with a timeout, then run the 0x5, 0x4, 0x0 sequence. After reset every register is assumed to be at its power-on
value (zero for the control registers) [I].

Trigger points on the stock stack: `ISP_RESET` ioctl (0x6B00) issued by `IspDrvImp::init` when the "ISP function"
reference count was 0 (libcamdrv 0xc9c4, calls `IspDrvImp::reset`, 0xc034), and `ISP_REF_CNT_DEC_AND_RESET_IF_LAST_ONE`
when the last user goes away (camera_isp.c:2575) [C].

### 3.3 Defaults written by the pass-1 pipe at init (`CamIOPipe::init`, libimageio 0x9750) [C]

Direct branch, in this order:

```
ISP[0x004] = 0x40000000;                 // CTL_EN1
ISP[0x008] = ISP[0x008] & 0x04000000;    // CTL_EN2: keep bit 26 only
ISP[0x00C] = 0;                          // CTL_DMA_EN
ISP[0x1600] = 0;                         // CQ module 39
ISP[0x018] = 0;                          // CTL_SEL: CQ triggers off
```

then it clears the real-time buffer rings with `rtBufCtrl` (ctrl 5 = CLEAR) for IMGO (id 4) and IMG2O (id 5).
`CamIOPipe::configPipe` repeats the first four writes (EN1 = 0x40000000, EN2 = 0, DMA = 0, 0x1600 = 0) when
EScenarioID is 2 (preview) before configuring (libimageio 0xa270, top of function).

No other module (BNR, LSC, CFA, CDRZ, ...) is touched for a raw or YUV pass-1 dump: the RAW/RGB/YUV pipe
objects only run `ISP_RAW_PIPE::_config`, which writes the HRZ scale (`ISP+0xF50 = (ISP[0xF50] & ~0x3FFF) | 0x800`,
i.e. 1.0, when EN1 bit9 is off; libimageio_plat_drv 0x15da8). Module `_config()` calls are skipped when the module flag
is zero. So the pass-1 raw dump relies on the power-on defaults of everything else [I]. LSC/CFA/BNR bypass is therefore
the reset state; if a block is not bypassed after reset it is a driver bug to chase on hardware.

### 3.4 One-time writes inside `ISP_TOP_CTRL::_config` [C values, ? meaning]

Executed for every scenario before the pass-1 specific part (libimageio_plat_drv 0x19e38, label at 0x2a4c0 in the
Ghidra address space), direct branch:

```
ISP[0x020] |= INT_EN                       // see 7.1
ISP[0x02C] |= 0   ISP[0x038] |= 0          // INTB/INTC enables, not used in pass 1
ISP[0x028] |= DMA_INT_BITS                 // 0x04000000 (preview) / 0x04100000 (capture), see 7.3
ISP[0x034] |= 0   ISP[0x040] |= 0
ISP[0x06C]  = 0x00000080
ISP[0x150]  = 0x0001FFFF
ISP[0x4F4]  = 0
ISP[0x380]  = 0x04000008   ISP[0x384] = 0
ISP[0x39C]  = 0x08000020   ISP[0x3A0] = 0
ISP[0x280]  = 0x08000010   ISP[0x284] = 0
```

The last six lines look like DMA FIFO/priority thresholds; meaning unknown, written for fidelity.

## 4. TG registers inside the ISP window

Written by `SeninfDrvImp` (libcamdrv), called from `SensorHalImp::setConf` (0x15560) in the order
GrabRange, SensorModeCfg, InputCfg, ViewFinderMode, then (later, with the SENINF phase counter) CMOS enable.
The SENINF-side bits (0x15008010 etc.) belong to the sibling document and are only noted.

| Step | Function (libcamdrv) | ISP-window effect | Conf. |
|---|---|---|---|
| 1 | `setTg1GrabRange(xs,xe,ys,ye)` 0x11fd4 | `TG_SEN_GRAB_PXL (0x418) = (xs & 0x7fff) \| (xe & 0x7fff) << 16`; `TG_SEN_GRAB_LIN (0x41C) = (ys & 0x7fff) \| (ye & 0x7fff) << 16`. End is exclusive: `xe = xs + width`, `ye = ys + height` | C |
| 2 | `setTg1SensorModeCfg(a,b)` 0x12024 | `TG_SEN_MODE (0x410) \|= 0x5` (bit0 and bit2); SENINF 0x8010 bits 9/10 get a/b | C |
| 3 | `setTg1InputCfg(pad, src, fmt, lsb)` 0x12078 | see below | C |
| 4 | `setTg1ViewFinderMode(single, frcon)` 0x121bc | `TG_VF_CON (0x414) = (old & 0xFFFFE0FD) \| 0x1000 \| (frcon & 7) << 8 \| (single & 1) << 1` | C |
| 5 | `setTg1PhaseCounter(...)` 0x12c2c | SENINF phase counter, then `TG_SEN_MODE \|= 1` (CMOS_EN), `usleep(1000)` | C |

`setTg1InputCfg` ISP-window effects (r1 = pad, r2 = src, r3 = fmt):

```
TG_PATH_CFG (0x420)  &= ~0x3
FMT_SEL_CLR (0x09C)  |= 0x70000                       // clear TG1_FMT field
FMT_SEL_SET (0x098)  |= (fmt & 7) << 16               // TG1_FMT
if (src == 8) {                                       // 8 = MIPI CSI-2 source
    TG_SEN_MODE &= ~0x300
} else {                                              // parallel
    TG_SEN_MODE |=  0x100
}
if (fmt == 7) {                                       // JPEG
    TG_SEN_MODE |= 0x2;  FMT_SEL_CLR |= 0x1000000;  FMT_SEL_SET |= 0x1000000;  TG_PATH_CFG |= 0x10
} else {
    TG_SEN_MODE &= ~0x2; FMT_SEL_CLR |= 0x1000000;  FMT_SEL_SET &= ~0x1000000; TG_PATH_CFG &= ~0x10
}
```

Values the HAL passes (`SensorHalImp::setConf`): for the sensor type "RAW" (type 0) `fmt = 1` (not derived from the bit
depth), for YUV (types 1, 2) `fmt = 3` (YUV422), for RGB565/RGB888/JPEG `fmt = 5/6/7`; `src = 8` when the interface
is MIPI, 3 for the other interface values seen in the code; TG grab for YUV is **twice the pixel width**:
`xe = xs + 2 * width` (two TG samples per pixel), for RAW `xe = xs + width`. `xs, ys` are the sensor's grab-start
offsets plus centring. TG_FMT numbering: 0 RAW8, 1 RAW10, 2 RAW12, 3 YUV422, 5 RGB565, 6 RGB888, 7 JPEG [I] (the
5/6/7 values are what the HAL passes for those sensor types).

Resulting TG state for a MIPI RAW or YUV sensor (assuming zero after reset): `TG_SEN_MODE = 0x00000005`,
`TG_PATH_CFG = 0`, `TG_VF_CON = 0x00001000 | frcon << 8`, `FMT_SEL[18:16] = fmt`, `TG_SEN_GRAB_PXL/LIN` as above.
For a parallel sensor `TG_SEN_MODE = 0x00000105` [C by construction].

`TG_VF_CON` bit0 = VFDATA_EN (set at start, cleared at stop), bit1 = SINGLE_MODE (see 8), bits [10:8] frame-rate
control, bit12 always set by the vendor [C for the writes, I for the names].

## 5. Pass-1 path configuration (`ISP_TOP_CTRL::_config`)

### 5.1 How the vendor derives the register images

`CamIOPipe::configPipe` (libimageio 0xa270) fills a parameter block at `CamIOPipe+0xac0`; `CamPathPass1::config`
(0xed40) copies it into the module objects; `ISP_TOP_CTRL::_config` (0x19e38) then writes it. Inputs:

* `CamIOPipe+0x24` = EScenarioID, `CamIOPipe+0x28` = EScenarioFmt (`PipeImp::PipeImp`, 0x8ab8; the constructor
  stores the 3rd and 4th arguments of `ICamIOPipe::createInstance`, 0xbe90). EScenarioFmt is 0 = RAW,
  1 = YUV; anything else makes `configPipe` fail [C].
* The camera HAL (`libcam.campipe.so`, `NSCamPipe::mapScenarioID` 0x8e8c) requests EScenarioID **2** for the preview-class
  software scenarios 4 and 5 and **8** for all other software scenarios on the sensor-input pipe [C].
  `mapScenarioFmt` is the identity (0 RAW, 1 YUV).
* Port list: sensor input port (type byte 0) and IMGO output port (type 0xC); IMG2O output (0xD) is a second output
  port that is not needed for a dump.
* Pixel packing code (the `PB` below) per port format (`configPipe` 0xa9e6..0xaa28, `mapRawFormat` libcam.campipe 0x8f06,
  `mapYUVFormat` 0x8f20): BAYER8 (0x10C) PB=4, FMT_SEL nibble 0; BAYER10 (0x10D) PB=5, nibble 1; BAYER12 (0x10E)
  PB=6, nibble 2; YUV 4:2:2 packed (0x14 YUY2, 0x100..0x103) PB=8, nibble 2. PB is "bytes per 4 pixels" (see 6.1).

### 5.2 Final register images, direct mode, IMGO-only, width <= 2304

`SET` values go to the SET registers, `CLR` values to the CLR registers. After a reset only the SET values
matter; the CLR values scrub bits left over from a previous configuration and are listed because the vendor
writes them. Computed by `/var/home/marc/.claude/jobs/b31c3008/tmp/cam-isp/model.py`, which replays the
`configPipe` and `_config` arithmetic [C for the arithmetic, I for the replay of branches not run here].

EScenarioID 8 (the generic sensor-to-memory scenario) and 2 (preview):

| Register | RAW10, ID 8 | YUV422, ID 8 | RAW10, ID 2 | YUV422, ID 2 |
|---|---|---|---|---|
| EN1_SET (+0x080) | 0x40001001 | 0x08000001 | 0x40203001 | 0x28003001 |
| EN1_CLR (+0x084) | 0x00002000 | 0x00003000 | 0x00000200 | 0x00000000 |
| EN2_SET (+0x088) | 0x00000000 | 0x00000000 | 0x00000003 | 0x00000002 |
| EN2_CLR (+0x08C) | 0x00000020 | 0x00000020 | 0x00020020 | 0x00020020 |
| DMA_SET (+0x090) | 0x00000001 | 0x00000001 | 0x00000001 | 0x00000001 |
| DMA_CLR (+0x094) | 0x00000000 | 0x00000000 | 0x00000400 | 0x00000400 |
| FMT_SET (+0x098) | 0x00001006 | 0x00002016 | 0x00001002 | 0x00002012 |
| FMT_CLR (+0x09C) | 0x0000E071 | 0x0000D061 | 0x0000E075 | 0x0000D065 |
| CTL_SET (+0x0A0) | 0x00010050 | 0x00010050 | 0x10000050 | 0x10000050 |
| CTL_CLR (+0x0A4) | 0x00000100 | 0x00000100 | 0x00000100 | 0x00000100 |
| MUX_SET (+0x0C0) | 0x00000000 | 0x00400040 | 0x00100008 | 0x00100008 |
| MUX_CLR (+0x0C4) | 0xDFFE7FFF | 0xDFBE7FBF | 0xDFEE7FF7 | 0xDFEE7FF7 |
| MUX2_SET (+0x0C8) | 0xC028410C | 0xC0081108 | 0x40080104 | 0x40080104 |
| MUX2_CLR (+0x0CC) | 0x00901E70 | 0x00B00E74 | 0x00B01E78 | 0x00B01E78 |
| CONF_SET (+0x0D0) | 0x110054F7 | 0x110054F7 | 0x000054F7 | 0x000054F7 |
| CONF_CLR (+0x0D4) | 0xEE000000 | 0xEE000000 | 0xFF000000 | 0xFF000000 |
| DMA_INT bits (+0x028 \|=) | 0x04100000 | 0x04100000 | 0x04000000 | 0x04000000 |
| PIX_ID (+0x0D8 low 2 bits) | sensor Bayer start (0..3) | n/a (from sensor info) | same | same |
| Mode | continuous | continuous | continuous | continuous |

FMT_SEL = SCENARIO (the vendor stores 6 for ID 8, 2 for ID 2) | SUB_MODE (0 RAW, 1 YUV) << 4 |
nibble << 12, where nibble = 0/1/2 for RAW8/10/12 and 2 for YUV. RAW8 and RAW12 variants differ only in
this nibble: ID 8 RAW8 FMT_SET = 0x00000006, RAW12 = 0x00002006.
CLR values for ID 8: `EN1_CLR = 0x38203200 & ~EN1 & ~(bits whose module flag is set)`, etc., see the model.

If the SET registers turn out to be plain registers rather than set-only helpers, OR the values (for FMT_SEL combine the
TG1_FMT bits of section 4 with the low half above) instead of writing them one after another.

Mandatory vs cosmetic: with a clean reset the minimum that selects the data path is EN1/EN2/DMA/FMT/CTL/MUX/MUX2/CONF
SET values, PIX_ID, and the TG registers of section 4. The SINGLE_MODE bit (VF_CON bit1) is **overwritten** by the
top-level config: `TG_VF_CON = (TG_VF_CON & ~2) | ((mode == 0) << 1)` (libimageio_plat_drv 0x19e38, pass-1 branch,
store at 0x4414), where `mode` is 0 only for the single-capture scenarios (EScenarioID 0, 6, 7), 1 for 2 and 8 [C].

The scenarios 0/6/7 (single capture) set `EN1 = 0x1000`, `EN2 = 0`, `DMA = 1` and drop the TG enable bit; I could not
establish their data source (probably memory input) so they are not used here [?].

### 5.3 Bit meanings that are established

* EN1 bit0 = TG1_EN: `ISP_TOP_CTRL::_enable` sets `TG_VF_CON |= 1` iff EN1 bit0 is set, `_disable` clears it
  and waits for VS1 (status 0x1); EN1 bit1 = TG2_EN: waits status 0x20 (VS2) [C].
* EN1 bit9 (0x200) = HRZ_EN: `ISP_RAW_PIPE::_config` checks `this+0x18 & 0x200` and programs the HRZ resizer
  (`ISP+0xF50`) when set; the vendor only sets it when an IMG2O output is requested with width > 2304 [C].
* EN1 bit30 (RAW) and bit27 (YUV): the vendor sets bit30 for `EScenarioFmt == 0` and bit27 for 1 in every
  scenario; names unknown [C values].
* DMA_EN bit0 = IMGO_EN, bit10 = IMG2O_EN [C]: the FBC init in `_enable` tests `DMA_EN << 21 < 0` (bit10) before
  initialising the IMG2O FBC.
* FMT_SEL fields (confirmed by the tile driver's `top.scenario/top.mode/top.cam_in_fmt` and the 0xF077 clear mask):
  [2:0] SCENARIO, [6:4] SUB_MODE, [11:8] cam_in_fmt, [15:12] pack nibble above, [18:16] TG1_FMT, bit24 TG1 JPEG [I].
* The other EN1/EN2/DMA bit names (BNR, LSC, SL2, C24, CFA, ...) can be read from the pass-2 tile mapping
  (`CamPathPass2::configTpipeData`, libimageio_plat_drv 0xf968: EN1 bit5 UNP, bit8 BNR, bit21 SL2, bit29 LSC, ...) but the
  mapping contradicts some pass-1 usage (EN2 bits 0/1 are set for preview RAW), so treat it as low confidence [?].

## 6. IMGO DMA

### 6.1 Geometry registers (`DMAO_B::_config`, libimageio_plat_drv 0x117e4) [C]

With W, H in pixels, `stride_px` the line pitch in pixels (>= W), `PB` the packing code of 5.1:

```
IMGO_XSIZE  (+0x308) = ((((W * PB + 3) >> 2) + 1) & ~1) - 1      // bytes per line, rounded up to even, minus 1
IMGO_YSIZE  (+0x30C) = H - 1
IMGO_STRIDE (+0x310) = (stride_px * PB) >> 2                      // bytes
IMGO_CON    (+0x314) = 0x08100850
IMGO_CON2   (+0x318) = 0x00100800
IMGO_CROP   (+0x31C) = crop_x | (crop_y << 16)                    // 0 for a full-frame dump
IMGO_OFST_ADDR (+0x304): not written for IMGO (0)
IMGO_BASE_ADDR (+0x300): see 6.2
```

IMG2O uses the same formulas at +0x320.. with `CON = 0x08100840`, `CON2 = 0x00100800`, and `DMAO_B::_config` also
writes `IMG2O_BASE_ADDR` (+0x320) directly.

Worked examples (stride tight):

| Mode | W x H | PB | XSIZE | YSIZE | STRIDE |
|---|---|---|---|---|---|
| RAW10 | 1600 x 1200 | 5 | 0x7CF (1999) | 0x4AF | 0x7D0 (2000) |
| YUV422 | 640 x 480 | 8 | 0x4FF (1279) | 0x1DF | 0x500 (1280) |

HAL-side stride rule (`NSCamPipe` helper at libcam.campipe 0x5654): for BAYER10 the pixel pitch is rounded up to a
multiple of 8 pixels (10 bytes), for BAYER12 to a multiple of 4 (6 bytes), BAYER8 unchanged. So the pitch is a multiple
of 2 bytes; no larger alignment check exists in the stock code [C]. PB = "bytes per 4 pixels" is an interpretation of
the constants 4/5/6/8 that makes the formulas yield byte counts for packed RAW8/10/12 and 16-bit YUV [I].

Packed RAW layout (BAYER10 verified on hardware, 8 and 12 by analogy [I]): each line is a continuous little-endian
bitstream, pixel i occupying bits [i * bpp, (i + 1) * bpp), LSB first. For RAW10, per 5 bytes b0..b4:
`p0 = b0 | (b1 & 3) << 8`, `p1 = b1 >> 2 | (b2 & 0xf) << 6`, `p2 = b2 >> 4 | (b3 & 0x3f) << 4`, `p3 = b3 >> 6 | b4 << 2`.
This is not the MIPI CSI-2 RAW10 layout (four MSB bytes then a byte of LSB pairs): unpacking a frame that way gives an
inter-channel correlation of 0.06 against 0.99 with the bitstream unpacking. The pack codes select 8/10/12-bit packed
output only; the ISP has neither MIPI-packed nor 16-bit unpacked RAW output. V4L2 calls this layout
`V4L2_PIX_FMT_MTISP_S*` (the MT8183 ISP pass 1 uses the same packing).

### 6.2 Base address and buffer ring

The vendor never writes IMGO_BASE_ADDR from userspace. The buffers are enqueued to the kernel ring
(`ISP_BUFFER_CTRL`, camera_isp.c:2878), the userspace `cqRingBuf` (libcamdrv 0xb980) turns them into CQ0C descriptors
(max 6, `ISP_RT_CQ0C_BUF_SIZE`; the descriptor init copies 6 entries of 0x158 bytes, `IspDrvImp::init`), and
`ISP_TOP_CTRL::_enable` points the hardware at the ring: `ISP[0x0BC] = getCQDescBufPhyAddr(3)` (+0x30 when the ring
has >= 2 buffers) [C]. The hardware command queue then loads IMGO_BASE_ADDR before each frame. The stock kernel IRQ
handler can also write `IMGO_BASE_ADDR` directly (to exchange a buffer) when
`FB_NUM != 0 && (FBC_CNT == FB_NUM || FBC_CNT == FB_NUM - 1)` and the current base equals the buffer being replaced
(camera_isp.c:4051-4080 at PASS1_DON, 4111-4150 at SOF1, and 2999 in the enqueue ioctl) [C]. That direct write is the only evidence that a CPU write to the base
register is accepted during streaming; whether it is latched immediately or at the next frame start is not shown
[?].

CQ-free scheme proposed here [I]: leave `0x0BC` and `CTL_SEL` CQ bits untouched, write `IMGO_BASE_ADDR` with the next
queued buffer in the PASS1_DON handler (7.2), and keep FBC off (6.3).

### 6.3 Frame-buffer controller (FBC), IMGO_FBC +0x0F4, IMG2O_FBC +0x0F8 [C]

Layout (`CQ_RTBC_FBC`, camera_isp.h):

```
[3:0]   FBC_CNT        filled buffers
[4]     DROP_INT_EN
[11]    RCNT_INC       write 1 to release one buffer (RCNT++, FBC_CNT--)
[14]    FBC_EN
[15]    LOCK_EN
[19:16] FB_NUM         buffers in the ring
[23:20] RCNT           read counter; the buffer to dequeue is index RCNT - 1
[27:24] WCNT           write counter
[31:28] DROP_CNT       frames dropped because the ring was full
```

Vendor init in `ISP_TOP_CTRL::_enable` (pass 1, direct, libimageio_plat_drv 0x12964): for IMGO then (if DMA_EN bit10)
IMG2O:

```
n = rtBufCtrl(GET_SIZE, id);         // ring size from the kernel
FBC |= 0x10;                         // DROP_INT_EN
FBC = (FBC & 0xFFF0FFFF) | (n & 0xF) << 16;   // FB_NUM
FBC &= ~0x4000;  usleep(10);  FBC |= 0x4000;  // FBC_EN pulse
```

Stop (`_disable`): `FBC &= ~0x4000` for both, then `DMA_EN_CLR (0x094) |= 0x20`, `EN2 &= ~0x4000` [C].
Dequeue in the stock kernel uses `RCNT - 1` as the ring index (camera_isp.c:3107) and release writes the register back with `RCNT_INC = 1` (camera_isp.c:3039, 3044).
FB_NUM is 4 bits, so at most 15 buffers; the vendor ring is capped at 6 by the CQ0C descriptor ring.
Behaviour with `FBC_EN = 0` is not exercised by the stock code; expectation: IMGO keeps writing the address in
IMGO_BASE_ADDR every frame [?].

## 7. Interrupts

### 7.1 Enable (INT_EN +0x020) [C value]

`CamIOPipe::configPipe` stores the constant `0x01A01E31` (libimageio 0xaba0/0xada8) and `ISP_TOP_CTRL::_config`
ORs it into `+0x020`. Bits (names from camera_isp.h, same numbering as INT_STATUS):

| Bit | Name | Bit | Name |
|---|---|---|---|
| 0 | VS1 | 11 | PASS1_TG2_DON |
| 4 | TG1_ERR | 12 | SOF1 |
| 5 | VS2 | 20 | IMGO_ERR |
| 9 | TG2_ERR | 21 | AAO_ERR |
| 10 | PASS1_TG1_DON | 23 | IMG2O_ERR |
|  |  | 24 | ESFKO_ERR |

Minimal set for a single-sensor capture: VS1 (0), TG1_ERR (4), PASS1_TG1_DON (10), SOF1 (12), IMGO_ERR (20), plus
DMA_ERR (30) if wanted. Other INT_STATUS bits per camera_isp.h: 1/2 TG1_ST1/ST2, 3 EXPDON1, 13 CQ_ERR, 14 PASS2_DON,
15 TPIPE_DON, 16 AF_DON, 17 FLK_DON, 19 CQ_DON, 25 FLK_ERR, 26 LSC_ERR, 27 LSC2_ERR, 28 BPC_ERR, 30 DMA_ERR [C].

### 7.2 Frame completion and acknowledge [C]

* Frame done = INT_STATUS bit10 (PASS1_TG1_DON). `CamIOPipe::irq(PASS1_TG1, PATH_DONE)` waits for 0x400 and
  `irq(PASS1_TG1, VSYNC)` waits for 0x1 (libimageio 0x9ff8); the wait is `ISP_WAIT_IRQ` with Clear = ISP_IRQ_CLEAR_WAIT,
  Type = INT, timeout 5000 ms (`CamPathPass1::_waitIrq`, libimageio_plat_drv 0xecb8).
* Acknowledge by reading INT_STATUS (read-clear). The IRQ is level low, so the read must happen in the handler.
* SOF1 (bit12) is used by the stock kernel to timestamp and to exchange buffers; PASS1_DON to finalise the filled
  buffer.
* Error handling: TG1_ERR is followed by a dump of SENINF 0x15008018/0x14; the stock kernel only logs.

### 7.3 The `+0x028/+0x034/+0x040` writes [?]

Pass-1 config ORs `0x04000000` (ID 2) or `0x04100000` (ID 8) into `+0x028`. The kernel reads `+0x028` as the read-clear
"DMA_INT" status whose defined bits are 0..15 (IMGO_DONE = bit0, ...). Bits 20 and 26 are not defined there. Either
the register has a different write layout (enable) or these bits are additional enables. They do not matter for
the frame-done path; the DMA-done bits are not enabled by the vendor in pass 1.

## 8. Start and stop

### 8.1 Start (`CamPath_B::_start`, 0xd94c; modules enabled in reverse list order, TOP last)

1. All config of 3.4/5.2/6.1 done, TG configured (section 4), IMGO buffers known.
2. `ISP_TOP_CTRL::_enable` (pass 1, direct branch) [C]:
   ```
   ISP[0x160] = 0x6000;                         // debug select
   FBC init for IMGO (and IMG2O if enabled)     // 6.3
   SENINF[0x014] = 0x7f;                        // [I] sibling document: write to IMGSYS+0x8014 (0x15008014)
   ISP[0x0BC] = ring base PA (CQ0C)             // 6.2
   if (EN1 & 1) TG_VF_CON |= 1;                 // VFDATA_EN, the actual stream start
   ```
3. After the first VS the stock `CamIOPipe::start` discards the sensor's startup frames
   (`skipFrame(n)`, libcam.campipe 0x5a9c, `n` from sensor feature 0x200D "delay frame").

### 8.2 Single frame vs stream [I]

`TG_VF_CON.SINGLE_MODE` (bit1) = 1 captures exactly one frame after VFDATA_EN rises; 0 streams. The stock
single-capture path (EScenarioID 0/6/7) sets it; ID 8 and 2 stream. A one-frame RAW dump can therefore be done with
the ID 8 register images, then `TG_VF_CON |= 0x2` before setting VFDATA_EN, and a wait for PASS1_DON.

### 8.3 Stop (`ISP_TOP_CTRL::_disable`, 0x1e150, direct branch) [C]

```
TG_VF_CON &= ~1;                                  // if EN1 bit0 (TG1)
wait INT_STATUS VS1 (0x1) [or 0x20 for TG2]       // timeout 5 s
IMGO_FBC &= ~0x4000;  IMG2O_FBC &= ~0x4000;
DMA_EN_CLR (+0x094) |= 0x20;
EN2 &= ~0x4000;
wait VS again if TG_INTER_ST[13:8] != 1           // TG not yet idle
```

Then buffers are released and the pipe is uninitialised; the final user triggers the reset of 3.2.

## 9. Minimal bring-up recipes (pseudo-C)

`W32(off, v)`: write `ISP + off`. `RMW(off, clr, set)`. Items marked [C] come directly from the code; the sequence
as a whole is [I] because it has not been run without the stock library.

### 9.1 RAW (Bayer, SP2508 / GC2355 type), IMGO path, stream

```c
/* power, reset */
enable_clocks(SMI_COMMON, CAM_SMI, CAM_CAM, SEN_TG, SEN_CAM, LARB2_SMI);          /* [C] */
emi_write(0x120, emi_read(0x120) | 0x3f);                                          /* [C] */
W32(0x05C, 1); poll(0x05C & 2); W32(0x05C, 5); W32(0x05C, 4); W32(0x05C, 0);       /* [C] */
/* defaults */
W32(0x004, 0x40000000); W32(0x008, 0); W32(0x00C, 0); W32(0x018, 0);               /* [C] */
/* SENINF / MIPI RX / TG clock: sibling document */
/* TG */
W32(0x418, xs | (xs + W) << 16);  W32(0x41C, ys | (ys + H) << 16);                 /* [C] */
RMW(0x410, 0, 0x5);                  /* MIPI: and clear 0x300; parallel: set 0x100 */ /* [C] */
RMW(0x420, 0x3 | 0x10, 0);
W32(0x09C, 0x70000 | 0x1000000);  W32(0x098, (tgfmt /*1 for RAW10*/) << 16);       /* [C] */
RMW(0x414, 0x1f02, 0x1000 | frcon << 8);                                            /* [C] */
/* top-level path: ID 8, RAW, IMGO only, continuous (table 5.2) */
W32(0x020, 0x01A01E31);              W32(0x028, 0x04100000);                        /* [C] */
W32(0x06C, 0x80);  W32(0x150, 0x1ffff); W32(0x4F4, 0);                              /* [C] */
W32(0x380, 0x04000008); W32(0x384, 0); W32(0x39C, 0x08000020); W32(0x3A0, 0);
W32(0x280, 0x08000010); W32(0x284, 0);
W32(0x084, 0x2000);  W32(0x094, 0);  W32(0x08C, 0x20);                              /* CLRs, optional */
W32(0x080, 0x40001001); W32(0x088, 0); W32(0x090, 0x1);                             /* [C] */
W32(0x098, 0x00001006 /* nibble 1 = RAW10; 0x0006 RAW8; 0x2006 RAW12 */);
W32(0x0A0, 0x00010050);
W32(0x0C0, 0x00000000); W32(0x0C8, 0xC028410C); W32(0x0D0, 0x110054F7);
W32(0x0D8, (pixid & 3) | 0x100000); W32(0x0DC, ~pixid & 3);
RMW(0x414, 0x2, 0);                  /* continuous: SINGLE_MODE = 0 */
/* IMGO DMA */
W32(0x308, ((((W * PB + 3) >> 2) + 1) & ~1) - 1);   /* PB = 4/5/6 for 8/10/12 bit */
W32(0x30C, H - 1);  W32(0x310, (stride_px * PB) >> 2);
W32(0x314, 0x08100850); W32(0x318, 0x00100800); W32(0x31C, 0);
W32(0x300, buf[0]);                                                                  /* [I]: CQ-free */
/* start */
W32(0x160, 0x6000);
W32(0x020, ... already set);
RMW(0x414, 0, 0x1);                                                                  /* VFDATA_EN [C] */
/* IRQ loop */
st = R32(0x024);                     /* read-clear */
if (st & (1 << 10)) { done = cur; W32(0x300, next_buf); ... }
if (st & ((1 << 4) | (1 << 20) | (1 << 30))) { /* error: stop, reset */ }
```

### 9.2 YUV422 (GC0310 / SP0A20 type), pass-through, stream

Same as 9.1 with these differences [C values from the table, [I] for the combination]:

```c
tgfmt = 3;  /* FMT_SEL[18:16], YUV422 */
W32(0x418, xs | (xs + 2 * W) << 16);   /* two TG samples per pixel */
W32(0x080, 0x08000001);  /* EN1: TG1_EN | bit27 */
W32(0x084, 0x00003000);  /* optional */
W32(0x098, 0x00002016);  /* SCENARIO 6, SUB_MODE 1, nibble 2 */
W32(0x0A0, 0x00010050);
W32(0x0C0, 0x00400040); W32(0x0C8, 0xC0081108); W32(0x0D0, 0x110054F7);
/* PB = 8 */
W32(0x308, ((((W * 8 + 3) >> 2) + 1) & ~1) - 1);   /* = 2W - 1 for even W */
W32(0x30C, H - 1);  W32(0x310, (stride_px * 8) >> 2);
```

Pixel/byte order: the vendor ISP configuration contains no U/V swap (no field of this kind appears in
`ISP_TOP_CTRL::_config`, `DMAO_B::_config` or the TG code); the HAL only labels the output with the image format
that matches the sensor order (`mapYUVFormat`: sensor order 4..7 (UYVY, VYUY, YUYV, YVYU) maps to image formats
0x102, 0x103, 0x14, 0x101). Expect the memory byte order to equal the sensor's order [I]. Any swap would be a SENINF
or MDP (pass 2) feature [?].

IMGO is the only output needed. IMG2O adds `DMA_EN` bit10, EN1/EN2 resizer bits and a second FBC; skip it.

## 10. Memory, alignment, cache, M4U

* IMGO writes **bus addresses**. The stock userspace allocates buffers with ION through `IMemDrv`, maps them with the M4U
  (`m4u_alloc_mva`, module 0x14) and configures all six camera SMI ports as **virtual** (`MTKM4UDrv::m4u_config_port`
  for ports 0x14 IMGI, 0x11 IMGO, 0x12 IMG2O, 0x13 LSCI, 0x15 ESFKO, 0x16 AAO with `Virtuality = 1`;
  `IMemDrvImp::init`, libcamdrv 0x27b76..0x27c9a; port ids = `m4u_port.h` larb2 numbering 17..22 plus IMGI = 20)
  [C]. So the addresses the hardware sees on stock are M4U virtual addresses, not physical addresses.
  For the mainline driver use either the M4U properly or leave the larb2 ports in bypass/physical mode; the
  reset state of the larb2 MMU-enable register is not documented here [?].
* Alignment: the stock code enforces none beyond what is implicit: buffers come page aligned from ION, pitch is a
  multiple of 2 bytes (6.1). Recommend 64-byte aligned bases and a pitch rounded up to 16 bytes until measured [I].
* Size limits: IMGO_XSIZE/STRIDE are 16-bit-class fields in the ISP (the vendor writes up to the full width; TG grab
  fields are 15 bits, i.e. max 32767). The vendor's own limit is the 2304-pixel IMG2O/HRZ path, not IMGO [I].
* Cache: DMA buffers are CPU-cached on the stock stack and the library calls `m4u_cache_sync`
  (`IMemDrvImp::cacheSyncbyRange`, libcamdrv) after DMA; use `dma_sync_single_for_cpu` before reading a finished frame.
* Number of buffers: FBC `FB_NUM` 4 bits (<= 15); stock ring <= 6 (`ISP_RT_CQ0C_BUF_SIZE`, include/mach/camera_isp.h:246) [C].
* Bandwidth: the EMI limiter write (`EMI+0x120 |= 0x3f`) happens before every reset [C].

## 11. Verification index

| Claim | Where |
|---|---|
| Reset sequence | camera_isp.c:1669-1735 (`ISP_Reset`) |
| Clock list | camera_isp.c:1616-1666 |
| IRQ registration / handler | camera_isp.c:4933, 3939-4194 |
| FBC layout | include/mach/camera_isp.h `CQ_RTBC_FBC` |
| RTBC enqueue/dequeue FBC handling | camera_isp.c:2878-3320 |
| Top-level reg writes | libimageio_plat_drv 0x19e38 (`ISP_TOP_CTRL::_config`) |
| VF enable / FBC init | libimageio_plat_drv 0x12964 (`_enable`) |
| Stop | libimageio_plat_drv 0x1e150 (`_disable`) |
| IMGO geometry | libimageio_plat_drv 0x117e4 (`DMAO_B::_config`) |
| Scenario/fmt -> images | libimageio 0xa270 (`CamIOPipe::configPipe`), switch table at 0xa75c |
| Default regs | libimageio 0x9750 (`CamIOPipe::init`) |
| TG regs | libcamdrv 0x11fd4, 0x12024, 0x12078, 0x121bc, 0x12c2c; caller 0x15560 |
| Pixel packing | libimageio 0xa9e6..0xaa28; stride rule libcam.campipe 0x5654 |
| IRQ wait mapping | libimageio 0x9ff8, libimageio_plat_drv 0xecb8 |
| Module register groups | libcamdrv data 0x53160 (`gIspCQModuleInfo`) |
| M4U ports | libcamdrv `IMemDrvImp::init` 0x27b76; kernel `m4u_port.h` |

## 12. Open questions

1. Names of the EN1/EN2/DMA/MUX/CTL bits (only EN1[0], EN1[1], EN1[9], DMA[0], DMA[10] are established).
2. What `+0x028/+0x034/+0x040` writes (0x04000000/0x04100000) enable, and whether the DMA_INT status needs its own enable.
3. Whether IMGO_BASE_ADDR may be rewritten at PASS1_DON and when it is latched; behaviour with FBC disabled and no CQ.
   First thing to try on hardware: ID 8 RAW images, FBC off, single buffer, PASS1_DON, read a frame.
4. Purpose of `+0x06C = 0x80`, `+0x150 = 0x1FFFF`, `+0x280/0x380/0x39C` constants, `+0x4F4`, and `SENINF+0x014 = 0x7F`.
5. Does the reset really finish at bit1 and are all control registers zero afterwards (needed for the "SET only" shortcut).
6. IMGO byte order and U/V order for YUV, and the TG_FMT value for 8-bit and 12-bit RAW sensors (the HAL always passes 1 for
   sensor type 0; the pack nibble and PB carry the depth).
7. SMI larb2 / M4U state for the camera ports at boot (bypass vs virtual) and the SMI bandwidth registers.
8. Meaning of the EScenarioID 2 vs 8 register differences (MUX2 0x40080104 vs 0xC028410C, CONF 0x54F7 vs 0x110054F7);
   ID 8 is used above because it needs the fewest extra enable bits for IMGO-only.
9. Single-capture scenarios 0/6/7: input source and whether they are required for a one-shot dump.
10. Exact alignment requirement of IMGO_BASE_ADDR and STRIDE.
