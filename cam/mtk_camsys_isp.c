// SPDX-License-Identifier: GPL-2.0
/*
 * ISP pass 1: reset, TG, path select and the IMGO DMA, driven continuously; the frame-done interrupt hands over
 * to the buffer rotation of the video node. Register values follow docs/isp-capture.md.
 */
#include <linux/bitfield.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/module.h>

#include "mtk_camsys.h"

static int grab_x = -1;
module_param(grab_x, int, 0644);
MODULE_PARM_DESC(grab_x, "TG grab start in pixels, -1 = port default");
static int grab_y = -1;
module_param(grab_y, int, 0644);
MODULE_PARM_DESC(grab_y, "TG grab start in lines, -1 = port default");

/*
 * Pass-1 path images (EScenarioID 8, IMGO only). Each entry is a SET and a CLR value written through the helper
 * registers of the register named by the macro.
 */
struct path_reg {
	u16 off_set, off_clr;
	u32 set, clr;
};

enum { PATH_EN1, PATH_EN2, PATH_DMA, PATH_FMT, PATH_CTL, PATH_MUX, PATH_MUX2, PATH_CONF, PATH_NUM };

static const struct path_reg path_raw10[PATH_NUM] = {
	[PATH_EN1]  = { ISP_EN1_SET,      ISP_EN1_CLR,      0x40001001, 0x00002000 },
	[PATH_EN2]  = { ISP_EN2_SET,      ISP_EN2_CLR,      0x00000000, 0x00000020 },
	[PATH_DMA]  = { ISP_DMA_EN_SET,   ISP_DMA_EN_CLR,   0x00000001, 0x00000000 },
	[PATH_FMT]  = { ISP_FMT_SEL_SET,  ISP_FMT_SEL_CLR,  0x00001006, 0x0000e071 },
	[PATH_CTL]  = { ISP_CTL_SEL_SET,  ISP_CTL_SEL_CLR,  0x00010050, 0x00000100 },
	[PATH_MUX]  = { ISP_MUX_SEL_SET,  ISP_MUX_SEL_CLR,  0x00000000, 0xdffe7fff },
	[PATH_MUX2] = { ISP_MUX_SEL2_SET, ISP_MUX_SEL2_CLR, 0xc028410c, 0x00901e70 },
	[PATH_CONF] = { ISP_MUX_CONF_SET, ISP_MUX_CONF_CLR, 0x110054f7, 0xee000000 },
};

static const struct path_reg path_yuv422[PATH_NUM] = {
	[PATH_EN1]  = { ISP_EN1_SET,      ISP_EN1_CLR,      0x08000001, 0x00003000 },
	[PATH_EN2]  = { ISP_EN2_SET,      ISP_EN2_CLR,      0x00000000, 0x00000020 },
	[PATH_DMA]  = { ISP_DMA_EN_SET,   ISP_DMA_EN_CLR,   0x00000001, 0x00000000 },
	[PATH_FMT]  = { ISP_FMT_SEL_SET,  ISP_FMT_SEL_CLR,  0x00002016, 0x0000d061 },
	[PATH_CTL]  = { ISP_CTL_SEL_SET,  ISP_CTL_SEL_CLR,  0x00010050, 0x00000100 },
	[PATH_MUX]  = { ISP_MUX_SEL_SET,  ISP_MUX_SEL_CLR,  0x00400040, 0xdfbe7fbf },
	[PATH_MUX2] = { ISP_MUX_SEL2_SET, ISP_MUX_SEL2_CLR, 0xc0081108, 0x00b00e74 },
	[PATH_CONF] = { ISP_MUX_CONF_SET, ISP_MUX_CONF_CLR, 0x110054f7, 0xee000000 },
};

#define IMGO_CON_DEFAULT	0x08100850
#define IMGO_CON2_DEFAULT	0x00100800
#define PIX_ID_SET_BIT20	BIT(20)
#define IMGO_DMA_EN		BIT(0)
#define IMGO_DMA_EXTRA		BIT(5)	/* cleared at stop by the vendor */
#define ISP_INT_EN_DEFAULT	0x01a01e31
#define ISP_DMA_INT_BITS	0x04100000
#define TG_IDLE_POLL_US		1000
#define TG_IDLE_TIMEOUT_US	200000

static void path_apply(struct mtk_camsys *cam, const struct path_reg *r)
{
	if (r->clr)
		writel(r->clr, cam->isp + r->off_clr);
	if (r->set)
		writel(r->set, cam->isp + r->off_set);
}

static void fmt_sel_field(struct mtk_camsys *cam, u32 clr, u32 set)
{
	writel(clr, cam->isp + ISP_FMT_SEL_CLR);
	if (set)
		writel(set, cam->isp + ISP_FMT_SEL_SET);
}

static void isp_reset(struct mtk_camsys *cam)
{
	void __iomem *isp = cam->isp;
	u32 v;

	writel(SW_RST_TRIG, isp + ISP_CTL_SW_CTL);
	if (readl_poll_timeout(isp + ISP_CTL_SW_CTL, v, v & SW_RST_STATUS, 10, 10000))
		dev_warn(cam->dev, "ISP reset status bit did not rise, continuing\n");
	writel(SW_RST_TRIG | SW_HW_RST, isp + ISP_CTL_SW_CTL);
	writel(SW_HW_RST, isp + ISP_CTL_SW_CTL);
	writel(0, isp + ISP_CTL_SW_CTL);
}

/* setTg1GrabRange, setTg1SensorModeCfg, setTg1InputCfg, setTg1ViewFinderMode and the CMOS enable. */
static void tg_config(struct mtk_camsys *cam, const struct mtk_camsys_port_cfg *cfg)
{
	void __iomem *isp = cam->isp;
	u32 xs = grab_x >= 0 ? grab_x : cfg->grab_x;
	u32 ys = grab_y >= 0 ? grab_y : cfg->grab_y;
	/* A YUV pixel takes two TG samples. */
	u32 xe = xs + (cfg->yuv ? 2 * cfg->width : cfg->width);
	u32 ye = ys + cfg->height;

	writel((xs & 0x7fff) | (xe & 0x7fff) << 16, isp + ISP_TG_SEN_GRAB_PXL);
	writel((ys & 0x7fff) | (ye & 0x7fff) << 16, isp + ISP_TG_SEN_GRAB_LIN);
	mtk_camsys_rmw(isp, ISP_TG_SEN_MODE, 0, TG_SEN_MODE_CMOS_EN | TG_SEN_MODE_SYNC);

	mtk_camsys_rmw(isp, ISP_TG_PATH_CFG, 0x3, 0);
	fmt_sel_field(cam, FMT_SEL_TG1_FMT_MASK, (cfg->tg_fmt & 7) << FMT_SEL_TG1_FMT_SHIFT);
	if (cfg->tg_src == 8)
		mtk_camsys_rmw(isp, ISP_TG_SEN_MODE, TG_SEN_MODE_MIPI_CLR, 0);
	else
		mtk_camsys_rmw(isp, ISP_TG_SEN_MODE, 0, TG_SEN_MODE_PARALLEL);
	mtk_camsys_rmw(isp, ISP_TG_SEN_MODE, TG_SEN_MODE_JPEG, 0);
	fmt_sel_field(cam, FMT_SEL_TG1_JPEG, 0);
	mtk_camsys_rmw(isp, ISP_TG_PATH_CFG, 0x10, 0);

	mtk_camsys_rmw(isp, ISP_TG_VF_CON, TG_VF_CLR_MASK, TG_VF_ALWAYS_SET | (cfg->vf_delay & 7) << TG_VF_DELAY_SHIFT);
	mtk_camsys_rmw(isp, ISP_TG_SEN_MODE, 0, TG_SEN_MODE_CMOS_EN);
}

static void isp_defaults(struct mtk_camsys *cam)
{
	void __iomem *isp = cam->isp;

	writel(ISP_EN1_DEFAULT, isp + ISP_CTL_EN1);
	mtk_camsys_rmw(isp, ISP_CTL_EN2, ~ISP_EN2_KEEP_MASK, 0);
	writel(0, isp + ISP_CTL_DMA_EN);
	writel(0, isp + ISP_REG_1600);
	writel(0, isp + ISP_CTL_SEL);

	writel(0, isp + ISP_CTL_INT_EN);
	mtk_camsys_rmw(isp, ISP_CTL_DMA_INT, 0, ISP_DMA_INT_BITS);
	writel(ISP_V06C, isp + ISP_REG_06C);
	writel(ISP_V150, isp + ISP_REG_150);
	writel(0, isp + ISP_REG_4F4);
	writel(ISP_V380, isp + ISP_REG_380);
	writel(0, isp + ISP_REG_384);
	writel(ISP_V39C, isp + ISP_REG_39C);
	writel(0, isp + ISP_REG_3A0);
	writel(ISP_V280, isp + ISP_REG_280);
	writel(0, isp + ISP_REG_284);
	writel(ISP_DBG_CQ_STATUS, isp + ISP_DBG_SET);
}

void mtk_camsys_isp_set_base(struct mtk_camsys *cam, dma_addr_t dma)
{
	writel(lower_32_bits(dma), cam->isp + ISP_IMGO_BASE_ADDR);
}

/*
 * Reset and configure pass 1 for the sensor on @cfg, program the IMGO geometry and the first buffer, then start
 * the view finder. The base address is written before the view finder starts, FBC stays off.
 */
void mtk_camsys_isp_start(struct mtk_camsys *cam, const struct mtk_camsys_port_cfg *cfg, u32 bytesperline,
			  dma_addr_t dma)
{
	const struct path_reg *path = cfg->yuv ? path_yuv422 : path_raw10;
	void __iomem *isp = cam->isp;
	unsigned int i;

	isp_reset(cam);
	mtk_camsys_tg_clock_restore(cam);
	isp_defaults(cam);
	tg_config(cam, cfg);

	for (i = 0; i < PATH_NUM; i++)
		if (i != PATH_DMA)
			path_apply(cam, &path[i]);
	if (!cfg->yuv) {
		writel((cfg->pixid & 3) | PIX_ID_SET_BIT20, isp + ISP_PIX_ID_SET);
		writel(~cfg->pixid & 3, isp + ISP_PIX_ID_CLR);
	}

	writel(0, isp + ISP_IMGO_OFST_ADDR);
	writel(bytesperline - 1, isp + ISP_IMGO_XSIZE);
	writel(cfg->height - 1, isp + ISP_IMGO_YSIZE);
	writel(bytesperline, isp + ISP_IMGO_STRIDE);
	writel(IMGO_CON_DEFAULT, isp + ISP_IMGO_CON);
	writel(IMGO_CON2_DEFAULT, isp + ISP_IMGO_CON2);
	writel(0, isp + ISP_IMGO_CROP);
	mtk_camsys_isp_set_base(cam, dma);
	path_apply(cam, &path[PATH_DMA]);
	writel(ISP_DBG_CQ_STATUS, isp + ISP_DBG_SET);

	/* Drop stale status (read to clear) before enabling the interrupts. */
	readl(isp + ISP_CTL_INT_STATUS);
	readl(isp + ISP_CTL_DMA_INT);
	enable_irq(cam->irq);
	writel(ISP_INT_EN_DEFAULT, isp + ISP_CTL_INT_EN);

	mtk_camsys_rmw(isp, ISP_TG_VF_CON, TG_VF_SINGLE_MODE, 0);
	mtk_camsys_rmw(isp, ISP_TG_VF_CON, 0, TG_VF_VFDATA_EN);
}

/* Stop the TG, the interrupts and the DMA; wait (bounded) for the TG to go idle. */
void mtk_camsys_isp_stop(struct mtk_camsys *cam)
{
	void __iomem *isp = cam->isp;
	u32 v;

	mtk_camsys_rmw(isp, ISP_TG_VF_CON, TG_VF_VFDATA_EN, 0);
	writel(0, isp + ISP_CTL_INT_EN);
	writel(IMGO_DMA_EN | IMGO_DMA_EXTRA, isp + ISP_DMA_EN_CLR);
	if (readl_poll_timeout(isp + ISP_TG_INTER_ST, v, FIELD_GET(TG_INTER_ST_MASK, v) == TG_INTER_ST_IDLE,
			       TG_IDLE_POLL_US, TG_IDLE_TIMEOUT_US))
		dev_warn(cam->dev, "TG did not reach idle (TG_INTER_ST %#x)\n", v);
	disable_irq(cam->irq);
	readl(isp + ISP_CTL_INT_STATUS);
	readl(isp + ISP_CTL_DMA_INT);
}

irqreturn_t mtk_camsys_isp_irq(int irq, void *data)
{
	struct mtk_camsys *cam = data;
	u32 st = readl(cam->isp + ISP_CTL_INT_STATUS);
	u32 dma = readl(cam->isp + ISP_CTL_DMA_INT);

	if (!st && !dma)
		return IRQ_NONE;

	if (st & INT_SOF1)
		cam->sof_ns = ktime_get_ns();
	if (st & INT_ERR_MASK)
		cam->err_bits |= st & INT_ERR_MASK;
	if (st & INT_PASS1_TG1_DON)
		mtk_camsys_video_frame_done(cam, st);
	return IRQ_HANDLED;
}
