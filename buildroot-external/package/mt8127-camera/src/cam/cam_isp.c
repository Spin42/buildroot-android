// SPDX-License-Identifier: GPL-2.0
/*
 * Stages 3 and 4: ISP reset and pass-1 configuration (TG, path select), IMGO DMA, single-frame capture.
 * Register values follow docs/isp-capture.md; its open questions are exposed as module parameters.
 */
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include "cam_proto.h"

/*
 * Pass-1 path images (docs/isp-capture.md 5.2, EScenarioID 8, IMGO only, continuous). Each pair is a SET and a
 * CLR helper register; "base" is the register they act on when set_clr_mode is 0 (0: no known base, helpers only).
 * The "DMA" entry is applied at capture time, the others in stage 3. The table is writable through debugfs
 * path_regs so that the values can be adjusted without rebuilding.
 */
struct cam_path_reg {
	const char *name;
	u16 base, off_set, off_clr;
	u32 set, clr;
};

static struct cam_path_reg path_raw10[] = {
	{ "EN1",  ISP_CTL_EN1,     ISP_EN1_SET,      ISP_EN1_CLR,      0x40001001, 0x00002000 },
	{ "EN2",  ISP_CTL_EN2,     ISP_EN2_SET,      ISP_EN2_CLR,      0x00000000, 0x00000020 },
	{ "DMA",  ISP_CTL_DMA_EN,  ISP_DMA_EN_SET,   ISP_DMA_EN_CLR,   0x00000001, 0x00000000 },
	{ "FMT",  ISP_CTL_FMT_SEL, ISP_FMT_SEL_SET,  ISP_FMT_SEL_CLR,  0x00001006, 0x0000e071 },
	{ "CTL",  ISP_CTL_SEL,     ISP_CTL_SEL_SET,  ISP_CTL_SEL_CLR,  0x00010050, 0x00000100 },
	{ "MUX",  0,               ISP_MUX_SEL_SET,  ISP_MUX_SEL_CLR,  0x00000000, 0xdffe7fff },
	{ "MUX2", 0,               ISP_MUX_SEL2_SET, ISP_MUX_SEL2_CLR, 0xc028410c, 0x00901e70 },
	{ "CONF", 0,               ISP_MUX_CONF_SET, ISP_MUX_CONF_CLR, 0x110054f7, 0xee000000 },
};

static struct cam_path_reg path_yuv422[] = {
	{ "EN1",  ISP_CTL_EN1,     ISP_EN1_SET,      ISP_EN1_CLR,      0x08000001, 0x00003000 },
	{ "EN2",  ISP_CTL_EN2,     ISP_EN2_SET,      ISP_EN2_CLR,      0x00000000, 0x00000020 },
	{ "DMA",  ISP_CTL_DMA_EN,  ISP_DMA_EN_SET,   ISP_DMA_EN_CLR,   0x00000001, 0x00000000 },
	{ "FMT",  ISP_CTL_FMT_SEL, ISP_FMT_SEL_SET,  ISP_FMT_SEL_CLR,  0x00002016, 0x0000d061 },
	{ "CTL",  ISP_CTL_SEL,     ISP_CTL_SEL_SET,  ISP_CTL_SEL_CLR,  0x00010050, 0x00000100 },
	{ "MUX",  0,               ISP_MUX_SEL_SET,  ISP_MUX_SEL_CLR,  0x00400040, 0xdfbe7fbf },
	{ "MUX2", 0,               ISP_MUX_SEL2_SET, ISP_MUX_SEL2_CLR, 0xc0081108, 0x00b00e74 },
	{ "CONF", 0,               ISP_MUX_CONF_SET, ISP_MUX_CONF_CLR, 0x110054f7, 0xee000000 },
};

#define IMGO_CON_DEFAULT	0x08100850
#define IMGO_CON2_DEFAULT	0x00100800
#define PIX_ID_SET_BIT20	BIT(20)
#define IMGO_DMA_EN		BIT(0)
#define IMGO_DMA_EXTRA		BIT(5)	/* cleared at stop by the vendor (DMA_EN_CLR |= 0x20) */
#define FBC_POLL_US		10

static struct cam_path_reg *path_table(const struct cam_proto *c, size_t *n)
{
	if (c->sensor && c->sensor->yuv) {
		*n = ARRAY_SIZE(path_yuv422);
		return path_yuv422;
	}
	*n = ARRAY_SIZE(path_raw10);
	return path_raw10;
}

void cam_geometry(const struct cam_proto *c, u32 *w, u32 *h)
{
	*w = cam_knobs.width > 0 ? cam_knobs.width : c->sensor->width;
	*h = cam_knobs.height > 0 ? cam_knobs.height : c->sensor->height;
}

static void isp_setclr(struct cam_proto *c, const struct cam_path_reg *r, u32 set, u32 clr)
{
	if (cam_knobs.set_clr_mode || !r->base) {
		if (clr)
			cam_wr(c, CAM_WIN_ISP, r->off_clr, clr);
		if (set)
			cam_wr(c, CAM_WIN_ISP, r->off_set, set);
	} else {
		cam_rmw(c, CAM_WIN_ISP, r->base, clr, set);
	}
}

static void path_apply(struct cam_proto *c, const struct cam_path_reg *r)
{
	isp_setclr(c, r, r->set, r->clr);
}

/* FMT_SEL clear-then-set of one field, through the helper registers or the base register. */
static void fmt_sel_field(struct cam_proto *c, u32 clr, u32 set)
{
	if (cam_knobs.set_clr_mode) {
		cam_wr(c, CAM_WIN_ISP, ISP_FMT_SEL_CLR, clr);
		if (set)
			cam_wr(c, CAM_WIN_ISP, ISP_FMT_SEL_SET, set);
	} else {
		cam_rmw(c, CAM_WIN_ISP, ISP_CTL_FMT_SEL, clr, set);
	}
}

static void isp_reset(struct cam_proto *c)
{
	u32 v;

	if (c->base[CAM_WIN_EMI])
		cam_rmw(c, CAM_WIN_EMI, EMI_BWL_REG, 0, EMI_BWL_TG_MASK);

	cam_wr(c, CAM_WIN_ISP, ISP_CTL_SW_CTL, SW_RST_TRIG);
	if (readl_poll_timeout(c->base[CAM_WIN_ISP] + ISP_CTL_SW_CTL, v, v & SW_RST_STATUS, 10, 10000))
		dev_warn(c->dev, "ISP reset status bit did not rise (SW_CTL %#x), continuing\n", v);
	cam_wr(c, CAM_WIN_ISP, ISP_CTL_SW_CTL, SW_RST_TRIG | SW_HW_RST);
	cam_wr(c, CAM_WIN_ISP, ISP_CTL_SW_CTL, SW_HW_RST);
	cam_wr(c, CAM_WIN_ISP, ISP_CTL_SW_CTL, 0);
	dev_info(c->dev, "ISP reset done, SW_CTL %#x\n", cam_rd(c, CAM_WIN_ISP, ISP_CTL_SW_CTL));
}

/* setTg1GrabRange, setTg1SensorModeCfg, setTg1InputCfg, setTg1ViewFinderMode and the CMOS enable. */
static void tg_config(struct cam_proto *c)
{
	const struct cam_sensor *s = c->sensor;
	u32 w, h, xs, ys, xe, ye;
	int src = CAM_KNOB(cam_knobs.tg_src, s->tg_src);
	int fmt = CAM_KNOB(cam_knobs.tg_fmt, s->tg_fmt);
	int delay = CAM_KNOB(cam_knobs.vf_delay, s->vf_delay);

	cam_geometry(c, &w, &h);
	xs = CAM_KNOB(cam_knobs.grab_x, s->grab_x);
	ys = CAM_KNOB(cam_knobs.grab_y, s->grab_y);
	xe = xs + (s->yuv ? 2 * w : w);
	ye = ys + h;

	cam_wr(c, CAM_WIN_ISP, ISP_TG_SEN_GRAB_PXL, (xs & 0x7fff) | (xe & 0x7fff) << 16);
	cam_wr(c, CAM_WIN_ISP, ISP_TG_SEN_GRAB_LIN, (ys & 0x7fff) | (ye & 0x7fff) << 16);
	cam_rmw(c, CAM_WIN_ISP, ISP_TG_SEN_MODE, 0, TG_SEN_MODE_CMOS_EN | TG_SEN_MODE_SYNC);

	cam_rmw(c, CAM_WIN_ISP, ISP_TG_PATH_CFG, 0x3, 0);
	fmt_sel_field(c, FMT_SEL_TG1_FMT_MASK, (fmt & 7) << FMT_SEL_TG1_FMT_SHIFT);
	if (src == 8)
		cam_rmw(c, CAM_WIN_ISP, ISP_TG_SEN_MODE, TG_SEN_MODE_MIPI_CLR, 0);
	else
		cam_rmw(c, CAM_WIN_ISP, ISP_TG_SEN_MODE, 0, TG_SEN_MODE_PARALLEL);
	cam_rmw(c, CAM_WIN_ISP, ISP_TG_SEN_MODE, TG_SEN_MODE_JPEG, 0);
	fmt_sel_field(c, FMT_SEL_TG1_JPEG, 0);
	cam_rmw(c, CAM_WIN_ISP, ISP_TG_PATH_CFG, 0x10, 0);

	cam_rmw(c, CAM_WIN_ISP, ISP_TG_VF_CON, TG_VF_CLR_MASK, TG_VF_ALWAYS_SET | (delay & 7) << TG_VF_DELAY_SHIFT);
	cam_rmw(c, CAM_WIN_ISP, ISP_TG_SEN_MODE, 0, TG_SEN_MODE_CMOS_EN);

	dev_info(c->dev, "TG: grab x %u..%u y %u..%u, SEN_MODE %#x VF_CON %#x PATH_CFG %#x FMT_SEL %#x\n", xs, xe,
		 ys, ye, cam_rd(c, CAM_WIN_ISP, ISP_TG_SEN_MODE), cam_rd(c, CAM_WIN_ISP, ISP_TG_VF_CON),
		 cam_rd(c, CAM_WIN_ISP, ISP_TG_PATH_CFG), cam_rd(c, CAM_WIN_ISP, ISP_CTL_FMT_SEL));
}

static void path_verify(struct cam_proto *c)
{
	size_t i, n;
	struct cam_path_reg *t = path_table(c, &n);

	for (i = 0; i < n; i++) {
		u32 v, bad;

		if (!t[i].base || !strcmp(t[i].name, "DMA"))
			continue;
		v = cam_rd(c, CAM_WIN_ISP, t[i].base);
		bad = (t[i].set & ~v) | (t[i].clr & ~t[i].set & v);
		if (bad)
			dev_warn(c->dev, "path %s: base reg %#x reads %#x, bits %#x differ from SET %#x / CLR %#x (set_clr_mode=%d)\n",
				 t[i].name, t[i].base, v, bad, t[i].set, t[i].clr, cam_knobs.set_clr_mode);
		else
			dev_info(c->dev, "path %s: base reg %#x reads %#x (SET %#x CLR %#x applied)\n", t[i].name,
				 t[i].base, v, t[i].set, t[i].clr);
	}
}

int cam_isp_configure(struct cam_proto *c)
{
	const struct cam_sensor *s = c->sensor;
	size_t i, n;
	struct cam_path_reg *t = path_table(c, &n);
	int pixid = CAM_KNOB(cam_knobs.pixid, s->pixid);
	u32 ph;

	dev_info(c->dev, "stage 3: ISP reset and pass-1 configuration for %s\n", s->name);
	isp_reset(c);
	ph = cam_rd(c, CAM_WIN_SENINF, SENINF1_TG_PH_CNT);
	if (!(ph & TG_PH_CNT_PCEN) || !(ph & TG_PH_CNT_ADCLK_EN))
		dev_warn(c->dev, "TG phase counter lost its MCLK setup after the reset: PH_CNT %#x\n", ph);

	cam_wr(c, CAM_WIN_ISP, ISP_CTL_EN1, ISP_EN1_DEFAULT);
	cam_rmw(c, CAM_WIN_ISP, ISP_CTL_EN2, ~ISP_EN2_KEEP_MASK, 0);
	cam_wr(c, CAM_WIN_ISP, ISP_CTL_DMA_EN, 0);
	cam_wr(c, CAM_WIN_ISP, ISP_REG_1600, 0);
	cam_wr(c, CAM_WIN_ISP, ISP_CTL_SEL, 0);

	cam_wr(c, CAM_WIN_ISP, ISP_CTL_INT_EN, 0);
	if (cam_knobs.dma_int_bits)
		cam_rmw(c, CAM_WIN_ISP, ISP_CTL_DMA_INT, 0, cam_knobs.dma_int_bits);
	cam_wr(c, CAM_WIN_ISP, ISP_REG_06C, ISP_V06C);
	cam_wr(c, CAM_WIN_ISP, ISP_REG_150, ISP_V150);
	cam_wr(c, CAM_WIN_ISP, ISP_REG_4F4, 0);
	cam_wr(c, CAM_WIN_ISP, ISP_REG_380, ISP_V380);
	cam_wr(c, CAM_WIN_ISP, ISP_REG_384, 0);
	cam_wr(c, CAM_WIN_ISP, ISP_REG_39C, ISP_V39C);
	cam_wr(c, CAM_WIN_ISP, ISP_REG_3A0, 0);
	cam_wr(c, CAM_WIN_ISP, ISP_REG_280, ISP_V280);
	cam_wr(c, CAM_WIN_ISP, ISP_REG_284, 0);
	cam_wr(c, CAM_WIN_ISP, ISP_DBG_SET, ISP_DBG_CQ_STATUS);

	tg_config(c);

	for (i = 0; i < n; i++)
		if (strcmp(t[i].name, "DMA"))
			path_apply(c, &t[i]);
	if (!s->yuv) {
		cam_wr(c, CAM_WIN_ISP, ISP_PIX_ID_SET, (pixid & 3) | PIX_ID_SET_BIT20);
		cam_wr(c, CAM_WIN_ISP, ISP_PIX_ID_CLR, ~pixid & 3);
	}
	path_verify(c);
	c->isp_cfg = true;
	dev_info(c->dev, "stage 3 done, DMA not started\n");
	return 0;
}

int cam_isp_path_write(struct cam_proto *c, const char *name, bool set, u32 val)
{
	size_t i, n;
	struct cam_path_reg *t = path_table(c, &n);

	for (i = 0; i < n; i++) {
		if (strcmp(t[i].name, name))
			continue;
		if (set)
			t[i].set = val;
		else
			t[i].clr = val;
		return 0;
	}
	return -ENOENT;
}

void cam_isp_path_show(struct cam_proto *c, char *buf, size_t sz, size_t *np)
{
	size_t i, n;
	struct cam_path_reg *t = path_table(c, &n);

	SNPR(buf, sz, *np, "# %s path image; write \"<NAME>_SET|<NAME>_CLR <value>\" to change\n",
	     c->sensor && c->sensor->yuv ? "yuv422" : "raw10");
	for (i = 0; i < n; i++)
		SNPR(buf, sz, *np, "%s_SET %#010x\n%s_CLR %#010x\n", t[i].name, t[i].set, t[i].name, t[i].clr);
}

/* IMGO geometry (DMAO_B::_config). */
static void imgo_geometry(struct cam_proto *c, u32 *xsize, u32 *stride)
{
	u32 w, h, pb, stride_px;

	cam_geometry(c, &w, &h);
	pb = cam_knobs.imgo_pb > 0 ? cam_knobs.imgo_pb : (c->sensor->yuv ? 8 : 5);
	stride_px = cam_knobs.imgo_stride_px > 0 ? cam_knobs.imgo_stride_px :
		    (c->sensor->yuv ? w : roundup(w, 8));
	*xsize = ((((w * pb + 3) >> 2) + 1) & ~1U) - 1;
	*stride = (stride_px * pb) >> 2;
}

int cam_isp_prepare_frame(struct cam_proto *c)
{
	u32 w, h, xsize, stride;
	size_t need;

	cam_geometry(c, &w, &h);
	imgo_geometry(c, &xsize, &stride);
	need = (size_t)stride * h;
	if (stride < xsize + 1) {
		dev_err(c->dev, "stride %u smaller than line size %u\n", stride, xsize + 1);
		return -EINVAL;
	}

	if (!c->frame_cpu || c->frame_alloc < need) {
		cam_isp_free_frame(c);
		c->frame_alloc = PAGE_ALIGN(need);
		c->frame_cpu = dma_alloc_coherent(c->dev, c->frame_alloc, &c->frame_dma, GFP_KERNEL);
		if (!c->frame_cpu) {
			c->frame_alloc = 0;
			dev_err(c->dev, "cannot allocate a %zu byte DMA buffer\n", need);
			return -ENOMEM;
		}
		dev_info(c->dev, "frame buffer %zu bytes, dma %pad\n", c->frame_alloc, &c->frame_dma);
	}
	c->frame_size = need;
	c->frame_w = w;
	c->frame_h = h;
	c->frame_stride = stride;
	c->frame_fmt = c->sensor->yuv ? "yuv422 (byte order as the sensor: UYVY, unverified)" :
			"raw10 packed (MTK IMGO: 4 pixels in 5 bytes, unverified)";
	return 0;
}

void cam_isp_free_frame(struct cam_proto *c)
{
	if (c->frame_cpu)
		dma_free_coherent(c->dev, c->frame_alloc, c->frame_cpu, c->frame_dma);
	c->frame_cpu = NULL;
	c->frame_alloc = 0;
	c->frame_size = 0;
	c->frame_valid = false;
}

static void imgo_program(struct cam_proto *c)
{
	u32 xsize, stride, w, h;

	cam_geometry(c, &w, &h);
	imgo_geometry(c, &xsize, &stride);
	cam_wr(c, CAM_WIN_ISP, ISP_IMGO_OFST_ADDR, 0);
	cam_wr(c, CAM_WIN_ISP, ISP_IMGO_XSIZE, xsize);
	cam_wr(c, CAM_WIN_ISP, ISP_IMGO_YSIZE, h - 1);
	cam_wr(c, CAM_WIN_ISP, ISP_IMGO_STRIDE, stride);
	cam_wr(c, CAM_WIN_ISP, ISP_IMGO_CON, CAM_KNOB(cam_knobs.imgo_con, IMGO_CON_DEFAULT));
	cam_wr(c, CAM_WIN_ISP, ISP_IMGO_CON2, CAM_KNOB(cam_knobs.imgo_con2, IMGO_CON2_DEFAULT));
	cam_wr(c, CAM_WIN_ISP, ISP_IMGO_CROP, 0);
	if (cam_knobs.imgo_base_mode & 1)
		cam_wr(c, CAM_WIN_ISP, ISP_IMGO_BASE_ADDR, lower_32_bits(c->frame_dma));
	dev_info(c->dev, "IMGO: XSIZE %u YSIZE %u STRIDE %u CON %#x CON2 %#x BASE %#x (dma %pad, mode %d)\n", xsize,
		 h - 1, stride, cam_rd(c, CAM_WIN_ISP, ISP_IMGO_CON), cam_rd(c, CAM_WIN_ISP, ISP_IMGO_CON2),
		 cam_rd(c, CAM_WIN_ISP, ISP_IMGO_BASE_ADDR), &c->frame_dma, cam_knobs.imgo_base_mode);
}

static void fbc_init(struct cam_proto *c)
{
	if (!cam_knobs.fbc_mode)
		return;
	cam_rmw(c, CAM_WIN_ISP, ISP_IMGO_FBC, 0, FBC_DROP_INT_EN);
	cam_rmw(c, CAM_WIN_ISP, ISP_IMGO_FBC, FBC_FB_NUM_MASK, 1U << FBC_FB_NUM_SHIFT);
	cam_rmw(c, CAM_WIN_ISP, ISP_IMGO_FBC, FBC_EN, 0);
	udelay(FBC_POLL_US);
	cam_rmw(c, CAM_WIN_ISP, ISP_IMGO_FBC, 0, FBC_EN);
}

static void accumulate(struct cam_proto *c, u32 st, u32 dma)
{
	atomic_or(st, &c->int_acc);
	atomic_or(dma, &c->dma_acc);
}

static void poll_status(struct cam_proto *c)
{
	accumulate(c, cam_rd(c, CAM_WIN_ISP, ISP_CTL_INT_STATUS), cam_rd(c, CAM_WIN_ISP, ISP_CTL_DMA_INT));
}

irqreturn_t cam_isp_irq(int irq, void *data)
{
	struct cam_proto *c = data;
	u32 st = cam_rd(c, CAM_WIN_ISP, ISP_CTL_INT_STATUS);
	u32 dma = cam_rd(c, CAM_WIN_ISP, ISP_CTL_DMA_INT);

	if (!st && !dma)
		return IRQ_NONE;

	accumulate(c, st, dma);
	atomic_inc(&c->irq_count);
	if ((st & INT_SOF1) && (cam_knobs.imgo_base_mode & 2))
		cam_wr(c, CAM_WIN_ISP, ISP_IMGO_BASE_ADDR, lower_32_bits(c->frame_dma));
	if ((st & INT_PASS1_TG1_DON) && (cam_knobs.imgo_base_mode & 4))
		cam_wr(c, CAM_WIN_ISP, ISP_IMGO_BASE_ADDR, lower_32_bits(c->frame_dma));
	if (st & ((u32)cam_knobs.done_mask | INT_ERR_MASK))
		complete(&c->frame_done);
	return IRQ_HANDLED;
}

static int wait_frame(struct cam_proto *c)
{
	unsigned long end = jiffies + msecs_to_jiffies(cam_knobs.capture_timeout_ms);
	u32 stop = (u32)cam_knobs.done_mask | INT_ERR_MASK;

	if (c->irq_req) {
		if (!wait_for_completion_timeout(&c->frame_done, msecs_to_jiffies(cam_knobs.capture_timeout_ms)))
			return -ETIMEDOUT;
	} else {
		while (!(atomic_read(&c->int_acc) & stop)) {
			if (time_after(jiffies, end))
				return -ETIMEDOUT;
			usleep_range(500, 1000);
			poll_status(c);
		}
	}
	if (atomic_read(&c->int_acc) & INT_ERR_MASK)
		return -EIO;
	return 0;
}

/* Stop: TG, FBC, DMA enable, interrupt enable; wait (bounded) for the TG to go idle. */
static void capture_stop(struct cam_proto *c)
{
	u32 v;
	size_t i, n;
	struct cam_path_reg *t = path_table(c, &n);

	cam_rmw(c, CAM_WIN_ISP, ISP_TG_VF_CON, TG_VF_VFDATA_EN, 0);
	cam_wr(c, CAM_WIN_ISP, ISP_CTL_INT_EN, 0);
	if (cam_knobs.fbc_mode)
		cam_rmw(c, CAM_WIN_ISP, ISP_IMGO_FBC, FBC_EN, 0);
	for (i = 0; i < n; i++)
		if (!strcmp(t[i].name, "DMA"))
			isp_setclr(c, &t[i], 0, IMGO_DMA_EN | IMGO_DMA_EXTRA);
	if (readl_poll_timeout(c->base[CAM_WIN_ISP] + ISP_TG_INTER_ST, v,
			       FIELD_GET(TG_INTER_ST_MASK, v) == TG_INTER_ST_IDLE, 1000, 200000))
		dev_warn(c->dev, "TG_INTER_ST did not reach idle (%#x), continuing\n", v);
	poll_status(c);
}

void cam_isp_quiesce(struct cam_proto *c)
{
	if (!c->clk_on || !c->isp_cfg)
		return;
	capture_stop(c);
	c->isp_cfg = false;
}

static void frame_stats(struct cam_proto *c)
{
	const u8 *p = c->frame_cpu;
	u64 sum = 0;
	size_t i, changed = 0;
	u8 mn = 255, mx = 0, fill = cam_knobs.frame_fill;

	for (i = 0; i < c->frame_size; i++) {
		if (p[i] != fill)
			changed++;
		sum += p[i];
		mn = min(mn, p[i]);
		mx = max(mx, p[i]);
	}
	c->frame_valid = changed > 0;
	snprintf(c->frame_stats, sizeof(c->frame_stats), "bytes_changed=%zu/%zu min=%u max=%u mean=%llu first=%8phN",
		 changed, c->frame_size, mn, mx, c->frame_size ? div_u64(sum, c->frame_size) : 0, p);
}

int cam_isp_capture(struct cam_proto *c)
{
	size_t i, n;
	struct cam_path_reg *t = path_table(c, &n);
	int ret;

	if (!c->isp_cfg || !c->frame_cpu)
		return -EINVAL;

	if (cam_knobs.irq_mode && !c->irq_req) {
		ret = request_irq(c->irq, cam_isp_irq, 0, "mt8127-cam-proto", c);
		if (ret)
			dev_warn(c->dev, "cannot request IRQ %d (%d), polling instead\n", c->irq, ret);
		else
			c->irq_req = true;
	} else if (!cam_knobs.irq_mode && c->irq_req) {
		free_irq(c->irq, c);
		c->irq_req = false;
	}

	memset(c->frame_cpu, cam_knobs.frame_fill, c->frame_alloc);
	c->frame_valid = false;
	atomic_set(&c->int_acc, 0);
	atomic_set(&c->dma_acc, 0);
	atomic_set(&c->irq_count, 0);
	reinit_completion(&c->frame_done);

	imgo_program(c);
	for (i = 0; i < n; i++)
		if (!strcmp(t[i].name, "DMA"))
			path_apply(c, &t[i]);
	fbc_init(c);
	cam_wr(c, CAM_WIN_ISP, ISP_DBG_SET, ISP_DBG_CQ_STATUS);
	poll_status(c);
	atomic_set(&c->int_acc, 0);
	atomic_set(&c->dma_acc, 0);
	cam_wr(c, CAM_WIN_ISP, ISP_CTL_INT_EN, cam_knobs.int_en);

	dev_info(c->dev, "stage 4: starting view finder (%s, %s)\n", cam_knobs.vf_single ? "single shot" : "continuous",
		 c->irq_req ? "irq" : "poll");
	cam_rmw(c, CAM_WIN_ISP, ISP_TG_VF_CON, TG_VF_SINGLE_MODE, cam_knobs.vf_single ? TG_VF_SINGLE_MODE : 0);
	cam_rmw(c, CAM_WIN_ISP, ISP_TG_VF_CON, 0, TG_VF_VFDATA_EN);

	ret = wait_frame(c);
	capture_stop(c);
	if (c->irq_req)
		synchronize_irq(c->irq);

	frame_stats(c);
	dev_info(c->dev, "capture: ret %d INT_STATUS %#x DMA_INT %#x irqs %d; %s\n", ret, atomic_read(&c->int_acc),
		 atomic_read(&c->dma_acc), atomic_read(&c->irq_count), c->frame_stats);
	if (ret == -ETIMEDOUT)
		dev_err(c->dev, "no frame done interrupt within %d ms\n", cam_knobs.capture_timeout_ms);
	else if (ret == -EIO)
		dev_err(c->dev, "pass-1 error interrupt: INT_STATUS %#x\n", atomic_read(&c->int_acc));
	if (!ret && !c->frame_valid) {
		dev_err(c->dev, "frame done reported but the buffer is untouched\n");
		ret = -ENODATA;
	}
	c->capture_ret = ret;
	return ret;
}

void cam_isp_dump(struct cam_proto *c, char *buf, size_t sz, size_t *np)
{
	static const u16 offs[] = { 0x004, 0x008, 0x00c, 0x010, 0x018, 0x020, 0x05c, 0x0c0, 0x0c8, 0x0d0, 0x0d8,
				    0x0f4, 0x300, 0x304, 0x308, 0x30c, 0x310, 0x314, 0x318, 0x31c, 0x3a4, 0x3c4,
				    0x410, 0x414, 0x418, 0x41c, 0x420, 0x44c };
	unsigned int i;
	u32 st, ti;

	if (c->isp_cfg)
		poll_status(c);
	st = atomic_read(&c->int_acc);
	ti = cam_rd(c, CAM_WIN_ISP, ISP_TG_INTER_ST);
	SNPR(buf, sz, *np, "[isp] INT_STATUS (accumulated, read-clear) %#x: VS1 %d TG1_ERR %d PASS1_DON %d SOF1 %d IMGO_ERR %d DMA_ERR %d\n",
	     st, !!(st & INT_VS1), !!(st & INT_TG1_ERR), !!(st & INT_PASS1_TG1_DON), !!(st & INT_SOF1),
	     !!(st & INT_IMGO_ERR), !!(st & INT_DMA_ERR));
	SNPR(buf, sz, *np, "  DMA_INT (accumulated) %#x, irq count %d, TG state %u (1 = idle)\n",
	     atomic_read(&c->dma_acc), atomic_read(&c->irq_count), (u32)FIELD_GET(TG_INTER_ST_MASK, ti));
	SNPR(buf, sz, *np, "  IMGO FBC %#x (FBC_CNT %u), DMA error status %#x\n", cam_rd(c, CAM_WIN_ISP, ISP_IMGO_FBC),
	     (u32)(cam_rd(c, CAM_WIN_ISP, ISP_IMGO_FBC) & FBC_CNT_MASK), cam_rd(c, CAM_WIN_ISP, ISP_IMGO_ERR_STAT));
	for (i = 0; i < ARRAY_SIZE(offs); i++)
		SNPR(buf, sz, *np, "isp+%#05x = %#010x\n", offs[i], cam_rd(c, CAM_WIN_ISP, offs[i]));
}
