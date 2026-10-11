# SP2508 image tuning recovered from the stock MediaTek camera stack

The stock Android 5.0.1 image of the TB3-710F carries a complete MediaTek ISP tuning for the rear SP2508:
black level handling, lens shading, AWB calibration, colour correction, gamma, AE tables and noise filters.
This document describes where each table lives, how it is encoded, what it contains and how it maps onto the
libcamera software ISP and onto `tools/raw2rgb.py`.

Confidence levels: **C** confirmed (format checked against code and data), **P** probable (consistent with the
data, one link not proven), **G** guess.

Files:

- `tools/mtk_tuning_extract.py libcameracustom.so data/sp2508`: extractor (parses the ELF, never loads it)
- `data/sp2508/isp_param.json`: ISP register sets (OBC, BPC, NR1, LSC, SL2, CFA, CCM, GGM, ANR, CCR, EE), CCM
  polynomial
- `data/sp2508/3a_param.json`: AE and AWB NVRAM
- `data/sp2508/ae_pline.json`: AE exposure/gain tables and the AE mode mapping
- `data/sp2508/lsc.json`: lens shading coefficients and the block corner gains derived from them
- `tools/raw2rgb.py`: host reference processor using these tables
- `ipa/simple/sp2508.yaml`, `libcamera-patches/`: libcamera v0.5.0 software ISP tuning, sensor helper and
  sensor properties

## 1. Sources

`system/lib/libcameracustom.so` (stripped, dynamic symbols kept). The sensor table registers
`NSFeature::RAWSensorInfo<9480u>` (9480 = 0x2508) and `RAWSensorInfo<9045u>` (0x2355, the GC2355 fitted on other
units). `RAWSensorInfo<9480u>::impGetDefaultData` (0x17d0c) copies the default data by type
(`CAMERA_DATA_TYPE_ENUM`):

| Type | Content | Address | Size | Struct |
|---|---|---|---|---|
| 0 | ISP parameters | 0x1dac4 (.rodata) | 0x5800 | `NVRAM_CAMERA_ISP_PARAM_STRUCT` |
| 1 | 3A (AE, AWB) | 0x133b44 (.rodata) | 0x1004 | `NVRAM_CAMERA_3A_STRUCT` |
| 2 | Lens shading | 0x33328 (.rodata) | 0x10081c | `NVRAM_CAMERA_SHADING_STRUCT` |
| 4 | AE plines | 0x26ab98 (.data, file offset -0x1000) | 0xa4 | `AE_PLINETABLE_T` |
| 6 | TSF tables | 0x232c4 (.rodata) | 0x10064 | `CAMERA_TSF_TBL_STRUCT` |

The ISP and 3A blocks start with version 1 and sensor id 0x2508. The struct layouts are those of the MT8127
`camera_custom_nvram.h`/`ispif.h` (KitKat-era MediaTek custom headers, e.g. github.com/bq/edison-3
`mediatek/custom/mt8127/hal/inc`); the computed layout ends exactly where the data ends (ISP: the CCM polynomial
ends at 0x5014, all zero after it) **C**.

Code that consumes the tables, used to pin the formats: `lib3a.so` `MultiCCM`/`Interpolation` (dynamic CCM),
`AeAlgo::UpdatePreviewFlare` and `libfeatureio.so` `AeMgr::UpdateFlare2ISP`/`ISP_MGR_PGN::setIspPregain` (flare),
`libcamalgo.so` `LscGaintoHWTbl` (shading coefficients), `libcameracustom.so`
`IspTuningCustom::evaluate_Shading_CCT_index`, `is_to_invoke_dynamic_ccm` (returns 1), `isEnableTSF` (1 for the
main sensor).

The MediaTek ISP order is: OBC (black) -> BPC (defects) -> NR1 (Gr/Gb crosstalk) -> LSC -> PGN (white balance
gains, flare) -> CFA (demosaic) -> G2G (CCM) -> GGM (gamma) -> G2C/ANR/EE (YUV, noise, edges).

Active register set indexes (`ISPRegs.Idx`): OBC 0, BPC 1, NR1 1, LSC 0, SL2 0, CFA 1, CCM 2, GGM 0, ANR 2, CCR 1,
EE 2. The stock tuning code changes some of them per ISO at run time; `evaluate_nvram_index` is empty in this build.

## 2. Black level

- ISP OBC (`ISP_NVRAM_OBC_T`, 12 sets, all identical): offsets `0x2000` for the four channels, gains `0x200`.
  The offset field is 13 bits (Q1.12, two's complement), so the programmed offset is 0; the gain field is
  Q0.4.9, 0x200 = 1.0. The ISP subtracts no fixed black level **C**.
- AE flare: `UpdatePreviewFlare` and `UpdateFlare2ISP` program the PGN block with `offset = -flare` and
  `gain = 4095 * 512 / (4095 - flare)` (12-bit domain), i.e. `out = (in - flare) * 4095 / (4095 - flare)`
  together with the AWB gains **C**. The flare is estimated per frame from the dark end of the AE histogram
  (`getFlareAlgo`, `CalculateHWFlareValue`). The 3A NVRAM fixed flare offsets are 64 for preview, capture and
  video and 32 for strobe; the capture/video/strobe threshold switches are 0 and the preview max/min flare
  thresholds 8/0.
- Sensor side: the init table sets the four "offset" registers (page 1, 0xf0..0xf3) to 0, and the vendor log
  mentions an earlier "offset h'20" that was removed. Whether the sensor adds a pedestal is not documented.

Result: the stock black level is the flare offset, 64 at 12 bits = **16 at 10 bits** (P). The libcamera tuning
uses `blackLevel: 1024` (16-bit scale) and `raw2rgb.py` uses `--black 16`. A dark frame (lens covered, minimum
gain) gives the real value; update both if it differs.

## 3. Lens shading

### Geometry (`ISP_NVRAM_LSC_T`, LSC[0], used for every mode) **C**

| Register | Value | Meaning |
|---|---|---|
| CTL1 | 0x30000000 | block offsets 0, 0 |
| CTL2 | 0xf032 | 16 blocks across (XNUM 15), width 50 |
| CTL3 | 0xf025 | 16 blocks down (YNUM 15), height 37 |
| LBLOCK | 0x0032002d | last block width 50, height 45 |
| RATIO | 0x20202020 | 32 for the four ratios (strength 1.0) |

Units are 2x2 Bayer quads: 15 x 50 + 50 = 800 quads = 1600 pixels, 15 x 37 + 45 = 600 quads = 1200 pixels. The
grid covers the 1600x1200 image area that the stock grabs at (2, 2) from the 1616x1216 sensor output, so on a
full-frame capture of `mtk_camsys` the shading origin is pixel (2, 2); the Bayer phase (BGGR) is unchanged.

### Table layout (`SHADING_DATA`) **C**

`Version` 1, `SensorId` field 1, `LSCSize[6]` 0, SVD sizes 2048, then at offset 0x1c six tables of 4 colour
temperatures x 4096 words: `PreviewFrmTable`, `CaptureFrmTable`, `CaptureTilTable`, `VideoFrmTable`,
`N3DPvwTable`, `N3DCapTable`. Only the preview and capture tables have the four colour temperatures filled; the
others hold one table (copies of a preview/capture table).

One table = 16 x 16 blocks (row-major) x 4 channels x 4 words. Channels are in Bayer raster order: B, Gb, Gr, R
(P: the per-channel falloff is largest on channel 3, as expected of red). Each word holds three 10-bit fields
(bits 0-9, 10-19, 20-29), sign and magnitude (bit 9 sign), giving coefficients c0..c11 in field order.

### Coefficient meaning **C**

`LscGaintoHWTbl` (libcamalgo 0x637b0) builds the coefficients from a gain grid with natural cubic splines along
each axis and scales them by 512 (c0..c8), 256 (c9, c10) and 128 (c11) before packing. Inverting it, the gain at
normalised position (u, v) in a block (u horizontal, both in [0, 1]) is:

```
gain = c11/128 + (c9*u + c10*v)/256
     + (c0*u^3*v + c1*u*v^3 + c2*u^3 + c3*u^2*v + c4*u*v^2 + c5*v^3 + c6*u^2 + c7*u*v + c8*v^2)/512
```

Check: the value at the right/bottom edge of every block equals the value at the left/top edge of its
neighbour within 0.01 (the 1/128 quantisation of c11) for all tables and channels.

### Content

Corner gains (centre 1.0):

| Table / CT index | B | Gb | Gr | R |
|---|---|---|---|---|
| preview 0 | 1.83 | 1.95 | 1.91 | 1.97 |
| preview 1 | 1.84 | 1.94 | 1.91 | 1.99 |
| preview 2 | 1.84 | 1.91 | 1.90 | 2.09 |
| preview 3 | 1.79 | 1.86 | 1.84 | 2.14 |
| capture 0 | 1.57 | 1.53 | 1.53 | 1.96 |
| capture 1 | 1.41 | 1.45 | 1.45 | 1.66 |
| capture 2 | 1.42 | 1.47 | 1.48 | 1.82 |
| capture 3 | 1.34 | 1.32 | 1.38 | 1.34 |

Preview and capture use the same sensor mode; the capture tables are weaker. The preview tables are the ones
used for the continuous stream.

CT index selection (`evaluate_Shading_CCT_index`, with hysteresis on the AWB CCT): 0 below 2500-2800 K, 1 up
to 4672-5154 K, 2 above, i.e. 0 = A, 1 = TL84/CWF, 2 = D65. Index 3 is not selected by this code **C**.

TSF (temperature-shading-free, adaptive shading) is enabled for the main sensor (`isEnableTSF(1)` = 1); its
tables (type 6) are not decoded. The stock shading is therefore the table above refined per scene.

The software ISP of libcamera v0.5.0 has no lens shading stage; `raw2rgb.py` applies it.

## 4. White balance

`AWB_NVRAM_T` (3A block from word 74) **C**:

- Calibration: unit, golden and tuning-unit gains 0 (no per-module OTP calibration), D65 gain R 717, G 539,
  B 512. The stock driver has OTP code (`SP2508MIPI_USE_OTP`) that is compiled out.
- Light source gains, 512 = 1.0, normalised so the smallest gain is 512:

| Light | R | G | B | R/G | B/G | CCT | Rotated X, Y |
|---|---|---|---|---|---|---|---|
| Horizon | 512 | 735 | 1068 | 0.697 | 1.453 | 2300 | -272, -4 |
| A | 512 | 627 | 861 | 0.817 | 1.373 | 2850 | -192, -43 |
| TL84 | 587 | 512 | 648 | 1.146 | 1.266 | 4100 | -36, -138 |
| CWF | 619 | 512 | 656 | 1.209 | 1.281 | | -21, -161 |
| DNP | 557 | 512 | 584 | 1.088 | 1.141 | 5100 | -17, -80 |
| D65 | 717 | 539 | 512 | 1.330 | 0.950 | 6500 | 125, -86 |
| DF | 557 | 512 | 584 | 1.088 | 1.141 | | -17, -80 |

  The CCT column is `rCCTEstimation` (2300, 2850, 4100, 5100, 6500 K at rotated X -397, -317, -161, -142, 0).
- Rotation matrix angle 0 (cos 256, sin 0), daylight locus slope -94/128; light areas, preference gains (all
  512) and the preset (PWB) gains are in `3a_param.json`. Presets, R/G and B/G: daylight 1.21/1.04, cloudy
  1.45/0.87, shade 1.55/0.81, twilight 0.92/1.37, fluorescent 1.21/1.15, warm fluorescent 0.91/1.53,
  incandescent 0.87/1.46.

The gains are applied on the shading-corrected raw data (PGN). They are small for a raw sensor (D65 R/G 1.33);
the reason (sensor-side processing, IR-cut filter) is not known, the CCMs below are tuned for these gains.

## 5. Colour correction

- `ISP_NVRAM_CCM_T` (G2G, 4 sets): CCM[0] = [1.0, -0.168, 0.168; -0.496, 2.617, -1.121; -0.434, -1.512, 2.945],
  the others are zero. Format: 11-bit two's complement, 256 = 1.0, row-major (CONV0A = c00 | c01 << 16,
  CONV0B = c02, ...) **C**. The fixed matrices are not used: `is_to_invoke_dynamic_ccm` returns 1.
- Dynamic CCM (`ISP_NVRAM_CCM_POLY22_STRUCT`, `lib3a.so` `Interpolation` 0x21738) **C**:
  - `x = (512*R/G - R_AVG/100) / (R_STD/100)`, `y = (512*B/G - B_AVG/100) / (B_STD/100)` from the current AWB
    gains; R_AVG 56425, R_STD 10435, B_AVG 62750, B_STD 9359.
  - each element `m_i = P00_i + P10_i*x + P01_i*y + P20_i*x^2 + P11_i*x*y + P02_i*y^2`, coefficients / 10000,
    result in 1/256 units, truncated to an integer.
  - if a diagonal element is below 258, (x, y) is scaled towards 0 until it reaches 258 (smallest scale over
    the three diagonals);
  - the off-diagonal elements are scaled down so that each row's off-diagonal sum stays within +-765 and each
    element within +-1021;
  - the diagonal is set to 256 minus the row's off-diagonal sum (rows sum to 1.0).
  - The four light source gains passed to `MultiCCM` are not used by `Interpolation`.
- Resulting matrices at the AWB light sources (rows R, G, B; applied to white-balanced camera RGB):

| Light | Matrix |
|---|---|
| Horizon | 1.008 0.453 -0.461 / -0.512 2.168 -0.656 / -0.582 -1.402 2.984 |
| A | 1.008 0.445 -0.453 / -0.508 2.168 -0.660 / -0.574 -1.395 2.969 |
| TL84 | 2.387 -1.227 -0.160 / -0.480 1.762 -0.281 / -0.348 -1.176 2.523 |
| CWF | 2.754 -1.648 -0.105 / -0.492 1.676 -0.184 / -0.309 -1.133 2.441 |
| DNP | 1.746 -0.527 -0.219 / -0.422 1.883 -0.461 / -0.363 -1.227 2.590 |
| D65 | 2.480 -1.477 -0.004 / -0.352 1.613 -0.262 / -0.168 -1.070 2.238 |
| Mean (x = y = 0) | 2.054 -0.849 -0.206 / -0.463 1.840 -0.377 / -0.370 -1.213 2.583 |

  Below about 3000 K the red diagonal hits the 258 limit, hence the near-unity red row.
- CCR (chroma suppression, 7 sets) and PCA (hue/saturation LUT, slider 32/-35/13) follow in the YUV domain and
  are kept as raw registers in `isp_param.json`.

## 6. Gamma

`ISP_NVRAM_GGM_T`, 5 sets (GGM[0] active, GGM[1..3] alternatives, GGM[4] empty) **C** for the encoding, **P**
for the input spacing:

- 144 entries; RB word = R | B << 16, G word = G; R, G and B are identical in every set.
- Each 16-bit entry is `out | (next_out - out) << 10`: bits 0-9 the 10-bit output, bits 10-15 the step to the
  next entry (checked for all entries).
- Input breakpoints (10-bit input): entries 0-63 every 2 codes, 64-95 every 4, 96-127 every 8, 128-143 every
  32 (0..1008, then 1024). The per-entry output deltas double at entry 64 and roughly quadruple at entry
  128, as the input step does, and with this spacing the slope decreases along the curve apart from small
  tuning ripples.
- GGM[0] reaches 0.31 at 1/8 input, 0.56 at 1/4, 0.79 at 1/2 and 0.99 at 1008: a darker toe and brighter
  mid-tones than sRGB (0.39, 0.54, 0.74). The decoded tables are `isp_param.json` `ggm[n].input/r/g/b`.

libcamera v0.5.0 uses a fixed gamma of 0.5 with an optional contrast S-curve; the stock curve cannot be
expressed in its tuning file. `raw2rgb.py --gamma stock0` uses it.

## 7. Auto exposure

AE devices info (3A NVRAM) **C**:

| Field | Value |
|---|---|
| min / max gain (1024 = 1x) | 1228 (1.20x) / 8120 (7.93x) |
| ISO at 1x | 54, gain step 128 |
| line time (preview, video, capture) | 38 668 ns |
| max frame rate | 21 fps |
| lens | f/2.8, 3.50 mm |
| AE target | 60 (8-bit, central weighting), initial index 50 |
| histogram stretch / anti over-exposure / backlight | enabled / disabled / enabled, see `3a_param.json` |

Line time = 1160 sensor clocks / 30 MHz = 38.667 us, which matches the driver (HTS 2320 pixels at 60 Mpixel/s).

Plines (`ae_pline.json`, one row = exposure in us, sensor gain x1024, ISP gain x1024; 60 Hz and 50 Hz variants):
`AE_PLINETABLE_T` only carries the mode mapping pointer (the 40 table pointers are NULL); the 25 `strAETable`
records of this sensor are found through their relocated table pointers in .data 0x264000-0x277000 **C**. AE
mode AUTO maps to `CAPTURE_AUTO`/`CAPTURE_AUTO_ZSD`; preview uses `PREVIEW_AUTO`:

- 131 rows, BV 109 down to -21. Exposure from 200 us at 1.125x sensor gain, flicker-locked multiples of
  8.33 ms (60 Hz) from 8.3 ms to 66.7 ms with the sensor gain between 1.125x and 1.75x in between the steps,
  then gain up to 3.6x at 66.7 ms, then 83-125 ms with gain up to 10x, then exposure up to 191.6 ms at 10x.
- The ISP gain stays 1.0-1.1x; the sensor gain never goes below 1.125x.
- Longest exposures: preview 191.6 ms (4956 lines), capture 200-241.6 ms, night ZSD capture (`MODE_INDEX5`)
  500 ms (12 930 lines).

## 8. Sensor driver comparison (`sensors/sp2508.c` vs the stock driver)

- Gain: stock `SetGain` takes the MediaTek sensor gain (64 = 1x, AE gain / 16) and writes
  `0x24 = 16 * gain / 64` clamped to 0x10..0x90 (1x-9x). Its fractional term divides by `0x10 / BASEGAIN` = 0;
  the effective code is in 1/16 steps. The driver's `ANALOGUE_GAIN` 0x10..0x90, code/16, matches; the libcamera
  helper is `AnalogueGainLinear{1, 0, 0, 16}`. The stock AE requests up to 10x, which the driver clamps to 9x.
- Minimum gain: the stock AE never uses less than 1.125x sensor gain (min gain 1.2x total). Not enforced by the
  driver; the reason is not documented (the vendor log only mentions pinkish highlights fixed by the 1.375x
  digital gain, page 2 0x00/0x01/0x03/0x04 = 0xb0 with 0x80 = 1x).
- Exposure: stock `SetShutter` clamps to 7..0xffff lines and does not touch the frame length (`SetDummy` calls
  are commented out), while the plines ask for up to 4956 lines in preview with VTS 1241: the sensor lengthens
  the frame by itself when the exposure exceeds it. The driver limits `EXPOSURE` to VTS - 5 and `VBLANK` to 211
  (one register byte), i.e. 1422 lines = 55 ms, a quarter of the stock preview maximum; low-light images are
  darker or noisier than stock. The register controlling the frame length beyond 8 bits is not known.
  `SP2508MIPI_MAX_EXPOSURE_LINES` (VTS - 5) is only reported to the CCT tool.
- Delays: stock `AEShutDelayFrame` 0, `AESensorGainDelayFrame` 0, `AEISPGainDelayFrame` 1: exposure and gain are
  latched together by page 1 0x01; libcamera properties use 2/2/2/2.
- Init table: identical to the stock `SP2508MIPI_camera_para_to_sensor` (including 0x24 = 0xa0, overwritten
  by the control handler), plus 0x3f = 0 that the stock writes in `SetFlipMirror(IMAGE_HV_MIRROR)`.
- Orientation: the stock reports the main sensor at 90 degrees (`MAIN_SENSOR_ORIENTATION_ANGLE`,
  `getSensorOrientation` {90, 270}) with the same 0x3f setting, so the device tree `rotation` is 90 for the
  rear sensor (P: Android's sensor orientation and the V4L2 `rotation` property follow the same convention,
  not checked on a picture).
- View angle (stock custom): 53 x 49 degrees.

## 9. Defects and noise

- BPC enabled in all 9 sets (`BPC_CON` 0x301: enable, HF mode, threshold mode), thresholds in `isp_param.json`.
  `raw2rgb.py` clamps each pixel to the range of its 8 same-colour neighbours instead (`--no-dpc` disables it).
- NR1 (Gr/Gb crosstalk) enabled in sets 0 and 1 (`NR1_CON` 0x10, active index 1).
- ANR (YUV noise reduction) enabled with ISO-dependent strength, EE (edge enhancement) 49 sets, both raw in
  `isp_param.json`. The libcamera software ISP has none of these.

## 10. Using the data

### Host processing

```
tools/raw2rgb.py frame.raw out.png -v                 # full stock pipeline, grey-world AWB
tools/raw2rgb.py frame.raw out.png --awb d65          # stock light source gains
tools/raw2rgb.py frame.raw out.png --no-lsc --gamma srgb --black 0
```

Options: `--frame N`, `--black`, `--no-crop` (implies `--no-lsc`), `--no-dpc`, `--no-lsc`,
`--lsc-table preview|capture`, `--lsc-ct 0..3`, `--lsc-strength`, `--awb grey|<light>|<preset>`,
`--gains R/G,B/G`, `--no-awb`, `--no-ccm`, `--gamma stockN|srgb|none|<exponent>`, `--exposure`. With grey
world, the shading table follows the light source nearest to the measured gains.

### libcamera v0.5.0 (simple pipeline, software ISP)

Apply `libcamera-patches/000[1-3]*.patch` on top of v0.5.0 plus the MT8127 simple-pipeline commit:

1. `CameraSensorHelperSp2508`: gain = code / 16, so AGC works in real gain units (1x-9x).
2. `camera_sensor_properties` entry `sp2508`: test patterns (off, colour bars), delays 2/2/2/2.
3. `src/ipa/simple/data/sp2508.yaml` (same content as `ipa/simple/sp2508.yaml`), installed as
   `share/libcamera/ipa/simple/sp2508.yaml` and picked by sensor model name.

The tuning file sets the black level (16 at 10 bits) and five CCMs. The simple IPA of v0.5.0 computes
`diag(awb gains) * ccm`, i.e. it applies the white balance gains after the matrix, while the stock (and the
correct) order is `ccm * diag(gains)`; each matrix in the file is pre-compensated for its light source
(`diag(1/g) * CCM * diag(g)`). If the simple IPA is changed to apply the gains first, use the stock matrices of
section 5 instead. The `ct` keys are the temperatures the simple AWB estimates for those light sources
(`estimateCCT` of the inverse gains: 2637, 3479, 5467, 6425, 13651), not the physical ones, so the matrix
selection follows the gains.

Not expressible in v0.5.0: lens shading, the stock gamma, AE target and plines, noise reduction.

## 11. Open points

- Real black level: measure a dark frame (section 2).
- Gamma input spacing (section 6) is inferred from the slope of the curve.
- LSC channel order B, Gb, Gr, R is inferred from the red channel having the strongest falloff.
- TSF tables are not decoded.
- Frame length register above 8 bits, needed for exposures longer than 55 ms within V4L2 rules.
- Unit cell size of the SP2508 (not in the stock data); 53 degrees horizontal at 3.5 mm would give about
  2.2 um, not verified.
