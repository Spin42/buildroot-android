// SPDX-License-Identifier: GPL-2.0
/*
 * Stage 2 (receiver side): MIPI RX analog PHY, MIPI RX digital block, CSI-2 and SENINF1 configuration.
 * Register values follow docs/seninf-mipi.md (initTg1CSI2, setTg1CSI2, setTg1InputCfg, setTg1SensorModeCfg).
 */
#include <linux/delay.h>
#include <linux/iopoll.h>

#include "cam_proto.h"

static const u8 ana_lanes[] = { ANA_CON04, ANA_CON08, ANA_CON0C, ANA_CON10 };

int cam_seninf_lanes(const struct cam_proto *c)
{
	return cam_knobs.lanes > 0 ? clamp(cam_knobs.lanes, 1, 4) : c->sensor->lanes;
}

static int seninf_port(const struct cam_proto *c)
{
	if (cam_knobs.port >= 0)
		return cam_knobs.port ? 1 : 0;
	return c->sensor->slot == CAM_SLOT_MAIN ? 0 : 1;
}

static void ana_set(struct cam_proto *c, u32 off, u32 bits)
{
	cam_rmw(c, CAM_WIN_MIPI_ANA, off, 0, bits);
}

static void ana_clr(struct cam_proto *c, u32 off, u32 bits)
{
	cam_rmw(c, CAM_WIN_MIPI_ANA, off, bits, 0);
}

static void ana_all_data(struct cam_proto *c, bool set, u32 bits)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ana_lanes); i++)
		cam_rmw(c, CAM_WIN_MIPI_ANA, ana_lanes[i], set ? 0 : bits, set ? bits : 0);
}

static void csi2_config(struct cam_proto *c);

/* initTg1CSI2(1): analog PHY power-up and pin mux of the selected port. */
int cam_seninf_phy_on(struct cam_proto *c)
{
	u8 lanemap;
	u32 s44, s48;
	int port = seninf_port(c);

	c->port = port;
	dev_info(c->dev, "stage 2: MIPI RX on CSI%d, %d lane(s)\n", port, cam_seninf_lanes(c));

	cam_rmw(c, CAM_WIN_MIPI_ANA, ANA_CON48, ANA48_PD_MASK, 0);
	cam_rmw(c, CAM_WIN_MIPI_ANA, ANA_CON4C, ~ANA4C_PD_CLR_MASK, 0);
	ana_clr(c, ANA_CON50, ANA50_PD);
	cam_rmw(c, CAM_WIN_GPIO, GPIO_IES_117_120, GPIO_IES_117_120_BIT, 0);

	if (port == 0) {
		cam_rmw(c, CAM_WIN_GPIO, GPIO_MODE_100_104, 0xfff8, 0x1248);
		cam_rmw(c, CAM_WIN_GPIO, GPIO_MODE_105_109, 0x7fff, 0x1249);
		cam_rmw(c, CAM_WIN_GPIO, GPIO_MODE_110_114, 0x0007, 0x0001);
		ana_set(c, ANA_CON00, ANA_LANE_CSI0_SEL);
		ana_all_data(c, true, ANA_LANE_CSI0_SEL);
		ana_clr(c, ANA_CON00, ANA_LANE_CSI1_SEL);
		ana_clr(c, ANA_CON0C, ANA_LANE_CSI1_SEL);
		ana_clr(c, ANA_CON10, ANA_LANE_CSI1_SEL);
		ana_all_data(c, true, ANA_LANE_EN);
		ana_clr(c, ANA_CON20, ANA20_CSI1_SEL);
		lanemap = RX_LANEMAP_CSI0;
	} else {
		cam_rmw(c, CAM_WIN_GPIO, GPIO_MODE_110_114, 0xfff8, 0x1248);
		cam_rmw(c, CAM_WIN_GPIO, GPIO_MODE_115_119, 0x003f, 0x0009);
		ana_set(c, ANA_CON00, ANA_LANE_CSI1_SEL);
		ana_set(c, ANA_CON0C, ANA_LANE_CSI1_SEL);
		ana_set(c, ANA_CON10, ANA_LANE_CSI1_SEL);
		ana_clr(c, ANA_CON00, ANA_LANE_CSI0_SEL);
		ana_all_data(c, false, ANA_LANE_CSI0_SEL);
		ana_set(c, ANA_CON0C, ANA_LANE_EN);
		ana_set(c, ANA_CON10, ANA_LANE_EN);
		ana_set(c, ANA_CON20, ANA20_CSI1_SEL);
		usleep_range(1, 5);
		lanemap = RX_LANEMAP_CSI1;
	}
	cam_rmw(c, CAM_WIN_MIPI_RX, RX_LANE_MAP, RX_LANE_MAP_MASK, (u32)lanemap << 24);

	ana_set(c, ANA_CON24, 0x3);
	usleep_range(30, 60);
	ana_set(c, ANA_CON20, 0x1);
	usleep_range(1, 5);
	ana_set(c, ANA_CON00, ANA_LANE_EN);
	cam_rmw(c, CAM_WIN_MIPI_RX, RX_LANE_MAP, 0, RX_LANE_MAP_BIT19);
	cam_rmw(c, CAM_WIN_MIPI_RX, RX_CAL_CTRL, 0, 0x1);
	cam_wr(c, CAM_WIN_MIPI_RX, RX_CAL_DATA, RX_CAL_DATA_VAL);
	cam_rmw(c, CAM_WIN_MIPI_RX, RX_CAL_CTRL, 0, 0x4);
	usleep_range(500, 1000);
	s44 = cam_rd(c, CAM_WIN_MIPI_RX, RX_STA0);
	s48 = cam_rd(c, CAM_WIN_MIPI_RX, RX_STA1);
	cam_rmw(c, CAM_WIN_MIPI_RX, RX_CAL_CTRL, 0x1, 0);
	ana_clr(c, ANA_CON20, 0x20);
	ana_all_data(c, false, ANA_LANE_BIT22);
	ana_set(c, ANA_CON20, 0x2);
	cam_wr(c, CAM_WIN_SENINF, SENINF1_CSI2_LNMUX, lanemap);
	c->seninf_on = true;
	csi2_config(c);

	dev_info(c->dev, "MIPI RX calibration: RX_STA0 %#x (ready mask %#x), RX_STA1 %#x (ready mask %#x)\n",
		 s44, RX_STA0_READY, s48, RX_STA1_READY);
	if (!(s44 & RX_STA0_READY) || !(s48 & RX_STA1_READY)) {
		dev_warn(c->dev, "MIPI RX reports not ready (the stock code only logs this)\n");
		if (!cam_knobs.run_while_error)
			return -EIO;
	}
	return 0;
}

static void seninf_reset(struct cam_proto *c)
{
	u32 v = cam_rd(c, CAM_WIN_SENINF, SENINF1_CTRL);

	cam_wr(c, CAM_WIN_SENINF, SENINF1_CTRL, v | SENINF1_CTRL_RST);
	usleep_range(10, 20);
	cam_wr(c, CAM_WIN_SENINF, SENINF1_CTRL, v & ~SENINF1_CTRL_RST);
}

/* setTg1CSI2(): CSI-2 receiver control, delay and the SENINF1 reset pulse. */
static void csi2_config(struct cam_proto *c)
{
	const struct cam_sensor *s = c->sensor;
	u32 ctrl, v;
	int lanes = cam_seninf_lanes(c);
	int ecc = CAM_KNOB(cam_knobs.csi2_ecc_order, s->ecc_order);
	int vs = CAM_KNOB(cam_knobs.csi2_vsync, s->vsync_pol);

	ctrl = CSI2_CTRL_BASE | (ecc & 1) << CSI2_CTRL_ECC_ORDER_SHIFT | (vs & 1) << CSI2_CTRL_VSYNC_SHIFT |
	       (((1U << (lanes - 1)) - 1) << 1);
	if (cam_knobs.csi2_ctrl >= 0)
		ctrl = cam_knobs.csi2_ctrl;

	cam_rmw(c, CAM_WIN_SENINF, SENINF1_CTRL, SENINF1_CTRL_SRC_MASK, 0);
	cam_wr(c, CAM_WIN_SENINF, SENINF1_CSI2_DELAY, (cam_knobs.csi2_settle & 0xff) << CSI2_DELAY_SETTLE_SHIFT);
	cam_wr(c, CAM_WIN_SENINF, SENINF1_CSI2_REG108, cam_rd(c, CAM_WIN_SENINF, SENINF1_CSI2_REG10C) | 0x10);
	cam_wr(c, CAM_WIN_SENINF, SENINF1_CSI2_CTRL, ctrl);
	v = cam_rd(c, CAM_WIN_SENINF, SENINF1_CTRL) & ~(SENINF1_CTRL_SYNC_A | SENINF1_CTRL_RST);
	cam_wr(c, CAM_WIN_SENINF, SENINF1_CTRL, v | SENINF1_CTRL_RST);
	cam_wr(c, CAM_WIN_SENINF, SENINF1_CTRL, v);
	dev_info(c->dev, "CSI2_CTRL %#x CSI2_DELAY %#x SENINF1_CTRL %#x\n", ctrl,
		 cam_rd(c, CAM_WIN_SENINF, SENINF1_CSI2_DELAY), v);
}

/* setConf() receiver part: SENINF1 reset, sync mode, input routing, then setTg1CSI2() again. */
int cam_seninf_route(struct cam_proto *c)
{
	const struct cam_sensor *s = c->sensor;
	int pad = CAM_KNOB(cam_knobs.tg_pad, s->tg_pad);
	int src = CAM_KNOB(cam_knobs.tg_src, s->tg_src);
	int a = CAM_KNOB(cam_knobs.sync_a, s->sync_a);
	int b = CAM_KNOB(cam_knobs.sync_b, s->sync_b);
	u32 v;

	seninf_reset(c);
	cam_rmw(c, CAM_WIN_SENINF, SENINF1_CTRL, SENINF1_CTRL_SYNC_A | SENINF1_CTRL_SYNC_B,
		(b & 1) << 9 | (a & 1) << 10);

	v = cam_rd(c, CAM_WIN_SENINF, SENINF1_CTRL) & ~(SENINF1_CTRL_EN | SENINF1_CTRL_PAD_MASK | SENINF1_CTRL_SRC_MASK);
	v |= SENINF1_CTRL_EN | (pad & 7) << SENINF1_CTRL_PAD_SHIFT;
	if (src != 8)
		v |= (src << 12) & 0xffff;
	cam_wr(c, CAM_WIN_SENINF, SENINF1_CTRL, v);
	v = cam_rd(c, CAM_WIN_SENINF, SENINF1_CTRL) & ~SENINF1_CTRL_CONST_MASK;
	cam_wr(c, CAM_WIN_SENINF, SENINF1_CTRL, v | SENINF1_CTRL_CONST);
	cam_rmw(c, CAM_WIN_SENINF, SENINF1_CTRL, SENINF1_CTRL_JPEG, 0);

	csi2_config(c);
	cam_wr(c, CAM_WIN_SENINF, SENINF1_INTEN, cam_knobs.seninf_inten);
	return 0;
}

/* One-line summary of the receiver state, printed at the end of stage 2. */
void cam_seninf_report(struct cam_proto *c)
{
	u32 size = cam_rd(c, CAM_WIN_SENINF, SENINF1_SIZE);
	u32 err = cam_rd(c, CAM_WIN_SENINF, SENINF_NCSI2_INTSTA);

	dev_info(c->dev, "SENINF: CTRL %#x INTSTA %#x detected size %#x (hi %u, lo %u)\n",
		 cam_rd(c, CAM_WIN_SENINF, SENINF1_CTRL), cam_rd(c, CAM_WIN_SENINF, SENINF1_INTSTA), size,
		 size >> 16, size & 0xffff);
	dev_info(c->dev, "CSI-2: reg10C %#x err flags %#x (mask %#x -> %#x)\n",
		 cam_rd(c, CAM_WIN_SENINF, SENINF1_CSI2_REG10C), err, SENINF_NCSI2_ERR_MASK,
		 err & SENINF_NCSI2_ERR_MASK);
}

/* initTg1CSI2(0) and SeninfDrvImp::uninit(): PHY power-down. */
void cam_seninf_off(struct cam_proto *c)
{
	if (!c->seninf_on || !c->clk_on)
		return;

	ana_clr(c, ANA_CON24, 0x1);
	cam_rmw(c, CAM_WIN_MIPI_ANA, ANA_CON48, 0, ANA48_PD_MASK);
	cam_rmw(c, CAM_WIN_MIPI_ANA, ANA_CON4C, 0, ANA4C_PD_SET);
	ana_set(c, ANA_CON50, ANA50_PD);
	ana_clr(c, ANA_CON00, ANA_LANE_CSI0_SEL | ANA_LANE_CSI1_SEL);
	ana_clr(c, ANA_CON04, ANA_LANE_CSI0_SEL);
	ana_clr(c, ANA_CON08, ANA_LANE_CSI0_SEL);
	ana_clr(c, ANA_CON0C, ANA_LANE_CSI0_SEL | ANA_LANE_CSI1_SEL);
	ana_clr(c, ANA_CON10, ANA_LANE_CSI0_SEL | ANA_LANE_CSI1_SEL);
	ana_clr(c, ANA_CON24, 0x3);
	ana_clr(c, ANA_CON20, 0x43);
	ana_clr(c, ANA_CON00, 0x1f);
	ana_all_data(c, false, ANA_LANE_EN);
	c->seninf_on = false;
	dev_info(c->dev, "MIPI RX off\n");
}

static void dump_range(struct cam_proto *c, enum cam_win w, const char *tag, const u16 *offs, int n, char *buf,
		       size_t sz, size_t *np)
{
	int i;

	for (i = 0; i < n; i++)
		SNPR(buf, sz, *np, "%s+%#05x = %#010x\n", tag, offs[i], cam_rd(c, w, offs[i]));
}

void cam_seninf_dump(struct cam_proto *c, char *buf, size_t sz, size_t *np)
{
	static const u16 sen[] = { 0x000, 0x010, 0x014, 0x018, 0x02c, 0x100, 0x104, 0x108, 0x10c, 0x110, 0x114,
				   0x118, 0x11c, 0x120, 0x124, 0x128, 0x300, 0x304, 0x618 };
	static const u16 rx[] = { 0x24, 0x38, 0x3c, 0x44, 0x48 };
	static const u16 ana[] = { 0x00, 0x04, 0x08, 0x0c, 0x10, 0x20, 0x24, 0x48, 0x4c, 0x50 };
	u32 size = cam_rd(c, CAM_WIN_SENINF, SENINF1_SIZE);
	u32 err = cam_rd(c, CAM_WIN_SENINF, SENINF_NCSI2_INTSTA);

	SNPR(buf, sz, *np, "[seninf] port CSI%d lanes %d\n", c->port, cam_seninf_lanes(c));
	SNPR(buf, sz, *np, "  SENINF1 detected size: %#x (hi %u, lo %u)\n", size, size >> 16, size & 0xffff);
	SNPR(buf, sz, *np, "  SENINF1_INTSTA %#x, NCSI2 error flags %#x (masked %#x)\n",
	     cam_rd(c, CAM_WIN_SENINF, SENINF1_INTSTA), err, err & SENINF_NCSI2_ERR_MASK);
	SNPR(buf, sz, *np, "  MIPI RX ready: STA0 %#x (need %#x) STA1 %#x (need %#x)\n",
	     cam_rd(c, CAM_WIN_MIPI_RX, RX_STA0), RX_STA0_READY, cam_rd(c, CAM_WIN_MIPI_RX, RX_STA1),
	     RX_STA1_READY);
	dump_range(c, CAM_WIN_SENINF, "seninf", sen, ARRAY_SIZE(sen), buf, sz, np);
	dump_range(c, CAM_WIN_MIPI_RX, "mipi-rx", rx, ARRAY_SIZE(rx), buf, sz, np);
	dump_range(c, CAM_WIN_MIPI_ANA, "mipi-ana", ana, ARRAY_SIZE(ana), buf, sz, np);
}
