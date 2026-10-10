// SPDX-License-Identifier: GPL-2.0
/*
 * Camera power and clocks: ISP power domain, IMG clock gates, camtg mux, TG phase counter and the 24 MHz sensor
 * master clock exported as a clock provider. The order and the values follow tools/camera-probe.sh.
 *
 * The master clock is prepared by taking a runtime PM reference (domain, gates and camtg mux on) and gated by the
 * ADCLK_EN bit of the TG phase counter.
 */
#include <linux/clk-provider.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>

#include "mtk_camsys.h"

#define DOMAIN_TIMEOUT_US	20000
#define MCLK_RATE		24000000
#define MCLK_IO_DRIVE		0x6000

#define to_cam(hw)		container_of(hw, struct mtk_camsys, mclk_hw)

static bool domain_is_on(struct mtk_camsys *cam)
{
	return readl(cam->spm + SPM_PWR_STATUS) & ISP_PWR_STA_MASK;
}

static int domain_on(struct mtk_camsys *cam)
{
	void __iomem *spm = cam->spm;
	u32 v;
	int ret;

	if (domain_is_on(cam))
		return 0;

	mtk_camsys_rmw(spm, SPM_ISP_PWR_CON, 0, PWR_ON);
	mtk_camsys_rmw(spm, SPM_ISP_PWR_CON, 0, PWR_ON_S);
	ret = readl_poll_timeout(spm + SPM_PWR_STATUS, v, v & ISP_PWR_STA_MASK, 10, DOMAIN_TIMEOUT_US);
	if (!ret)
		ret = readl_poll_timeout(spm + SPM_PWR_STATUS_S, v, v & ISP_PWR_STA_MASK, 10,
					 DOMAIN_TIMEOUT_US);
	if (ret) {
		dev_err(cam->dev, "ISP power domain did not come up\n");
		mtk_camsys_rmw(spm, SPM_ISP_PWR_CON, PWR_ON | PWR_ON_S, 0);
		return -ETIMEDOUT;
	}

	mtk_camsys_rmw(spm, SPM_ISP_PWR_CON, PWR_CLK_DIS, 0);
	mtk_camsys_rmw(spm, SPM_ISP_PWR_CON, PWR_ISO, 0);
	mtk_camsys_rmw(spm, SPM_ISP_PWR_CON, 0, PWR_RST_B);
	mtk_camsys_rmw(spm, SPM_ISP_PWR_CON, PWR_SRAM_PDN, 0);
	if (readl_poll_timeout(spm + SPM_ISP_PWR_CON, v, !(v & PWR_SRAM_ACK), 10, DOMAIN_TIMEOUT_US))
		dev_warn(cam->dev, "ISP SRAM power-up ack still set, continuing\n");

	cam->domain_ours = true;
	return 0;
}

static void domain_off(struct mtk_camsys *cam)
{
	void __iomem *spm = cam->spm;
	u32 v;

	if (!cam->domain_ours)
		return;

	mtk_camsys_rmw(spm, SPM_ISP_PWR_CON, 0, PWR_SRAM_PDN);
	if (readl_poll_timeout(spm + SPM_ISP_PWR_CON, v, (v & PWR_SRAM_ACK) == PWR_SRAM_ACK, 10,
			       DOMAIN_TIMEOUT_US))
		dev_warn(cam->dev, "ISP SRAM power-down ack missing, continuing\n");
	mtk_camsys_rmw(spm, SPM_ISP_PWR_CON, 0, PWR_ISO);
	mtk_camsys_rmw(spm, SPM_ISP_PWR_CON, PWR_RST_B, PWR_CLK_DIS);
	mtk_camsys_rmw(spm, SPM_ISP_PWR_CON, PWR_ON | PWR_ON_S, 0);
	if (readl_poll_timeout(spm + SPM_PWR_STATUS, v, !(v & ISP_PWR_STA_MASK), 10, DOMAIN_TIMEOUT_US))
		dev_warn(cam->dev, "ISP power domain still reports on\n");
	cam->domain_ours = false;
}

/* TG phase counter: pixel clock enable and the 48 MHz / 2 divider; the master clock gate is left alone. */
static void tg_clock_setup(struct mtk_camsys *cam)
{
	unsigned long flags;

	spin_lock_irqsave(&cam->tg_lock, flags);
	mtk_camsys_rmw(cam->seninf, SENINF_TOP_CTRL, 0, SENINF_TOP_TG1_CLK_EN);
	writel(TG_SEN_CK_24MHZ, cam->seninf + SENINF1_TG_SEN_CK);
	mtk_camsys_rmw(cam->seninf, SENINF1_TG_PH_CNT, TG_PH_CNT_CLR_MASK & ~TG_PH_CNT_ADCLK_EN,
		       TG_PH_CNT_PCEN | TG_PH_CNT_TGCLK_SEL | (cam->mclk_on ? TG_PH_CNT_ADCLK_EN : 0));
	spin_unlock_irqrestore(&cam->tg_lock, flags);
}

/* The ISP reset may clear the phase counter; put the clock setup back. */
void mtk_camsys_tg_clock_restore(struct mtk_camsys *cam)
{
	u32 ph = readl(cam->seninf + SENINF1_TG_PH_CNT);

	if ((ph & (TG_PH_CNT_PCEN | TG_PH_CNT_TGCLK_SEL)) != (TG_PH_CNT_PCEN | TG_PH_CNT_TGCLK_SEL) ||
	    !!(ph & TG_PH_CNT_ADCLK_EN) != cam->mclk_on)
		tg_clock_setup(cam);
}

int mtk_camsys_pm_resume(struct device *dev)
{
	struct mtk_camsys *cam = dev_get_drvdata(dev);
	u32 cg;
	int ret;

	ret = domain_on(cam);
	if (ret)
		return ret;

	writel(IMG_CG_CAM_MASK, cam->imgsys + IMG_CG_CLR);
	cg = readl(cam->imgsys + IMG_CG_CON);
	if (cg & IMG_CG_CAM_MASK) {
		dev_err(dev, "IMG clock gates did not open: IMG_CG_CON %#x\n", cg);
		domain_off(cam);
		return -EIO;
	}

	mtk_camsys_rmw(cam->topckgen, TOP_CLK_CFG_1, CAMTG_PDN | CAMTG_SEL_MASK, CAMTG_SEL_UNIVPLL_D26);
	tg_clock_setup(cam);
	mtk_camsys_rmw(cam->isp, ISP_TG_SEN_MODE, 0, TG_SEN_MODE_CMOS_EN);

	/* CMMCLK pad (GPIO119) function 1; pins 115..118 keep their fields. */
	mtk_camsys_rmw(cam->gpio, GPIO_MODE_115_119, GENMASK(15, 12), 1U << 12);
	mtk_camsys_rmw(cam->gpio, GPIO_DRV_117_120, GPIO_DRV_117_120_MASK, MCLK_IO_DRIVE);
	return 0;
}

int mtk_camsys_pm_suspend(struct device *dev)
{
	struct mtk_camsys *cam = dev_get_drvdata(dev);

	mtk_camsys_rmw(cam->seninf, SENINF1_TG_PH_CNT, TG_PH_CNT_PCEN | TG_PH_CNT_ADCLK_EN, 0);
	mtk_camsys_rmw(cam->topckgen, TOP_CLK_CFG_1, 0, CAMTG_PDN);
	writel(IMG_CG_CAM_MASK, cam->imgsys + IMG_CG_SET);
	domain_off(cam);
	return 0;
}

static int mclk_prepare(struct clk_hw *hw)
{
	return pm_runtime_resume_and_get(to_cam(hw)->dev);
}

static void mclk_unprepare(struct clk_hw *hw)
{
	pm_runtime_put_sync(to_cam(hw)->dev);
}

static int mclk_enable(struct clk_hw *hw)
{
	struct mtk_camsys *cam = to_cam(hw);
	unsigned long flags;

	spin_lock_irqsave(&cam->tg_lock, flags);
	mtk_camsys_rmw(cam->seninf, SENINF1_TG_PH_CNT, 0, TG_PH_CNT_ADCLK_EN);
	cam->mclk_on = true;
	spin_unlock_irqrestore(&cam->tg_lock, flags);
	return 0;
}

static void mclk_disable(struct clk_hw *hw)
{
	struct mtk_camsys *cam = to_cam(hw);
	unsigned long flags;

	spin_lock_irqsave(&cam->tg_lock, flags);
	mtk_camsys_rmw(cam->seninf, SENINF1_TG_PH_CNT, TG_PH_CNT_ADCLK_EN, 0);
	cam->mclk_on = false;
	spin_unlock_irqrestore(&cam->tg_lock, flags);
}

static int mclk_is_enabled(struct clk_hw *hw)
{
	return to_cam(hw)->mclk_on;
}

static unsigned long mclk_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	return MCLK_RATE;
}

static const struct clk_ops mclk_ops = {
	.prepare = mclk_prepare,
	.unprepare = mclk_unprepare,
	.enable = mclk_enable,
	.disable = mclk_disable,
	.is_enabled = mclk_is_enabled,
	.recalc_rate = mclk_recalc_rate,
};

int mtk_camsys_clk_register(struct mtk_camsys *cam)
{
	struct clk_init_data init = {
		.ops = &mclk_ops,
	};
	int ret;

	init.name = devm_kasprintf(cam->dev, GFP_KERNEL, "%s-mclk", dev_name(cam->dev));
	if (!init.name)
		return -ENOMEM;

	cam->mclk_hw.init = &init;
	ret = devm_clk_hw_register(cam->dev, &cam->mclk_hw);
	if (ret)
		return dev_err_probe(cam->dev, ret, "cannot register the sensor master clock\n");

	return devm_of_clk_add_hw_provider(cam->dev, of_clk_hw_simple_get, &cam->mclk_hw);
}
