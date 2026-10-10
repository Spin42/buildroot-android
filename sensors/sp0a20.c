// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the SuperPix SP0A20 YUV camera sensor (MIPI CSI-2, one lane).
 *
 * The register values, the controls and the power sequence come from the
 * vendor driver of the Lenovo Tab3 7 Essential (MT8127).
 *
 * Copyright (c) 2026 Marc Lainez <marc.lainez@gmail.com>
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* Every register lives in a page selected through SP0A20_REG_PAGE. */
#define SP0A20_REG_PAGE			CCI_REG8(0xfd)

/* Page 0 */
#define SP0A20_REG_CHIP_ID		CCI_REG8(0x02)
#define SP0A20_CHIP_ID			0x2b

/* Page 1 */
#define SP0A20_REG_AWB_CTRL		CCI_REG8(0x32)
#define SP0A20_AWB_AUTO			0x15
#define SP0A20_AWB_MANUAL		0x05
#define SP0A20_REG_EFFECT_FREEZE	CCI_REG8(0x36)
#define SP0A20_REG_EFFECT_Y		CCI_REG8(0x66)
#define SP0A20_REG_EFFECT_U		CCI_REG8(0x67)
#define SP0A20_REG_EFFECT_V		CCI_REG8(0x68)
#define SP0A20_REG_EV_BIAS		CCI_REG8(0xdb)

/* Page 2 */
#define SP0A20_REG_AWB_GAIN_R		CCI_REG8(0x26)
#define SP0A20_REG_AWB_GAIN_B		CCI_REG8(0x27)

#define SP0A20_XCLK_FREQ		(24 * HZ_PER_MHZ)

/*
 * The sensor outputs VGA. The vendor driver uses the 632x474 area centered
 * in it as the image.
 */
#define SP0A20_WIDTH			640
#define SP0A20_HEIGHT			480

/* Vendor line and frame length: 784x510 at 13 MHz is about 32 frames/s. */
#define SP0A20_PIXEL_RATE		(13 * HZ_PER_MHZ)
#define SP0A20_HBLANK			(784 - SP0A20_WIDTH)
#define SP0A20_VBLANK			(510 - SP0A20_HEIGHT)

/*
 * Link frequency for one lane of UYVY8 (16 bit per pixel) at
 * SP0A20_PIXEL_RATE: 208 Mbit/s, DDR clock 104 MHz.
 */
static const s64 sp0a20_link_freq_menu[] = {
	104 * HZ_PER_MHZ,
};

static const char * const sp0a20_supply_names[] = {
	"avdd",
	"dovdd",
};

struct sp0a20_awb_preset {
	u8 gain_r;
	u8 gain_b;
};

/* Manual white balance gains, indexed by the V4L2 preset menu value. */
static const struct sp0a20_awb_preset sp0a20_awb_presets[] = {
	[V4L2_WHITE_BALANCE_AUTO] = { 0xac, 0xad },
	[V4L2_WHITE_BALANCE_INCANDESCENT] = { 0x88, 0xd0 },
	[V4L2_WHITE_BALANCE_FLUORESCENT] = { 0xbf, 0xcc },
	[V4L2_WHITE_BALANCE_DAYLIGHT] = { 0xe8, 0xc8 },
	[V4L2_WHITE_BALANCE_CLOUDY] = { 0xdb, 0x90 },
};

#define SP0A20_AWB_SUPPORTED_MASK				\
	(BIT(V4L2_WHITE_BALANCE_AUTO) |				\
	 BIT(V4L2_WHITE_BALANCE_INCANDESCENT) |			\
	 BIT(V4L2_WHITE_BALANCE_FLUORESCENT) |			\
	 BIT(V4L2_WHITE_BALANCE_DAYLIGHT) |			\
	 BIT(V4L2_WHITE_BALANCE_CLOUDY))

struct sp0a20_effect {
	u8 y;
	u8 u;
	u8 v;
};

static const struct sp0a20_effect sp0a20_effects[] = {
	[V4L2_COLORFX_NONE] = { 0x00, 0x80, 0x80 },
	[V4L2_COLORFX_BW] = { 0x20, 0x80, 0x80 },
	[V4L2_COLORFX_SEPIA] = { 0x10, 0x98, 0x58 },
	[V4L2_COLORFX_NEGATIVE] = { 0x04, 0x80, 0x80 },
	[V4L2_COLORFX_SKY_BLUE] = { 0x10, 0x80, 0xb0 },
	[V4L2_COLORFX_GRASS_GREEN] = { 0x10, 0x50, 0x50 },
};

#define SP0A20_COLORFX_SUPPORTED_MASK				\
	(BIT(V4L2_COLORFX_NONE) | BIT(V4L2_COLORFX_BW) |	\
	 BIT(V4L2_COLORFX_SEPIA) | BIT(V4L2_COLORFX_NEGATIVE) |	\
	 BIT(V4L2_COLORFX_SKY_BLUE) | BIT(V4L2_COLORFX_GRASS_GREEN))

/* Exposure bias in milli-EV, matching the vendor steps. */
static const s64 sp0a20_ev_bias_menu[] = {
	-1300, -1000, -700, -300, 0, 300, 700, 1000, 1300,
};

/* Register 0xdb value of each sp0a20_ev_bias_menu entry (two's complement). */
static const u8 sp0a20_ev_bias_regs[] = {
	0xc0, 0xd0, 0xe0, 0xf0, 0x00, 0x10, 0x20, 0x30, 0x40,
};

static const struct cci_reg_sequence sp0a20_init_regs[] = {
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x36), 0x02 },
	{ CCI_REG8(0xfd), 0x00 },
	{ CCI_REG8(0x0c), 0x00 },
	{ CCI_REG8(0x92), 0x11 },
	{ CCI_REG8(0x99), 0x05 },
	{ CCI_REG8(0x1b), 0x27 },
	{ CCI_REG8(0x12), 0x02 },
	{ CCI_REG8(0x13), 0x2f },
	{ CCI_REG8(0x6d), 0x32 },
	{ CCI_REG8(0x6c), 0x32 },
	{ CCI_REG8(0x6f), 0x33 },
	{ CCI_REG8(0x6e), 0x34 },
	{ CCI_REG8(0x99), 0x04 },
	{ CCI_REG8(0x16), 0x38 },
	{ CCI_REG8(0x17), 0x38 },
	{ CCI_REG8(0x70), 0x3a },
	{ CCI_REG8(0x14), 0x02 },
	{ CCI_REG8(0x15), 0x20 },
	{ CCI_REG8(0x71), 0x23 },
	{ CCI_REG8(0x69), 0x25 },
	{ CCI_REG8(0x6a), 0x1a },
	{ CCI_REG8(0x72), 0x1c },
	{ CCI_REG8(0x75), 0x1e },
	{ CCI_REG8(0x73), 0x3c },
	{ CCI_REG8(0x74), 0x21 },
	{ CCI_REG8(0x79), 0x00 },
	{ CCI_REG8(0x77), 0x10 },
	{ CCI_REG8(0x1a), 0x4d },
	{ CCI_REG8(0x1c), 0x07 },
	{ CCI_REG8(0x1e), 0x15 },
	{ CCI_REG8(0x21), 0x08 },
	{ CCI_REG8(0x22), 0x28 },
	{ CCI_REG8(0x26), 0x66 },
	{ CCI_REG8(0x28), 0x0b },
	{ CCI_REG8(0x37), 0x4a },
	{ CCI_REG8(0xfd), 0x02 },
	{ CCI_REG8(0x01), 0x80 },
	{ CCI_REG8(0x52), 0x10 },
	{ CCI_REG8(0x54), 0x00 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x41), 0x00 },
	{ CCI_REG8(0x42), 0x00 },
	{ CCI_REG8(0x43), 0x00 },
	{ CCI_REG8(0x44), 0x00 },
	{ CCI_REG8(0xfd), 0x00 },
	{ CCI_REG8(0x03), 0x00 },
	{ CCI_REG8(0x04), 0x50 },
	{ CCI_REG8(0x05), 0x00 },
	{ CCI_REG8(0x06), 0x00 },
	{ CCI_REG8(0x07), 0x00 },
	{ CCI_REG8(0x08), 0x00 },
	{ CCI_REG8(0x09), 0x02 },
	{ CCI_REG8(0x0a), 0xf4 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0xf0), 0x00 },
	{ CCI_REG8(0xf7), 0x4b },
	{ CCI_REG8(0x02), 0x0e },
	{ CCI_REG8(0x03), 0x01 },
	{ CCI_REG8(0x06), 0x4b },
	{ CCI_REG8(0x07), 0x00 },
	{ CCI_REG8(0x08), 0x01 },
	{ CCI_REG8(0x09), 0x00 },
	{ CCI_REG8(0xfd), 0x02 },
	{ CCI_REG8(0xbe), 0x1a },
	{ CCI_REG8(0xbf), 0x04 },
	{ CCI_REG8(0xd0), 0x1a },
	{ CCI_REG8(0xd1), 0x04 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x5a), 0x40 },
	{ CCI_REG8(0xfd), 0x02 },
	{ CCI_REG8(0xbc), 0x70 },
	{ CCI_REG8(0xbd), 0x50 },
	{ CCI_REG8(0xb8), 0x66 },
	{ CCI_REG8(0xb9), 0x8f },
	{ CCI_REG8(0xba), 0x30 },
	{ CCI_REG8(0xbb), 0x45 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0xe0), 0x50 },
	{ CCI_REG8(0xe1), 0x3e },
	{ CCI_REG8(0xe2), 0x36 },
	{ CCI_REG8(0xe3), 0x30 },
	{ CCI_REG8(0xe4), 0x30 },
	{ CCI_REG8(0xe5), 0x2e },
	{ CCI_REG8(0xe6), 0x2e },
	{ CCI_REG8(0xe7), 0x2c },
	{ CCI_REG8(0xe8), 0x2c },
	{ CCI_REG8(0xe9), 0x2c },
	{ CCI_REG8(0xea), 0x2a },
	{ CCI_REG8(0xf3), 0x2a },
	{ CCI_REG8(0xf4), 0x2a },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x04), 0xa0 },
	{ CCI_REG8(0x05), 0x2a },
	{ CCI_REG8(0x0a), 0xa0 },
	{ CCI_REG8(0x0b), 0x2a },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0xeb), 0x80 },
	{ CCI_REG8(0xec), 0x80 },
	{ CCI_REG8(0xed), 0x04 },
	{ CCI_REG8(0xee), 0x0a },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0xf2), 0x4d },
	{ CCI_REG8(0xfd), 0x02 },
	{ CCI_REG8(0x5b), 0x05 },
	{ CCI_REG8(0x5c), 0xa0 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x26), 0x80 },
	{ CCI_REG8(0x27), 0x4f },
	{ CCI_REG8(0x28), 0x00 },
	{ CCI_REG8(0x29), 0x20 },
	{ CCI_REG8(0x2a), 0x77 },
	{ CCI_REG8(0x2b), 0x03 },
	{ CCI_REG8(0x2c), 0x77 },
	{ CCI_REG8(0x2d), 0x20 },
	{ CCI_REG8(0x30), 0x00 },
	{ CCI_REG8(0x31), 0x00 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0xa1), 0x28 },
	{ CCI_REG8(0xa2), 0x28 },
	{ CCI_REG8(0xa3), 0x28 },
	{ CCI_REG8(0xa4), 0x28 },
	{ CCI_REG8(0xa5), 0x1c },
	{ CCI_REG8(0xa6), 0x1e },
	{ CCI_REG8(0xa7), 0x20 },
	{ CCI_REG8(0xa8), 0x20 },
	{ CCI_REG8(0xa9), 0x1a },
	{ CCI_REG8(0xaa), 0x1c },
	{ CCI_REG8(0xab), 0x20 },
	{ CCI_REG8(0xac), 0x20 },
	{ CCI_REG8(0xad), 0x09 },
	{ CCI_REG8(0xae), 0x06 },
	{ CCI_REG8(0xaf), 0x09 },
	{ CCI_REG8(0xb0), 0x06 },
	{ CCI_REG8(0xb1), 0x00 },
	{ CCI_REG8(0xb2), 0x00 },
	{ CCI_REG8(0xb3), 0x00 },
	{ CCI_REG8(0xb4), 0x00 },
	{ CCI_REG8(0xb5), 0x02 },
	{ CCI_REG8(0xb6), 0x02 },
	{ CCI_REG8(0xb7), 0x02 },
	{ CCI_REG8(0xb8), 0x02 },
	{ CCI_REG8(0xfd), 0x02 },
	{ CCI_REG8(0x08), 0x00 },
	{ CCI_REG8(0x09), 0x06 },
	{ CCI_REG8(0x1d), 0x03 },
	{ CCI_REG8(0x1f), 0x05 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x32), 0x00 },
	{ CCI_REG8(0xfd), 0x02 },
	{ CCI_REG8(0x26), 0xac },
	{ CCI_REG8(0x27), 0xad },
	{ CCI_REG8(0x10), 0x00 },
	{ CCI_REG8(0x11), 0xdd },
	{ CCI_REG8(0x1b), 0x80 },
	{ CCI_REG8(0x1a), 0x80 },
	{ CCI_REG8(0x18), 0x27 },
	{ CCI_REG8(0x19), 0x26 },
	{ CCI_REG8(0x2a), 0x00 },
	{ CCI_REG8(0x2b), 0x00 },
	{ CCI_REG8(0x28), 0xf8 },
	{ CCI_REG8(0x29), 0x08 },
	{ CCI_REG8(0x66), 0x40 },
	{ CCI_REG8(0x67), 0x62 },
	{ CCI_REG8(0x68), 0xd4 },
	{ CCI_REG8(0x69), 0xf4 },
	{ CCI_REG8(0x6a), 0xa5 },
	{ CCI_REG8(0x7c), 0x20 },
	{ CCI_REG8(0x7d), 0x4b },
	{ CCI_REG8(0x7e), 0xf4 },
	{ CCI_REG8(0x7f), 0x26 },
	{ CCI_REG8(0x80), 0xa6 },
	{ CCI_REG8(0x70), 0x26 },
	{ CCI_REG8(0x71), 0x4a },
	{ CCI_REG8(0x72), 0x25 },
	{ CCI_REG8(0x73), 0x46 },
	{ CCI_REG8(0x74), 0xaa },
	{ CCI_REG8(0x6b), 0x0a },
	{ CCI_REG8(0x6c), 0x31 },
	{ CCI_REG8(0x6d), 0x2e },
	{ CCI_REG8(0x6e), 0x52 },
	{ CCI_REG8(0x6f), 0xaa },
	{ CCI_REG8(0x61), 0xfb },
	{ CCI_REG8(0x62), 0x13 },
	{ CCI_REG8(0x63), 0x48 },
	{ CCI_REG8(0x64), 0x6e },
	{ CCI_REG8(0x65), 0x6a },
	{ CCI_REG8(0x75), 0x80 },
	{ CCI_REG8(0x76), 0x09 },
	{ CCI_REG8(0x77), 0x02 },
	{ CCI_REG8(0x24), 0x25 },
	{ CCI_REG8(0x0e), 0x16 },
	{ CCI_REG8(0x3b), 0x09 },
	{ CCI_REG8(0xfd), 0x02 },
	{ CCI_REG8(0xde), 0x0f },
	{ CCI_REG8(0xd7), 0x18 },
	{ CCI_REG8(0xd8), 0x18 },
	{ CCI_REG8(0xd9), 0x10 },
	{ CCI_REG8(0xda), 0x14 },
	{ CCI_REG8(0xe8), 0x1a },
	{ CCI_REG8(0xe9), 0x18 },
	{ CCI_REG8(0xea), 0x11 },
	{ CCI_REG8(0xeb), 0x18 },
	{ CCI_REG8(0xec), 0x11 },
	{ CCI_REG8(0xed), 0x11 },
	{ CCI_REG8(0xee), 0x11 },
	{ CCI_REG8(0xef), 0x11 },
	{ CCI_REG8(0xd3), 0x08 },
	{ CCI_REG8(0xd4), 0x10 },
	{ CCI_REG8(0xd5), 0x10 },
	{ CCI_REG8(0xd6), 0x08 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0xd1), 0x20 },
	{ CCI_REG8(0xfd), 0x02 },
	{ CCI_REG8(0xdc), 0x05 },
	{ CCI_REG8(0x05), 0x20 },
	{ CCI_REG8(0xfd), 0x02 },
	{ CCI_REG8(0x81), 0x00 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0xfc), 0x00 },
	{ CCI_REG8(0x7d), 0x05 },
	{ CCI_REG8(0x7e), 0x05 },
	{ CCI_REG8(0x7f), 0x09 },
	{ CCI_REG8(0x80), 0x08 },
	{ CCI_REG8(0xfd), 0x02 },
	{ CCI_REG8(0xdd), 0x0f },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x6d), 0x08 },
	{ CCI_REG8(0x6e), 0x08 },
	{ CCI_REG8(0x6f), 0x10 },
	{ CCI_REG8(0x70), 0x18 },
	{ CCI_REG8(0x86), 0x18 },
	{ CCI_REG8(0x71), 0x10 },
	{ CCI_REG8(0x72), 0x10 },
	{ CCI_REG8(0x73), 0x10 },
	{ CCI_REG8(0x74), 0x10 },
	{ CCI_REG8(0x75), 0x10 },
	{ CCI_REG8(0x76), 0x10 },
	{ CCI_REG8(0x77), 0x10 },
	{ CCI_REG8(0x78), 0x10 },
	{ CCI_REG8(0x79), 0x35 },
	{ CCI_REG8(0x7a), 0x34 },
	{ CCI_REG8(0x7b), 0x34 },
	{ CCI_REG8(0x7c), 0x22 },
	{ CCI_REG8(0x81), 0x0d },
	{ CCI_REG8(0x82), 0x18 },
	{ CCI_REG8(0x83), 0x20 },
	{ CCI_REG8(0x84), 0x24 },
	{ CCI_REG8(0xfd), 0x02 },
	{ CCI_REG8(0x83), 0x12 },
	{ CCI_REG8(0x84), 0x14 },
	{ CCI_REG8(0x86), 0x04 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x61), 0x60 },
	{ CCI_REG8(0x62), 0x28 },
	{ CCI_REG8(0x8a), 0x10 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x8b), 0x00 },
	{ CCI_REG8(0x8c), 0x0b },
	{ CCI_REG8(0x8d), 0x18 },
	{ CCI_REG8(0x8e), 0x26 },
	{ CCI_REG8(0x8f), 0x31 },
	{ CCI_REG8(0x90), 0x47 },
	{ CCI_REG8(0x91), 0x59 },
	{ CCI_REG8(0x92), 0x66 },
	{ CCI_REG8(0x93), 0x74 },
	{ CCI_REG8(0x94), 0x84 },
	{ CCI_REG8(0x95), 0x92 },
	{ CCI_REG8(0x96), 0xa0 },
	{ CCI_REG8(0x97), 0xab },
	{ CCI_REG8(0x98), 0xb7 },
	{ CCI_REG8(0x99), 0xc0 },
	{ CCI_REG8(0x9a), 0xca },
	{ CCI_REG8(0x9b), 0xd2 },
	{ CCI_REG8(0x9c), 0xdb },
	{ CCI_REG8(0x9d), 0xe3 },
	{ CCI_REG8(0x9e), 0xeb },
	{ CCI_REG8(0x9f), 0xf5 },
	{ CCI_REG8(0xa0), 0xff },
	{ CCI_REG8(0xfd), 0x02 },
	{ CCI_REG8(0x15), 0xc0 },
	{ CCI_REG8(0x16), 0x8c },
	{ CCI_REG8(0xa0), 0xa0 },
	{ CCI_REG8(0xa1), 0x03 },
	{ CCI_REG8(0xa2), 0xdd },
	{ CCI_REG8(0xa3), 0xe2 },
	{ CCI_REG8(0xa4), 0xab },
	{ CCI_REG8(0xa5), 0xf4 },
	{ CCI_REG8(0xa6), 0xf0 },
	{ CCI_REG8(0xa7), 0xc7 },
	{ CCI_REG8(0xa8), 0xca },
	{ CCI_REG8(0xa9), 0x30 },
	{ CCI_REG8(0xaa), 0x33 },
	{ CCI_REG8(0xab), 0x0f },
	{ CCI_REG8(0xac), 0x9d },
	{ CCI_REG8(0xad), 0xf0 },
	{ CCI_REG8(0xae), 0xf3 },
	{ CCI_REG8(0xaf), 0xdd },
	{ CCI_REG8(0xb0), 0xc9 },
	{ CCI_REG8(0xb1), 0xda },
	{ CCI_REG8(0xb2), 0xed },
	{ CCI_REG8(0xb3), 0x7d },
	{ CCI_REG8(0xb4), 0x16 },
	{ CCI_REG8(0xb5), 0x3c },
	{ CCI_REG8(0xb6), 0x33 },
	{ CCI_REG8(0xb7), 0x1f },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0xd3), 0x66 },
	{ CCI_REG8(0xd4), 0x58 },
	{ CCI_REG8(0xd5), 0x4a },
	{ CCI_REG8(0xd6), 0x40 },
	{ CCI_REG8(0xd7), 0x66 },
	{ CCI_REG8(0xd8), 0x58 },
	{ CCI_REG8(0xd9), 0x4a },
	{ CCI_REG8(0xda), 0x40 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0xdd), 0x30 },
	{ CCI_REG8(0xde), 0x10 },
	{ CCI_REG8(0xdf), 0xff },
	{ CCI_REG8(0x00), 0x00 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0xc2), 0xaa },
	{ CCI_REG8(0xc3), 0x88 },
	{ CCI_REG8(0xc4), 0x77 },
	{ CCI_REG8(0xc5), 0x66 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0xcd), 0x10 },
	{ CCI_REG8(0xce), 0x1f },
	{ CCI_REG8(0xcf), 0x30 },
	{ CCI_REG8(0xd0), 0x45 },
	{ CCI_REG8(0xfd), 0x02 },
	{ CCI_REG8(0x31), 0x60 },
	{ CCI_REG8(0x32), 0x60 },
	{ CCI_REG8(0x33), 0xc0 },
	{ CCI_REG8(0x35), 0x60 },
	{ CCI_REG8(0x37), 0x13 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x0e), 0x80 },
	{ CCI_REG8(0x0f), 0x20 },
	{ CCI_REG8(0x10), 0x80 },
	{ CCI_REG8(0x11), 0x80 },
	{ CCI_REG8(0x12), 0x80 },
	{ CCI_REG8(0x13), 0x80 },
	{ CCI_REG8(0x14), 0x8d },
	{ CCI_REG8(0x15), 0x86 },
	{ CCI_REG8(0x16), 0x84 },
	{ CCI_REG8(0x17), 0x80 },
	{ CCI_REG8(0xfd), 0x00 },
	{ CCI_REG8(0x31), 0x06 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x32), 0x15 },
	{ CCI_REG8(0x33), 0xef },
	{ CCI_REG8(0x34), 0x07 },
	{ CCI_REG8(0xd2), 0x01 },
	{ CCI_REG8(0xfb), 0x25 },
	{ CCI_REG8(0xf2), 0x49 },
	{ CCI_REG8(0x35), 0x40 },
	{ CCI_REG8(0x5d), 0x11 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x36), 0x00 },
	{ CCI_REG8(0xfd), 0x00 },
	{ CCI_REG8(0x92), 0x11 },
	{ CCI_REG8(0xfd), 0x01 },
};

struct sp0a20 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *xclk;
	struct regulator *supplies[ARRAY_SIZE(sp0a20_supply_names)];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *powerdown_gpio;
	bool streaming;

	struct v4l2_ctrl_handler ctrls;
};

static inline struct sp0a20 *to_sp0a20(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sp0a20, sd);
}

/*
 * Power sequence of the vendor driver. The vendor driver does not use the
 * reset line; when one is wired it is released before the power-down pulse.
 */
static int sp0a20_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sp0a20 *sp0a20 = to_sp0a20(sd);
	int ret, i;

	gpiod_set_value_cansleep(sp0a20->reset_gpio, 1);
	gpiod_set_value_cansleep(sp0a20->powerdown_gpio, 0);
	fsleep(1000);

	/* The analogue supply is enabled before the I/O supply. */
	for (i = 0; i < ARRAY_SIZE(sp0a20->supplies); i++) {
		ret = regulator_enable(sp0a20->supplies[i]);
		if (ret) {
			dev_err(dev, "failed to enable %s: %d\n",
				sp0a20_supply_names[i], ret);
			goto err_supplies;
		}
		fsleep(5000);
	}

	ret = clk_prepare_enable(sp0a20->xclk);
	if (ret) {
		dev_err(dev, "failed to enable xclk: %d\n", ret);
		goto err_supplies;
	}
	fsleep(5000);

	gpiod_set_value_cansleep(sp0a20->reset_gpio, 0);
	gpiod_set_value_cansleep(sp0a20->powerdown_gpio, 1);
	fsleep(100 * USEC_PER_MSEC);
	gpiod_set_value_cansleep(sp0a20->powerdown_gpio, 0);
	fsleep(5000);

	return 0;

err_supplies:
	while (--i >= 0)
		regulator_disable(sp0a20->supplies[i]);
	gpiod_set_value_cansleep(sp0a20->powerdown_gpio, 1);

	return ret;
}

static int sp0a20_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sp0a20 *sp0a20 = to_sp0a20(sd);
	int i;

	gpiod_set_value_cansleep(sp0a20->powerdown_gpio, 1);
	gpiod_set_value_cansleep(sp0a20->reset_gpio, 1);
	clk_disable_unprepare(sp0a20->xclk);
	fsleep(5000);

	for (i = ARRAY_SIZE(sp0a20->supplies) - 1; i >= 0; i--) {
		regulator_disable(sp0a20->supplies[i]);
		fsleep(2000);
	}

	return 0;
}

static int sp0a20_set_white_balance(struct sp0a20 *sp0a20, int preset)
{
	const struct sp0a20_awb_preset *gains = &sp0a20_awb_presets[preset];
	bool automatic = preset == V4L2_WHITE_BALANCE_AUTO;
	u8 awb_ctrl = automatic ? SP0A20_AWB_AUTO : SP0A20_AWB_MANUAL;
	int ret = 0;

	if (!automatic) {
		cci_write(sp0a20->regmap, SP0A20_REG_PAGE, 1, &ret);
		cci_write(sp0a20->regmap, SP0A20_REG_AWB_CTRL, awb_ctrl, &ret);
	}

	cci_write(sp0a20->regmap, SP0A20_REG_PAGE, 2, &ret);
	cci_write(sp0a20->regmap, SP0A20_REG_AWB_GAIN_R, gains->gain_r, &ret);
	cci_write(sp0a20->regmap, SP0A20_REG_AWB_GAIN_B, gains->gain_b, &ret);

	if (automatic) {
		cci_write(sp0a20->regmap, SP0A20_REG_PAGE, 1, &ret);
		cci_write(sp0a20->regmap, SP0A20_REG_AWB_CTRL, awb_ctrl, &ret);
	}

	cci_write(sp0a20->regmap, SP0A20_REG_PAGE, 0, &ret);

	return ret;
}

static int sp0a20_set_effect(struct sp0a20 *sp0a20, int effect)
{
	const struct sp0a20_effect *fx = &sp0a20_effects[effect];
	int ret = 0;

	cci_write(sp0a20->regmap, SP0A20_REG_PAGE, 1, &ret);
	cci_write(sp0a20->regmap, SP0A20_REG_EFFECT_FREEZE, 0x02, &ret);
	cci_write(sp0a20->regmap, SP0A20_REG_PAGE, 0, &ret);
	if (sp0a20->streaming)
		msleep(200);
	cci_write(sp0a20->regmap, SP0A20_REG_PAGE, 1, &ret);
	cci_write(sp0a20->regmap, SP0A20_REG_EFFECT_Y, fx->y, &ret);
	cci_write(sp0a20->regmap, SP0A20_REG_EFFECT_U, fx->u, &ret);
	cci_write(sp0a20->regmap, SP0A20_REG_EFFECT_V, fx->v, &ret);
	cci_write(sp0a20->regmap, SP0A20_REG_EFFECT_FREEZE, 0x00, &ret);

	return ret;
}

static int sp0a20_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sp0a20 *sp0a20 = container_of(ctrl->handler, struct sp0a20, ctrls);
	struct device *dev = sp0a20->sd.dev;
	int ret = 0;

	if (!pm_runtime_get_if_in_use(dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE:
		ret = sp0a20_set_white_balance(sp0a20, ctrl->val);
		break;
	case V4L2_CID_COLORFX:
		ret = sp0a20_set_effect(sp0a20, ctrl->val);
		break;
	case V4L2_CID_AUTO_EXPOSURE_BIAS:
		cci_write(sp0a20->regmap, SP0A20_REG_PAGE, 1, &ret);
		cci_write(sp0a20->regmap, SP0A20_REG_EV_BIAS,
			  sp0a20_ev_bias_regs[ctrl->val], &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(dev);

	return ret;
}

static const struct v4l2_ctrl_ops sp0a20_ctrl_ops = {
	.s_ctrl = sp0a20_s_ctrl,
};

static int sp0a20_init_controls(struct sp0a20 *sp0a20)
{
	struct v4l2_ctrl_handler *hdl = &sp0a20->ctrls;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	v4l2_ctrl_handler_init(hdl, 9);

	v4l2_ctrl_new_std_menu(hdl, &sp0a20_ctrl_ops,
			       V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE,
			       V4L2_WHITE_BALANCE_SHADE,
			       ~SP0A20_AWB_SUPPORTED_MASK &
			       (BIT(V4L2_WHITE_BALANCE_SHADE + 1) - 1),
			       V4L2_WHITE_BALANCE_AUTO);

	v4l2_ctrl_new_std_menu(hdl, &sp0a20_ctrl_ops, V4L2_CID_COLORFX,
			       V4L2_COLORFX_GRASS_GREEN,
			       ~SP0A20_COLORFX_SUPPORTED_MASK &
			       (BIT(V4L2_COLORFX_GRASS_GREEN + 1) - 1),
			       V4L2_COLORFX_NONE);

	v4l2_ctrl_new_int_menu(hdl, &sp0a20_ctrl_ops,
			       V4L2_CID_AUTO_EXPOSURE_BIAS,
			       ARRAY_SIZE(sp0a20_ev_bias_menu) - 1,
			       ARRAY_SIZE(sp0a20_ev_bias_menu) / 2,
			       sp0a20_ev_bias_menu);

	ctrl = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_PIXEL_RATE,
				 SP0A20_PIXEL_RATE, SP0A20_PIXEL_RATE, 1,
				 SP0A20_PIXEL_RATE);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_int_menu(hdl, NULL, V4L2_CID_LINK_FREQ,
				      ARRAY_SIZE(sp0a20_link_freq_menu) - 1, 0,
				      sp0a20_link_freq_menu);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_HBLANK, SP0A20_HBLANK,
				 SP0A20_HBLANK, 1, SP0A20_HBLANK);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_VBLANK, SP0A20_VBLANK,
				 SP0A20_VBLANK, 1, SP0A20_VBLANK);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (hdl->error) {
		ret = hdl->error;
		goto err_free;
	}

	ret = v4l2_fwnode_device_parse(sp0a20->sd.dev, &props);
	if (ret)
		goto err_free;

	ret = v4l2_ctrl_new_fwnode_properties(hdl, &sp0a20_ctrl_ops, &props);
	if (ret)
		goto err_free;

	sp0a20->sd.ctrl_handler = hdl;

	return 0;

err_free:
	v4l2_ctrl_handler_free(hdl);

	return ret;
}

/*
 * MIPI CSI-2 carries YUV 4:2:2 8-bit as U Y V Y in memory order, which the
 * bus format API describes as UYVY8_1X16 (one 16-bit pixel per sample); the
 * _2X8 variant is for 8-bit parallel buses.
 */
#define SP0A20_MBUS_CODE	MEDIA_BUS_FMT_UYVY8_1X16

static void sp0a20_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = SP0A20_WIDTH;
	fmt->height = SP0A20_HEIGHT;
	fmt->code = SP0A20_MBUS_CODE;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_SRGB;
}

static int sp0a20_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	sp0a20_fill_format(v4l2_subdev_state_get_format(state, 0));

	return 0;
}

static int sp0a20_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	/* A single mode is supported, the requested format is ignored. */
	sp0a20_fill_format(&fmt->format);
	*v4l2_subdev_state_get_format(state, fmt->pad) = fmt->format;

	return 0;
}

static int sp0a20_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = SP0A20_MBUS_CODE;

	return 0;
}

static int sp0a20_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != SP0A20_MBUS_CODE)
		return -EINVAL;

	fse->min_width = SP0A20_WIDTH;
	fse->max_width = SP0A20_WIDTH;
	fse->min_height = SP0A20_HEIGHT;
	fse->max_height = SP0A20_HEIGHT;

	return 0;
}

static int sp0a20_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = SP0A20_WIDTH;
		sel->r.height = SP0A20_HEIGHT;
		return 0;
	}

	return -EINVAL;
}

static int sp0a20_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct sp0a20 *sp0a20 = to_sp0a20(sd);
	struct device *dev = sp0a20->sd.dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	ret = cci_multi_reg_write(sp0a20->regmap, sp0a20_init_regs,
				  ARRAY_SIZE(sp0a20_init_regs), NULL);
	if (ret) {
		dev_err(dev, "failed to write the init sequence: %d\n", ret);
		goto err_put;
	}

	/* The sensor streams as soon as the init sequence is written. */
	ret = __v4l2_ctrl_handler_setup(&sp0a20->ctrls);
	if (ret)
		goto err_put;

	sp0a20->streaming = true;

	return 0;

err_put:
	pm_runtime_put_sync_suspend(dev);

	return ret;
}

static int sp0a20_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct sp0a20 *sp0a20 = to_sp0a20(sd);

	/* No stream control register is known: cutting the power stops the output. */
	sp0a20->streaming = false;
	pm_runtime_put_sync_suspend(sd->dev);

	return 0;
}

static const struct v4l2_subdev_video_ops sp0a20_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops sp0a20_pad_ops = {
	.enum_mbus_code = sp0a20_enum_mbus_code,
	.enum_frame_size = sp0a20_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = sp0a20_set_fmt,
	.get_selection = sp0a20_get_selection,
	.enable_streams = sp0a20_enable_streams,
	.disable_streams = sp0a20_disable_streams,
};

static const struct v4l2_subdev_ops sp0a20_subdev_ops = {
	.video = &sp0a20_video_ops,
	.pad = &sp0a20_pad_ops,
};

static const struct v4l2_subdev_internal_ops sp0a20_internal_ops = {
	.init_state = sp0a20_init_state,
};

static int sp0a20_identify(struct sp0a20 *sp0a20)
{
	struct device *dev = sp0a20->sd.dev;
	u64 id = 0;
	int ret = 0;

	cci_write(sp0a20->regmap, SP0A20_REG_PAGE, 0, &ret);
	cci_read(sp0a20->regmap, SP0A20_REG_CHIP_ID, &id, &ret);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read the chip id\n");

	if (id != SP0A20_CHIP_ID)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected chip id 0x%02llx\n", id);

	return 0;
}

static int sp0a20_check_hwcfg(struct sp0a20 *sp0a20)
{
	struct device *dev = sp0a20->sd.dev;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *ep;
	unsigned long freq_bitmap;
	u32 xclk_freq;
	int ret;

	sp0a20->xclk = devm_clk_get_optional(dev, "xclk");
	if (IS_ERR(sp0a20->xclk))
		return dev_err_probe(dev, PTR_ERR(sp0a20->xclk),
				     "failed to get xclk\n");

	ret = device_property_read_u32(dev, "clock-frequency", &xclk_freq);
	if (ret) {
		if (!sp0a20->xclk)
			return dev_err_probe(dev, ret,
					     "missing clock-frequency\n");
		xclk_freq = clk_get_rate(sp0a20->xclk);
	}
	if (xclk_freq != SP0A20_XCLK_FREQ)
		return dev_err_probe(dev, -EINVAL,
				     "unsupported external clock %u Hz\n",
				     xclk_freq);

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0, 0);
	if (!ep)
		return dev_err_probe(dev, -ENXIO, "missing endpoint\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(dev, ret, "failed to parse endpoint\n");

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != 1) {
		ret = dev_err_probe(dev, -EINVAL,
				    "%u data lanes not supported, need 1\n",
				    bus_cfg.bus.mipi_csi2.num_data_lanes);
		goto out_free;
	}

	ret = v4l2_link_freq_to_bitmap(dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       sp0a20_link_freq_menu,
				       ARRAY_SIZE(sp0a20_link_freq_menu),
				       &freq_bitmap);
	if (ret)
		ret = dev_err_probe(dev, ret,
				    "link-frequencies must include %lld Hz\n",
				    sp0a20_link_freq_menu[0]);

out_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int sp0a20_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sp0a20 *sp0a20;
	int ret, i;

	sp0a20 = devm_kzalloc(dev, sizeof(*sp0a20), GFP_KERNEL);
	if (!sp0a20)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&sp0a20->sd, client, &sp0a20_subdev_ops);

	ret = sp0a20_check_hwcfg(sp0a20);
	if (ret)
		return ret;

	sp0a20->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(sp0a20->regmap))
		return dev_err_probe(dev, PTR_ERR(sp0a20->regmap),
				     "failed to init the register map\n");

	/* The supplies are sequenced one by one, bulk enabling is not ordered. */
	for (i = 0; i < ARRAY_SIZE(sp0a20->supplies); i++) {
		sp0a20->supplies[i] = devm_regulator_get(dev,
							 sp0a20_supply_names[i]);
		if (IS_ERR(sp0a20->supplies[i]))
			return dev_err_probe(dev, PTR_ERR(sp0a20->supplies[i]),
					     "failed to get %s supply\n",
					     sp0a20_supply_names[i]);
	}

	sp0a20->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sp0a20->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sp0a20->reset_gpio),
				     "failed to get reset gpio\n");

	sp0a20->powerdown_gpio = devm_gpiod_get(dev, "powerdown",
						GPIOD_OUT_HIGH);
	if (IS_ERR(sp0a20->powerdown_gpio))
		return dev_err_probe(dev, PTR_ERR(sp0a20->powerdown_gpio),
				     "failed to get powerdown gpio\n");

	ret = sp0a20_power_on(dev);
	if (ret)
		return ret;

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);

	ret = sp0a20_identify(sp0a20);
	if (ret)
		goto err_pm;

	ret = sp0a20_init_controls(sp0a20);
	if (ret)
		goto err_pm;

	sp0a20->sd.internal_ops = &sp0a20_internal_ops;
	sp0a20->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sp0a20->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sp0a20->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sp0a20->sd.entity, 1, &sp0a20->pad);
	if (ret)
		goto err_ctrls;

	sp0a20->sd.state_lock = sp0a20->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&sp0a20->sd);
	if (ret)
		goto err_entity;

	ret = v4l2_async_register_subdev_sensor(&sp0a20->sd);
	if (ret)
		goto err_subdev;

	pm_runtime_put_sync_suspend(dev);

	return 0;

err_subdev:
	v4l2_subdev_cleanup(&sp0a20->sd);
err_entity:
	media_entity_cleanup(&sp0a20->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&sp0a20->ctrls);
err_pm:
	pm_runtime_put_noidle(dev);
	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		sp0a20_power_off(dev);
	pm_runtime_set_suspended(dev);

	return ret;
}

static void sp0a20_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sp0a20 *sp0a20 = to_sp0a20(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sp0a20->ctrls);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		sp0a20_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(sp0a20_pm_ops, sp0a20_power_off,
				 sp0a20_power_on, NULL);

static const struct of_device_id sp0a20_of_match[] = {
	{ .compatible = "superpix,sp0a20" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sp0a20_of_match);

static struct i2c_driver sp0a20_driver = {
	.driver = {
		.name = "sp0a20",
		.of_match_table = sp0a20_of_match,
		.pm = pm_ptr(&sp0a20_pm_ops),
	},
	.probe = sp0a20_probe,
	.remove = sp0a20_remove,
};
module_i2c_driver(sp0a20_driver);

MODULE_DESCRIPTION("SuperPix SP0A20 camera sensor driver");
MODULE_AUTHOR("Marc Lainez <marc.lainez@gmail.com>");
MODULE_LICENSE("GPL");
