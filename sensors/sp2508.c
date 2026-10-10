// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the SuperPix SP2508 raw camera sensor (MIPI CSI-2, one lane).
 *
 * The register values, blanking and gain limits come from the vendor driver
 * of the Lenovo Tab3 7 Essential (MT8127).
 *
 * Copyright (c) 2026 Marc Lainez <marc.lainez@gmail.com>
 */

#include <linux/array_size.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/math.h>
#include <linux/minmax.h>
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

/* Every register lives in a page selected through SP2508_REG_PAGE. */
#define SP2508_REG_PAGE			CCI_REG8(0xfd)

/* Page 0 */
#define SP2508_REG_CHIP_ID_H		CCI_REG8(0x02)
#define SP2508_REG_CHIP_ID_L		CCI_REG8(0x03)
#define SP2508_CHIP_ID			0x2508

/* Page 1 */
#define SP2508_REG_UPDATE		CCI_REG8(0x01)	/* latches the shadowed registers */
#define SP2508_REG_EXPOSURE_H		CCI_REG8(0x03)
#define SP2508_REG_EXPOSURE_L		CCI_REG8(0x04)
#define SP2508_REG_VBLANK_L		CCI_REG8(0x06)
#define SP2508_REG_HBLANK_H		CCI_REG8(0x09)
#define SP2508_REG_HBLANK_L		CCI_REG8(0x0a)
#define SP2508_REG_TEST_PATTERN		CCI_REG8(0x0d)
#define SP2508_REG_ANALOGUE_GAIN	CCI_REG8(0x24)
#define SP2508_REG_TP_BIAS		CCI_REG8(0xfb)
#define SP2508_REG_MIPI_UPDATE		CCI_REG8(0xac)

#define SP2508_TP_BIAS_NORMAL		0x25
#define SP2508_TP_BIAS_PATTERN		0x00

#define SP2508_XCLK_FREQ		(24 * HZ_PER_MHZ)

/*
 * The sensor delivers 1616x1216 pixels on the bus, of which 1600x1200 starting
 * at (2, 2) are the image area used by the vendor driver.
 */
#define SP2508_WIDTH			1616
#define SP2508_HEIGHT			1216

/*
 * The horizontal timing counts in units of one sensor clock, which carries
 * two pixels: the vendor line length is 1160 clocks (30 MHz). The vendor frame
 * length is 1241 lines.
 */
#define SP2508_PIXELS_PER_CLK		2
#define SP2508_PIXEL_RATE		(60 * HZ_PER_MHZ)
#define SP2508_HBLANK_OFFSET_CLK	32
#define SP2508_HBLANK_REG_MAX		0x03ff
#define SP2508_HBLANK_DEFAULT		\
	(SP2508_PIXELS_PER_CLK * (0x140 + SP2508_HBLANK_OFFSET_CLK))
#define SP2508_HBLANK_MAX		\
	(SP2508_PIXELS_PER_CLK * (SP2508_HBLANK_REG_MAX + SP2508_HBLANK_OFFSET_CLK))

/* VBLANK register = vertical blanking lines + SP2508_VBLANK_OFFSET */
#define SP2508_VBLANK_OFFSET		44
#define SP2508_VBLANK_DEFAULT		(1241 - SP2508_HEIGHT)
#define SP2508_VBLANK_MAX		(0xff - SP2508_VBLANK_OFFSET)

#define SP2508_EXPOSURE_MIN		7
#define SP2508_EXPOSURE_MARGIN		5
#define SP2508_EXPOSURE_DEFAULT		777

/* Analogue gain code: 0x10 is 1x, the code is in units of 1/16. */
#define SP2508_GAIN_MIN			0x10
#define SP2508_GAIN_MAX			0x90
#define SP2508_GAIN_STEP		1
#define SP2508_GAIN_DEFAULT		SP2508_GAIN_MIN

/*
 * Link frequency for one lane of RAW10 at SP2508_PIXEL_RATE:
 * 60 Mpixel/s * 10 bit = 600 Mbit/s, DDR clock 300 MHz.
 */
static const s64 sp2508_link_freq_menu[] = {
	300 * HZ_PER_MHZ,
};

static const char * const sp2508_supply_names[] = {
	"avdd",
	"dovdd",
};

static const char * const sp2508_test_pattern_menu[] = {
	"Disabled",
	"Color Bars",
};

static const struct cci_reg_sequence sp2508_init_regs[] = {
	{ CCI_REG8(0xfd), 0x00 },
	{ CCI_REG8(0x1c), 0x03 },
	{ CCI_REG8(0x35), 0x20 }, /* pll bias */
	{ CCI_REG8(0x2f), 0x08 }, /* pll clk 60M */
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x03), 0x03 }, /* exp time, 3 base */
	{ CCI_REG8(0x04), 0x09 },
	{ CCI_REG8(0x06), 0x45 }, /* vblank  txr 0x10 */
	{ CCI_REG8(0x24), 0xa0 }, /* pga gain 6x */
	{ CCI_REG8(0x01), 0x01 }, /* enable reg write */
	{ CCI_REG8(0x2b), 0xc4 }, /* readout vref */
	{ CCI_REG8(0x2e), 0x20 }, /* dclk delay */
	{ CCI_REG8(0x79), 0x42 }, /* p39 p40 */
	{ CCI_REG8(0x85), 0x0f }, /* p51 */
	{ CCI_REG8(0x09), 0x01 }, /* hblank */
	{ CCI_REG8(0x0a), 0x40 },
	{ CCI_REG8(0x21), 0xef }, /* pcp tx 4.05v */
	{ CCI_REG8(0x25), 0xf2 }, /* reg dac 2.7v, enable bl_en,vbl 1.4v */
	{ CCI_REG8(0x26), 0x00 }, /* vref2 1v, disable ramp driver */
	{ CCI_REG8(0x2a), 0xea }, /* bypass dac res, adc range 0.745, vreg counter 0.9 */
	{ CCI_REG8(0x2c), 0xf0 }, /* high 8bit, pldo 2.7v */
	{ CCI_REG8(0x8a), 0x55 }, /* pixel bias 1uA */
	{ CCI_REG8(0x8b), 0x55 },
	{ CCI_REG8(0x19), 0xf3 }, /* icom1 1.7u, icom2 0.6u */
	{ CCI_REG8(0x11), 0x30 }, /* rst num */
	{ CCI_REG8(0xd0), 0x01 }, /* disable boost */
	{ CCI_REG8(0xd1), 0x01 },
	{ CCI_REG8(0xd2), 0xd0 },
	{ CCI_REG8(0x55), 0x10 },
	{ CCI_REG8(0x58), 0x30 },
	{ CCI_REG8(0x5d), 0x15 },
	{ CCI_REG8(0x5e), 0x05 },
	{ CCI_REG8(0x64), 0x40 },
	{ CCI_REG8(0x65), 0x00 },
	{ CCI_REG8(0x66), 0x66 },
	{ CCI_REG8(0x67), 0x00 },
	{ CCI_REG8(0x68), 0x68 },
	{ CCI_REG8(0x72), 0x70 },
	{ CCI_REG8(0xfb), 0x25 },
	{ CCI_REG8(0xf0), 0x00 }, /* offset */
	{ CCI_REG8(0xf1), 0x00 },
	{ CCI_REG8(0xf2), 0x00 },
	{ CCI_REG8(0xf3), 0x00 },
	{ CCI_REG8(0xfd), 0x02 }, /* raw data digital gain */
	{ CCI_REG8(0x00), 0xb0 },
	{ CCI_REG8(0x01), 0xb0 },
	{ CCI_REG8(0x03), 0xb0 },
	{ CCI_REG8(0x04), 0xb0 },
	{ CCI_REG8(0xfd), 0x01 }, /* mipi */
	{ CCI_REG8(0xb3), 0x00 },
	{ CCI_REG8(0x93), 0x01 },
	{ CCI_REG8(0x9d), 0x17 },
	{ CCI_REG8(0xc5), 0x01 },
	{ CCI_REG8(0xc6), 0x00 },
	{ CCI_REG8(0xb1), 0x01 },
	{ CCI_REG8(0x8e), 0x06 },
	{ CCI_REG8(0x8f), 0x50 },
	{ CCI_REG8(0x90), 0x04 },
	{ CCI_REG8(0x91), 0xc0 },
	{ CCI_REG8(0x92), 0x01 },
	{ CCI_REG8(0xa1), 0x05 },
	{ CCI_REG8(0xaa), 0x00 },
	{ CCI_REG8(0xac), 0x01 },
};

struct sp2508 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *xclk;
	struct regulator *supplies[ARRAY_SIZE(sp2508_supply_names)];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *powerdown_gpio;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
};

static inline struct sp2508 *to_sp2508(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sp2508, sd);
}

/* Power sequence of the vendor driver. */
static int sp2508_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sp2508 *sp2508 = to_sp2508(sd);
	int ret, i;

	gpiod_set_value_cansleep(sp2508->powerdown_gpio, 0);
	fsleep(1000);
	gpiod_set_value_cansleep(sp2508->reset_gpio, 1);
	fsleep(5000);

	/* The analogue supply is enabled before the I/O supply. */
	for (i = 0; i < ARRAY_SIZE(sp2508->supplies); i++) {
		ret = regulator_enable(sp2508->supplies[i]);
		if (ret) {
			dev_err(dev, "failed to enable %s: %d\n",
				sp2508_supply_names[i], ret);
			goto err_supplies;
		}
		fsleep(5000);
	}

	ret = clk_prepare_enable(sp2508->xclk);
	if (ret) {
		dev_err(dev, "failed to enable xclk: %d\n", ret);
		goto err_supplies;
	}
	fsleep(2000);

	/* One power-down pulse, then the sensor is taken out of reset. */
	gpiod_set_value_cansleep(sp2508->powerdown_gpio, 1);
	fsleep(100 * USEC_PER_MSEC);
	gpiod_set_value_cansleep(sp2508->powerdown_gpio, 0);
	fsleep(20 * USEC_PER_MSEC);
	gpiod_set_value_cansleep(sp2508->reset_gpio, 0);
	fsleep(5000);

	return 0;

err_supplies:
	while (--i >= 0)
		regulator_disable(sp2508->supplies[i]);
	gpiod_set_value_cansleep(sp2508->reset_gpio, 1);

	return ret;
}

static int sp2508_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sp2508 *sp2508 = to_sp2508(sd);
	int i;

	clk_disable_unprepare(sp2508->xclk);
	fsleep(5000);
	gpiod_set_value_cansleep(sp2508->reset_gpio, 1);
	fsleep(2000);
	gpiod_set_value_cansleep(sp2508->powerdown_gpio, 0);
	fsleep(2000);

	for (i = ARRAY_SIZE(sp2508->supplies) - 1; i >= 0; i--) {
		regulator_disable(sp2508->supplies[i]);
		fsleep(2000);
	}

	return 0;
}

static int sp2508_write_ctrl(struct sp2508 *sp2508, u32 reg, u64 val, int *err)
{
	cci_write(sp2508->regmap, SP2508_REG_PAGE, 1, err);
	cci_write(sp2508->regmap, reg, val, err);
	cci_write(sp2508->regmap, SP2508_REG_UPDATE, 1, err);

	return *err;
}

static int sp2508_set_exposure(struct sp2508 *sp2508, u32 lines)
{
	int ret = 0;

	cci_write(sp2508->regmap, SP2508_REG_PAGE, 1, &ret);
	cci_write(sp2508->regmap, SP2508_REG_EXPOSURE_H, lines >> 8, &ret);
	cci_write(sp2508->regmap, SP2508_REG_EXPOSURE_L, lines & 0xff, &ret);
	cci_write(sp2508->regmap, SP2508_REG_UPDATE, 1, &ret);

	return ret;
}

static int sp2508_set_hblank(struct sp2508 *sp2508, u32 hblank)
{
	u32 val = hblank / SP2508_PIXELS_PER_CLK - SP2508_HBLANK_OFFSET_CLK;
	int ret = 0;

	cci_write(sp2508->regmap, SP2508_REG_PAGE, 1, &ret);
	cci_write(sp2508->regmap, SP2508_REG_HBLANK_H, val >> 8, &ret);
	cci_write(sp2508->regmap, SP2508_REG_HBLANK_L, val & 0xff, &ret);
	cci_write(sp2508->regmap, SP2508_REG_UPDATE, 1, &ret);

	return ret;
}

static int sp2508_set_test_pattern(struct sp2508 *sp2508, int index)
{
	int ret = 0;

	cci_write(sp2508->regmap, SP2508_REG_PAGE, 1, &ret);
	cci_write(sp2508->regmap, SP2508_REG_TP_BIAS,
		  index ? SP2508_TP_BIAS_PATTERN : SP2508_TP_BIAS_NORMAL, &ret);
	cci_write(sp2508->regmap, SP2508_REG_MIPI_UPDATE, 1, &ret);
	cci_write(sp2508->regmap, SP2508_REG_TEST_PATTERN, index, &ret);

	return ret;
}

static int sp2508_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sp2508 *sp2508 = container_of(ctrl->handler, struct sp2508, ctrls);
	struct device *dev = sp2508->sd.dev;
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 exp_max = SP2508_HEIGHT + ctrl->val - SP2508_EXPOSURE_MARGIN;

		ret = __v4l2_ctrl_modify_range(sp2508->exposure,
					       sp2508->exposure->minimum,
					       exp_max, sp2508->exposure->step,
					       min_t(s64, sp2508->exposure->default_value,
						     exp_max));
		if (ret)
			return ret;
	}

	if (!pm_runtime_get_if_in_use(dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = sp2508_set_exposure(sp2508, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		sp2508_write_ctrl(sp2508, SP2508_REG_ANALOGUE_GAIN, ctrl->val,
				  &ret);
		break;
	case V4L2_CID_HBLANK:
		ret = sp2508_set_hblank(sp2508, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		sp2508_write_ctrl(sp2508, SP2508_REG_VBLANK_L,
				  ctrl->val + SP2508_VBLANK_OFFSET, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = sp2508_set_test_pattern(sp2508, ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(dev);

	return ret;
}

static const struct v4l2_ctrl_ops sp2508_ctrl_ops = {
	.s_ctrl = sp2508_s_ctrl,
};

static int sp2508_init_controls(struct sp2508 *sp2508)
{
	struct v4l2_ctrl_handler *hdl = &sp2508->ctrls;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	v4l2_ctrl_handler_init(hdl, 9);

	sp2508->exposure = v4l2_ctrl_new_std(hdl, &sp2508_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     SP2508_EXPOSURE_MIN,
					     SP2508_HEIGHT + SP2508_VBLANK_DEFAULT -
					     SP2508_EXPOSURE_MARGIN, 1,
					     SP2508_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std(hdl, &sp2508_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  SP2508_GAIN_MIN, SP2508_GAIN_MAX, SP2508_GAIN_STEP,
			  SP2508_GAIN_DEFAULT);

	sp2508->hblank = v4l2_ctrl_new_std(hdl, &sp2508_ctrl_ops,
					   V4L2_CID_HBLANK,
					   SP2508_HBLANK_DEFAULT,
					   SP2508_HBLANK_MAX,
					   SP2508_PIXELS_PER_CLK,
					   SP2508_HBLANK_DEFAULT);

	sp2508->vblank = v4l2_ctrl_new_std(hdl, &sp2508_ctrl_ops,
					   V4L2_CID_VBLANK,
					   SP2508_VBLANK_DEFAULT,
					   SP2508_VBLANK_MAX, 1,
					   SP2508_VBLANK_DEFAULT);

	ctrl = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_PIXEL_RATE,
				 SP2508_PIXEL_RATE, SP2508_PIXEL_RATE, 1,
				 SP2508_PIXEL_RATE);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_int_menu(hdl, NULL, V4L2_CID_LINK_FREQ,
				      ARRAY_SIZE(sp2508_link_freq_menu) - 1, 0,
				      sp2508_link_freq_menu);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std_menu_items(hdl, &sp2508_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(sp2508_test_pattern_menu) - 1,
				     0, 0, sp2508_test_pattern_menu);

	if (hdl->error) {
		ret = hdl->error;
		goto err_free;
	}

	ret = v4l2_fwnode_device_parse(sp2508->sd.dev, &props);
	if (ret)
		goto err_free;

	ret = v4l2_ctrl_new_fwnode_properties(hdl, &sp2508_ctrl_ops, &props);
	if (ret)
		goto err_free;

	sp2508->sd.ctrl_handler = hdl;

	return 0;

err_free:
	v4l2_ctrl_handler_free(hdl);

	return ret;
}

static void sp2508_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = SP2508_WIDTH;
	fmt->height = SP2508_HEIGHT;
	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int sp2508_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	sp2508_fill_format(v4l2_subdev_state_get_format(state, 0));

	return 0;
}

static int sp2508_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	/* A single mode is supported, the requested format is ignored. */
	sp2508_fill_format(&fmt->format);
	*v4l2_subdev_state_get_format(state, fmt->pad) = fmt->format;

	return 0;
}

static int sp2508_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;

	return 0;
}

static int sp2508_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;

	fse->min_width = SP2508_WIDTH;
	fse->max_width = SP2508_WIDTH;
	fse->min_height = SP2508_HEIGHT;
	fse->max_height = SP2508_HEIGHT;

	return 0;
}

static int sp2508_get_selection(struct v4l2_subdev *sd,
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
		sel->r.width = SP2508_WIDTH;
		sel->r.height = SP2508_HEIGHT;
		return 0;
	}

	return -EINVAL;
}

static int sp2508_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct sp2508 *sp2508 = to_sp2508(sd);
	struct device *dev = sp2508->sd.dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	ret = cci_multi_reg_write(sp2508->regmap, sp2508_init_regs,
				  ARRAY_SIZE(sp2508_init_regs), NULL);
	if (ret) {
		dev_err(dev, "failed to write the init sequence: %d\n", ret);
		goto err_put;
	}

	/* The sensor streams as soon as the init sequence is written. */
	ret = __v4l2_ctrl_handler_setup(&sp2508->ctrls);
	if (ret)
		goto err_put;

	return 0;

err_put:
	pm_runtime_put_sync_suspend(dev);

	return ret;
}

static int sp2508_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	/* No stream control register is known: cutting the power stops the output. */
	pm_runtime_put_sync_suspend(sd->dev);

	return 0;
}

static const struct v4l2_subdev_video_ops sp2508_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops sp2508_pad_ops = {
	.enum_mbus_code = sp2508_enum_mbus_code,
	.enum_frame_size = sp2508_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = sp2508_set_fmt,
	.get_selection = sp2508_get_selection,
	.enable_streams = sp2508_enable_streams,
	.disable_streams = sp2508_disable_streams,
};

static const struct v4l2_subdev_ops sp2508_subdev_ops = {
	.video = &sp2508_video_ops,
	.pad = &sp2508_pad_ops,
};

static const struct v4l2_subdev_internal_ops sp2508_internal_ops = {
	.init_state = sp2508_init_state,
};

static int sp2508_identify(struct sp2508 *sp2508)
{
	struct device *dev = sp2508->sd.dev;
	u64 hi = 0, lo = 0;
	int ret = 0;

	cci_write(sp2508->regmap, SP2508_REG_PAGE, 0, &ret);
	cci_read(sp2508->regmap, SP2508_REG_CHIP_ID_H, &hi, &ret);
	cci_read(sp2508->regmap, SP2508_REG_CHIP_ID_L, &lo, &ret);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read the chip id\n");

	if (((hi << 8) | lo) != SP2508_CHIP_ID)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected chip id 0x%04llx\n",
				     (hi << 8) | lo);

	return 0;
}

static int sp2508_check_hwcfg(struct sp2508 *sp2508)
{
	struct device *dev = sp2508->sd.dev;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *ep;
	unsigned long freq_bitmap;
	u32 xclk_freq;
	int ret;

	sp2508->xclk = devm_clk_get_optional(dev, "xclk");
	if (IS_ERR(sp2508->xclk))
		return dev_err_probe(dev, PTR_ERR(sp2508->xclk),
				     "failed to get xclk\n");

	ret = device_property_read_u32(dev, "clock-frequency", &xclk_freq);
	if (ret) {
		if (!sp2508->xclk)
			return dev_err_probe(dev, ret,
					     "missing clock-frequency\n");
		xclk_freq = clk_get_rate(sp2508->xclk);
	}
	if (xclk_freq != SP2508_XCLK_FREQ)
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
				       sp2508_link_freq_menu,
				       ARRAY_SIZE(sp2508_link_freq_menu),
				       &freq_bitmap);
	if (ret)
		ret = dev_err_probe(dev, ret,
				    "link-frequencies must include %lld Hz\n",
				    sp2508_link_freq_menu[0]);

out_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int sp2508_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sp2508 *sp2508;
	int ret, i;

	sp2508 = devm_kzalloc(dev, sizeof(*sp2508), GFP_KERNEL);
	if (!sp2508)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&sp2508->sd, client, &sp2508_subdev_ops);

	ret = sp2508_check_hwcfg(sp2508);
	if (ret)
		return ret;

	sp2508->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(sp2508->regmap))
		return dev_err_probe(dev, PTR_ERR(sp2508->regmap),
				     "failed to init the register map\n");

	/* The supplies are sequenced one by one, bulk enabling is not ordered. */
	for (i = 0; i < ARRAY_SIZE(sp2508->supplies); i++) {
		sp2508->supplies[i] = devm_regulator_get(dev,
							 sp2508_supply_names[i]);
		if (IS_ERR(sp2508->supplies[i]))
			return dev_err_probe(dev, PTR_ERR(sp2508->supplies[i]),
					     "failed to get %s supply\n",
					     sp2508_supply_names[i]);
	}

	sp2508->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sp2508->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sp2508->reset_gpio),
				     "failed to get reset gpio\n");

	sp2508->powerdown_gpio = devm_gpiod_get(dev, "powerdown", GPIOD_OUT_LOW);
	if (IS_ERR(sp2508->powerdown_gpio))
		return dev_err_probe(dev, PTR_ERR(sp2508->powerdown_gpio),
				     "failed to get powerdown gpio\n");

	ret = sp2508_power_on(dev);
	if (ret)
		return ret;

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);

	ret = sp2508_identify(sp2508);
	if (ret)
		goto err_pm;

	ret = sp2508_init_controls(sp2508);
	if (ret)
		goto err_pm;

	sp2508->sd.internal_ops = &sp2508_internal_ops;
	sp2508->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sp2508->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sp2508->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sp2508->sd.entity, 1, &sp2508->pad);
	if (ret)
		goto err_ctrls;

	sp2508->sd.state_lock = sp2508->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&sp2508->sd);
	if (ret)
		goto err_entity;

	ret = v4l2_async_register_subdev_sensor(&sp2508->sd);
	if (ret)
		goto err_subdev;

	pm_runtime_put_sync_suspend(dev);

	return 0;

err_subdev:
	v4l2_subdev_cleanup(&sp2508->sd);
err_entity:
	media_entity_cleanup(&sp2508->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&sp2508->ctrls);
err_pm:
	pm_runtime_put_noidle(dev);
	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		sp2508_power_off(dev);
	pm_runtime_set_suspended(dev);

	return ret;
}

static void sp2508_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sp2508 *sp2508 = to_sp2508(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sp2508->ctrls);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		sp2508_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(sp2508_pm_ops, sp2508_power_off,
				 sp2508_power_on, NULL);

static const struct of_device_id sp2508_of_match[] = {
	{ .compatible = "superpix,sp2508" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sp2508_of_match);

static struct i2c_driver sp2508_driver = {
	.driver = {
		.name = "sp2508",
		.of_match_table = sp2508_of_match,
		.pm = pm_ptr(&sp2508_pm_ops),
	},
	.probe = sp2508_probe,
	.remove = sp2508_remove,
};
module_i2c_driver(sp2508_driver);

MODULE_DESCRIPTION("SuperPix SP2508 camera sensor driver");
MODULE_AUTHOR("Marc Lainez <marc.lainez@gmail.com>");
MODULE_LICENSE("GPL");
