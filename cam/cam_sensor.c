// SPDX-License-Identifier: GPL-2.0
/* Sensor access over raw I2C: identification and the stock register programming of the two fitted sensors. */
#include <linux/delay.h>
#include <linux/string.h>

#include "cam_proto.h"
#include "sensor_tables.h"

#define SP_PAGE_REG		0xfd
#define SP2508_ID		0x2508
#define SP0A20_ID		0x2b
#define SP2508_FLIP_REG		0x3f

/* SP2508 at I2C0 0x3c: 1600x1200 RAW10, 1 lane. Grab start and settle values from SP2508MIPIGetInfo(). */
const struct cam_sensor cam_sensor_sp2508 = {
	.name = "sp2508",
	.slot = CAM_SLOT_MAIN,
	.i2c_addr = 0x3c,
	.yuv = false,
	.lanes = 1,
	.width = 1600,
	.height = 1200,
	.grab_x = 2,
	.grab_y = 2,
	.tg_pad = 0,
	.tg_src = 3,
	.tg_fmt = 1,
	.pixid = 0,	/* SENSOR_OUTPUT_FORMAT_RAW_B */
	.vf_delay = 0,
	.ecc_order = 1,
	.vsync_pol = 0,	/* SENSOR_CLOCK_POLARITY_HIGH */
	.sync_a = 1,	/* SensorHsyncPolarity = LOW */
	.sync_b = 0,	/* SensorVsyncPolarity = HIGH */
};

/* SP0A20 at I2C0 0x21: UYVY 632x474 (640-8 x 480-6 in the stock header). */
const struct cam_sensor cam_sensor_sp0a20 = {
	.name = "sp0a20",
	.slot = CAM_SLOT_SUB,
	.i2c_addr = 0x21,
	.yuv = true,
	.lanes = 1,
	.width = 632,
	.height = 474,
	.grab_x = 1,
	.grab_y = 1,
	.tg_pad = 4,
	.tg_src = 8,
	.tg_fmt = 3,
	.pixid = 0,
	.vf_delay = 1,	/* SensorInterruptDelayLines */
	.ecc_order = 1,
	.vsync_pol = 0,
	.sync_a = 1,
	.sync_b = 0,
};

const struct cam_sensor *cam_sensor_by_name(const char *name)
{
	if (!strcmp(name, cam_sensor_sp2508.name))
		return &cam_sensor_sp2508;
	if (!strcmp(name, cam_sensor_sp0a20.name))
		return &cam_sensor_sp0a20;
	return NULL;
}

static int sens_wr(struct cam_proto *c, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg m = { .addr = c->sensor->i2c_addr, .flags = 0, .len = 2, .buf = buf };
	int ret = i2c_transfer(c->adap, &m, 1);

	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static int sens_rd(struct cam_proto *c, u8 reg, u8 *val)
{
	struct i2c_msg m[2] = {
		{ .addr = c->sensor->i2c_addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = c->sensor->i2c_addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	int ret = i2c_transfer(c->adap, m, 2);

	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static int sens_table(struct cam_proto *c, const struct cam_regval *t, size_t n, const char *what)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = sens_wr(c, t[i].reg, t[i].val);
		if (ret) {
			dev_err(c->dev, "%s: write %#04x = %#04x (entry %zu/%zu) failed: %d\n", what, t[i].reg,
				t[i].val, i, n, ret);
			return ret;
		}
	}
	dev_info(c->dev, "%s: %zu registers written\n", what, n);
	return 0;
}

int cam_sensor_identify(struct cam_proto *c)
{
	const struct cam_sensor *s = c->sensor;
	u8 hi = 0, lo = 0;
	u32 id;
	int ret = 0, retry;

	for (retry = 0; retry < 3; retry++) {
		ret = sens_wr(c, SP_PAGE_REG, 0x00);
		if (!ret)
			ret = sens_rd(c, 0x02, &hi);
		if (!ret && !s->yuv)
			ret = sens_rd(c, 0x03, &lo);
		if (!ret)
			break;
		msleep(10);
	}
	if (ret) {
		dev_err(c->dev, "%s: I2C access at %#x failed: %d\n", s->name, s->i2c_addr, ret);
		return ret;
	}

	id = s->yuv ? hi : (hi << 8 | lo);
	dev_info(c->dev, "%s: chip id %#x (expected %#x)\n", s->name, id, s->yuv ? SP0A20_ID : SP2508_ID);
	if (id != (s->yuv ? SP0A20_ID : SP2508_ID))
		return -ENODEV;
	return 0;
}

int cam_sensor_program(struct cam_proto *c)
{
	int ret;

	if (c->sensor->yuv)
		return sens_table(c, sp0a20_init, ARRAY_SIZE(sp0a20_init), "sp0a20 init");

	ret = sens_table(c, sp2508_init, ARRAY_SIZE(sp2508_init), "sp2508 init");
	if (!ret)
		ret = sens_table(c, sp2508_mode, ARRAY_SIZE(sp2508_mode), "sp2508 mode (flip/mirror)");
	if (!ret && cam_knobs.test_pattern) {
		ret = sens_table(c, sp2508_testpattern_on, ARRAY_SIZE(sp2508_testpattern_on),
				 "sp2508 colour bar");
	}
	return ret;
}
