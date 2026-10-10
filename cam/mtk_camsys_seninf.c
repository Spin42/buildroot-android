// SPDX-License-Identifier: GPL-2.0
/*
 * CSI-2 receiver: MIPI RX analog PHY, MIPI RX digital block, CSI-2 and SENINF1 configuration, and the internal
 * subdev that models it. Register values follow docs/seninf-mipi.md (initTg1CSI2, setTg1CSI2, setTg1InputCfg).
 *
 * Pads: one sink per sensor port (0 rear on CSI0, 1 front on CSI1) and one source towards the ISP. Only one sink
 * link can be enabled at a time because both sensors share the MCLK and the SENINF1 input.
 */
#include <linux/delay.h>
#include <linux/minmax.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-subdev.h>

#include "mtk_camsys.h"

#define CSI2_SETTLE		14
#define SENINF_INTEN_DEFAULT	0x7f

const struct mtk_camsys_port_cfg mtk_camsys_port_cfgs[MTK_CAMSYS_NUM_PORTS] = {
	[MTK_CAMSYS_PORT_REAR] = {
		.mbus_code = MEDIA_BUS_FMT_SBGGR10_1X10,
		.pixelformat = V4L2_PIX_FMT_SBGGR10P,
		.width = 1616,
		.height = 1216,
		.yuv = false,
		.csi_port = 0,
		.tg_pad = 0,
		.tg_src = 3,
		.tg_fmt = 1,
		.pixid = 0,		/* first pixel is blue */
		.vf_delay = 0,
		.ecc_order = 1,
		.vsync_pol = 1,		/* without it the receiver sees frame packets but no pixels */
		.sync_a = 1,
		.sync_b = 0,
	},
	[MTK_CAMSYS_PORT_FRONT] = {
		.mbus_code = MEDIA_BUS_FMT_UYVY8_1X16,
		.pixelformat = V4L2_PIX_FMT_UYVY,
		.width = 640,
		.height = 480,
		.yuv = true,
		.csi_port = 1,
		.tg_pad = 4,
		.tg_src = 8,
		.tg_fmt = 3,
		.pixid = 0,
		.vf_delay = 1,
		.ecc_order = 1,
		.vsync_pol = 1,
		.sync_a = 1,
		.sync_b = 0,
	},
};

static const u8 ana_data_lanes[] = { ANA_CON04, ANA_CON08, ANA_CON0C, ANA_CON10 };

static void ana_set(struct mtk_camsys *cam, u32 off, u32 bits)
{
	mtk_camsys_rmw(cam->mipi_ana, off, 0, bits);
}

static void ana_clr(struct mtk_camsys *cam, u32 off, u32 bits)
{
	mtk_camsys_rmw(cam->mipi_ana, off, bits, 0);
}

static void ana_all_data(struct mtk_camsys *cam, bool set, u32 bits)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ana_data_lanes); i++)
		mtk_camsys_rmw(cam->mipi_ana, ana_data_lanes[i], set ? 0 : bits, set ? bits : 0);
}

/* setTg1CSI2(): CSI-2 receiver control, delay and the SENINF1 reset pulse. */
static void csi2_config(struct mtk_camsys *cam, const struct mtk_camsys_port_cfg *cfg, unsigned int lanes)
{
	u32 ctrl, v;

	ctrl = CSI2_CTRL_BASE | (cfg->ecc_order & 1) << CSI2_CTRL_ECC_ORDER_SHIFT |
	       (cfg->vsync_pol & 1) << CSI2_CTRL_VSYNC_SHIFT | (((1U << (lanes - 1)) - 1) << 1);

	mtk_camsys_rmw(cam->seninf, SENINF1_CTRL, SENINF1_CTRL_SRC_MASK, 0);
	writel(CSI2_SETTLE << CSI2_DELAY_SETTLE_SHIFT, cam->seninf + SENINF1_CSI2_DELAY);
	writel(readl(cam->seninf + SENINF1_CSI2_REG10C) | 0x10, cam->seninf + SENINF1_CSI2_REG108);
	writel(ctrl, cam->seninf + SENINF1_CSI2_CTRL);
	v = readl(cam->seninf + SENINF1_CTRL) & ~(SENINF1_CTRL_SYNC_A | SENINF1_CTRL_RST);
	writel(v | SENINF1_CTRL_RST, cam->seninf + SENINF1_CTRL);
	writel(v, cam->seninf + SENINF1_CTRL);
}

/* initTg1CSI2(1): analog PHY power-up, pin mux and lane map of the sensor's port. */
static void phy_on(struct mtk_camsys *cam, const struct mtk_camsys_port_cfg *cfg, unsigned int lanes)
{
	u8 lanemap;
	u32 sta0, sta1;

	mtk_camsys_rmw(cam->mipi_ana, ANA_CON48, ANA48_PD_MASK, 0);
	mtk_camsys_rmw(cam->mipi_ana, ANA_CON4C, ~ANA4C_PD_CLR_MASK, 0);
	ana_clr(cam, ANA_CON50, ANA50_PD);
	mtk_camsys_rmw(cam->gpio, GPIO_IES_117_120, GPIO_IES_117_120_BIT, 0);

	if (cfg->csi_port == 0) {
		mtk_camsys_rmw(cam->gpio, GPIO_MODE_100_104, 0xfff8, 0x1248);
		mtk_camsys_rmw(cam->gpio, GPIO_MODE_105_109, 0x7fff, 0x1249);
		mtk_camsys_rmw(cam->gpio, GPIO_MODE_110_114, 0x0007, 0x0001);
		ana_set(cam, ANA_CON00, ANA_LANE_CSI0_SEL);
		ana_all_data(cam, true, ANA_LANE_CSI0_SEL);
		ana_clr(cam, ANA_CON00, ANA_LANE_CSI1_SEL);
		ana_clr(cam, ANA_CON0C, ANA_LANE_CSI1_SEL);
		ana_clr(cam, ANA_CON10, ANA_LANE_CSI1_SEL);
		ana_all_data(cam, true, ANA_LANE_EN);
		ana_clr(cam, ANA_CON20, ANA20_CSI1_SEL);
		lanemap = RX_LANEMAP_CSI0;
	} else {
		mtk_camsys_rmw(cam->gpio, GPIO_MODE_110_114, 0xfff8, 0x1248);
		mtk_camsys_rmw(cam->gpio, GPIO_MODE_115_119, 0x003f, 0x0009);
		ana_set(cam, ANA_CON00, ANA_LANE_CSI1_SEL);
		ana_set(cam, ANA_CON0C, ANA_LANE_CSI1_SEL);
		ana_set(cam, ANA_CON10, ANA_LANE_CSI1_SEL);
		ana_clr(cam, ANA_CON00, ANA_LANE_CSI0_SEL);
		ana_all_data(cam, false, ANA_LANE_CSI0_SEL);
		ana_set(cam, ANA_CON0C, ANA_LANE_EN);
		ana_set(cam, ANA_CON10, ANA_LANE_EN);
		ana_set(cam, ANA_CON20, ANA20_CSI1_SEL);
		usleep_range(1, 5);
		lanemap = RX_LANEMAP_CSI1;
	}
	mtk_camsys_rmw(cam->mipi_rx, RX_LANE_MAP, RX_LANE_MAP_MASK, (u32)lanemap << 24);

	ana_set(cam, ANA_CON24, 0x3);
	usleep_range(30, 60);
	ana_set(cam, ANA_CON20, 0x1);
	usleep_range(1, 5);
	ana_set(cam, ANA_CON00, ANA_LANE_EN);
	mtk_camsys_rmw(cam->mipi_rx, RX_LANE_MAP, 0, RX_LANE_MAP_BIT19);
	mtk_camsys_rmw(cam->mipi_rx, RX_CAL_CTRL, 0, 0x1);
	writel(RX_CAL_DATA_VAL, cam->mipi_rx + RX_CAL_DATA);
	mtk_camsys_rmw(cam->mipi_rx, RX_CAL_CTRL, 0, 0x4);
	usleep_range(500, 1000);
	sta0 = readl(cam->mipi_rx + RX_STA0);
	sta1 = readl(cam->mipi_rx + RX_STA1);
	mtk_camsys_rmw(cam->mipi_rx, RX_CAL_CTRL, 0x1, 0);
	ana_clr(cam, ANA_CON20, 0x20);
	ana_all_data(cam, false, ANA_LANE_BIT22);
	ana_set(cam, ANA_CON20, 0x2);
	writel(lanemap, cam->seninf + SENINF1_CSI2_LNMUX);
	csi2_config(cam, cfg, lanes);

	/* The stock code only logs a failed calibration and carries on. */
	if ((sta0 & RX_STA0_READY) != RX_STA0_READY || (sta1 & RX_STA1_READY) != RX_STA1_READY)
		dev_dbg(cam->dev, "MIPI RX calibration not ready: STA0 %#x STA1 %#x\n", sta0, sta1);
}

/* setConf() receiver part: SENINF1 reset, sync mode, input routing, then setTg1CSI2() again. */
static void seninf_route(struct mtk_camsys *cam, const struct mtk_camsys_port_cfg *cfg, unsigned int lanes)
{
	u32 v = readl(cam->seninf + SENINF1_CTRL);

	writel(v | SENINF1_CTRL_RST, cam->seninf + SENINF1_CTRL);
	usleep_range(10, 20);
	writel(v & ~SENINF1_CTRL_RST, cam->seninf + SENINF1_CTRL);
	mtk_camsys_rmw(cam->seninf, SENINF1_CTRL, SENINF1_CTRL_SYNC_A | SENINF1_CTRL_SYNC_B,
		       (cfg->sync_b & 1) << 9 | (cfg->sync_a & 1) << 10);

	v = readl(cam->seninf + SENINF1_CTRL) & ~(SENINF1_CTRL_EN | SENINF1_CTRL_PAD_MASK | SENINF1_CTRL_SRC_MASK);
	v |= SENINF1_CTRL_EN | (cfg->tg_pad & 7) << SENINF1_CTRL_PAD_SHIFT;
	if (cfg->tg_src != 8)
		v |= (cfg->tg_src << 12) & 0xffff;
	writel(v, cam->seninf + SENINF1_CTRL);
	v = readl(cam->seninf + SENINF1_CTRL) & ~SENINF1_CTRL_CONST_MASK;
	writel(v | SENINF1_CTRL_CONST, cam->seninf + SENINF1_CTRL);
	mtk_camsys_rmw(cam->seninf, SENINF1_CTRL, SENINF1_CTRL_JPEG, 0);

	csi2_config(cam, cfg, lanes);
	writel(SENINF_INTEN_DEFAULT, cam->seninf + SENINF1_INTEN);
}

/* initTg1CSI2(0) and SeninfDrvImp::uninit(): PHY power-down. */
static void phy_off(struct mtk_camsys *cam)
{
	writel(0, cam->seninf + SENINF1_INTEN);
	ana_clr(cam, ANA_CON24, 0x1);
	mtk_camsys_rmw(cam->mipi_ana, ANA_CON48, 0, ANA48_PD_MASK);
	mtk_camsys_rmw(cam->mipi_ana, ANA_CON4C, 0, ANA4C_PD_SET);
	ana_set(cam, ANA_CON50, ANA50_PD);
	ana_clr(cam, ANA_CON00, ANA_LANE_CSI0_SEL | ANA_LANE_CSI1_SEL);
	ana_clr(cam, ANA_CON04, ANA_LANE_CSI0_SEL);
	ana_clr(cam, ANA_CON08, ANA_LANE_CSI0_SEL);
	ana_clr(cam, ANA_CON0C, ANA_LANE_CSI0_SEL | ANA_LANE_CSI1_SEL);
	ana_clr(cam, ANA_CON10, ANA_LANE_CSI0_SEL | ANA_LANE_CSI1_SEL);
	ana_clr(cam, ANA_CON24, 0x3);
	ana_clr(cam, ANA_CON20, 0x43);
	ana_clr(cam, ANA_CON00, 0x1f);
	ana_all_data(cam, false, ANA_LANE_EN);
}

/* Subdev */

static struct mtk_camsys *sd_to_cam(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mtk_camsys, csi2);
}

static void csi2_fill_format(struct v4l2_mbus_framefmt *fmt, const struct mtk_camsys_port_cfg *cfg)
{
	fmt->width = cfg->width;
	fmt->height = cfg->height;
	fmt->code = cfg->mbus_code;
	fmt->field = V4L2_FIELD_NONE;
	if (cfg->yuv) {
		fmt->colorspace = V4L2_COLORSPACE_SRGB;
		fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
		fmt->quantization = V4L2_QUANTIZATION_LIM_RANGE;
		fmt->xfer_func = V4L2_XFER_FUNC_SRGB;
	} else {
		fmt->colorspace = V4L2_COLORSPACE_RAW;
		fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
		fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
		fmt->xfer_func = V4L2_XFER_FUNC_NONE;
	}
}

static const struct mtk_camsys_port_cfg *csi2_pad_cfg(struct mtk_camsys *cam, u32 pad)
{
	if (pad == MTK_CSI2_PAD_SRC)
		pad = cam->active_port < 0 ? MTK_CAMSYS_PORT_REAR : cam->active_port;
	return &mtk_camsys_port_cfgs[pad];
}

static int csi2_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	struct mtk_camsys *cam = sd_to_cam(sd);
	unsigned int i;

	for (i = 0; i < MTK_CSI2_NUM_PADS; i++)
		csi2_fill_format(v4l2_subdev_state_get_format(state, i), csi2_pad_cfg(cam, i));
	return 0;
}

static int csi2_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state, struct v4l2_subdev_format *fmt)
{
	struct mtk_camsys *cam = sd_to_cam(sd);

	/* The formats are fixed by the sensors: the request is ignored and the current format returned. */
	csi2_fill_format(&fmt->format, csi2_pad_cfg(cam, fmt->pad));
	*v4l2_subdev_state_get_format(state, fmt->pad) = fmt->format;
	return 0;
}

static int csi2_enum_mbus_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			       struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad >= MTK_CSI2_NUM_PADS || code->index)
		return -EINVAL;
	code->code = csi2_pad_cfg(sd_to_cam(sd), code->pad)->mbus_code;
	return 0;
}

static int csi2_enum_frame_size(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				struct v4l2_subdev_frame_size_enum *fse)
{
	const struct mtk_camsys_port_cfg *cfg;

	if (fse->pad >= MTK_CSI2_NUM_PADS || fse->index)
		return -EINVAL;
	cfg = csi2_pad_cfg(sd_to_cam(sd), fse->pad);
	if (fse->code != cfg->mbus_code)
		return -EINVAL;
	fse->min_width = cfg->width;
	fse->max_width = cfg->width;
	fse->min_height = cfg->height;
	fse->max_height = cfg->height;
	return 0;
}

/* Only the data type and the frame size have to agree with the sensor; the colour space follows from the code. */
static int csi2_link_validate(struct v4l2_subdev *sd, struct media_link *link,
			      struct v4l2_subdev_format *source_fmt, struct v4l2_subdev_format *sink_fmt)
{
	if (source_fmt->format.code != sink_fmt->format.code ||
	    source_fmt->format.width != sink_fmt->format.width ||
	    source_fmt->format.height != sink_fmt->format.height)
		return -EPIPE;
	return 0;
}

static int csi2_enable_streams(struct v4l2_subdev *sd, struct v4l2_subdev_state *state, u32 pad, u64 streams_mask)
{
	struct mtk_camsys *cam = sd_to_cam(sd);
	struct mtk_camsys_port *port;
	struct media_pad *remote;
	struct v4l2_subdev *sensor;
	int ret;

	if (cam->active_port < 0)
		return -ENOLINK;
	port = &cam->ports[cam->active_port];

	remote = media_pad_remote_pad_unique(&sd->entity.pads[cam->active_port]);
	if (IS_ERR(remote))
		return PTR_ERR(remote);
	sensor = media_entity_to_v4l2_subdev(remote->entity);

	ret = pm_runtime_resume_and_get(cam->dev);
	if (ret)
		return ret;

	phy_on(cam, port->cfg, port->lanes);
	ret = v4l2_subdev_enable_streams(sensor, remote->index, BIT_ULL(0));
	if (ret) {
		phy_off(cam);
		pm_runtime_put(cam->dev);
		return ret;
	}
	seninf_route(cam, port->cfg, port->lanes);
	return 0;
}

static int csi2_disable_streams(struct v4l2_subdev *sd, struct v4l2_subdev_state *state, u32 pad, u64 streams_mask)
{
	struct mtk_camsys *cam = sd_to_cam(sd);
	struct media_pad *remote;

	if (cam->active_port >= 0) {
		remote = media_pad_remote_pad_unique(&sd->entity.pads[cam->active_port]);
		if (!IS_ERR(remote))
			v4l2_subdev_disable_streams(media_entity_to_v4l2_subdev(remote->entity), remote->index,
						    BIT_ULL(0));
	}
	phy_off(cam);
	pm_runtime_put(cam->dev);
	return 0;
}

static int csi2_link_setup(struct media_entity *entity, const struct media_pad *local,
			   const struct media_pad *remote, u32 flags)
{
	struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(entity);
	struct mtk_camsys *cam = sd_to_cam(sd);
	struct v4l2_subdev_state *state;

	if (!(local->flags & MEDIA_PAD_FL_SINK))
		return 0;

	if (flags & MEDIA_LNK_FL_ENABLED) {
		if (cam->active_port >= 0 && cam->active_port != local->index)
			return -EBUSY;
		cam->active_port = local->index;
	} else if (cam->active_port == local->index) {
		cam->active_port = -1;
	}

	state = v4l2_subdev_lock_and_get_active_state(sd);
	if (state) {
		csi2_fill_format(v4l2_subdev_state_get_format(state, MTK_CSI2_PAD_SRC),
				 csi2_pad_cfg(cam, MTK_CSI2_PAD_SRC));
		v4l2_subdev_unlock_state(state);
	}
	return 0;
}

static const struct v4l2_subdev_pad_ops csi2_pad_ops = {
	.enum_mbus_code = csi2_enum_mbus_code,
	.enum_frame_size = csi2_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = csi2_set_fmt,
	.link_validate = csi2_link_validate,
	.enable_streams = csi2_enable_streams,
	.disable_streams = csi2_disable_streams,
};

static const struct v4l2_subdev_ops csi2_subdev_ops = {
	.pad = &csi2_pad_ops,
};

static const struct v4l2_subdev_internal_ops csi2_internal_ops = {
	.init_state = csi2_init_state,
};

static const struct media_entity_operations csi2_entity_ops = {
	.link_setup = csi2_link_setup,
	.link_validate = v4l2_subdev_link_validate,
};

int mtk_camsys_csi2_register(struct mtk_camsys *cam)
{
	struct v4l2_subdev *sd = &cam->csi2;
	unsigned int i;
	int ret;

	cam->active_port = -1;

	v4l2_subdev_init(sd, &csi2_subdev_ops);
	sd->internal_ops = &csi2_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->dev = cam->dev;
	sd->owner = THIS_MODULE;
	strscpy(sd->name, "mtk-seninf", sizeof(sd->name));
	v4l2_set_subdevdata(sd, cam);

	sd->entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd->entity.ops = &csi2_entity_ops;
	for (i = 0; i < MTK_CAMSYS_NUM_PORTS; i++)
		cam->csi2_pads[i].flags = MEDIA_PAD_FL_SINK;
	cam->csi2_pads[MTK_CSI2_PAD_SRC].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sd->entity, MTK_CSI2_NUM_PADS, cam->csi2_pads);
	if (ret)
		return ret;

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto err_entity;

	ret = v4l2_device_register_subdev(&cam->v4l2_dev, sd);
	if (ret)
		goto err_state;
	return 0;

err_state:
	v4l2_subdev_cleanup(sd);
err_entity:
	media_entity_cleanup(&sd->entity);
	return ret;
}

void mtk_camsys_csi2_unregister(struct mtk_camsys *cam)
{
	v4l2_device_unregister_subdev(&cam->csi2);
	v4l2_subdev_cleanup(&cam->csi2);
	media_entity_cleanup(&cam->csi2.entity);
}
