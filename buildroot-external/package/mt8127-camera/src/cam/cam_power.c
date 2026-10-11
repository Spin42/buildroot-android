// SPDX-License-Identifier: GPL-2.0
/*
 * Stage 1: ISP power domain, IMG clock gates, camtg mux, 24 MHz MCLK, sensor rails and GPIO sequences.
 * The order and the values follow tools/camera-probe.sh, which was verified on the tablet.
 */
#include <linux/delay.h>
#include <linux/iopoll.h>

#include "cam_proto.h"

#define DOMAIN_TIMEOUT_US	20000

void cam_gpio_set(struct gpio_desc *d, int val)
{
	if (d)
		gpiod_set_value_cansleep(d, val);
}

/* The sequences use physical levels: the DT flags are GPIO_ACTIVE_HIGH. */
static void rst_main(struct cam_proto *c, int v) { cam_gpio_set(c->main_rst, v); }
static void pdn_main(struct cam_proto *c, int v) { cam_gpio_set(c->main_pdn, v); }
static void rst_sub(struct cam_proto *c, int v) { cam_gpio_set(c->sub_rst, v); }
static void pdn_sub(struct cam_proto *c, int v) { cam_gpio_set(c->sub_pdn, v); }

static bool isp_domain_is_on(struct cam_proto *c)
{
	return cam_rd(c, CAM_WIN_SPM, SPM_PWR_STATUS) & ISP_PWR_STA_MASK;
}

static int isp_domain_on(struct cam_proto *c)
{
	u32 v;
	int ret;

	if (isp_domain_is_on(c)) {
		dev_info(c->dev, "ISP power domain already on, left as is\n");
		return 0;
	}

	cam_rmw(c, CAM_WIN_SPM, SPM_ISP_PWR_CON, 0, PWR_ON);
	cam_rmw(c, CAM_WIN_SPM, SPM_ISP_PWR_CON, 0, PWR_ON_S);
	ret = readl_poll_timeout(c->base[CAM_WIN_SPM] + SPM_PWR_STATUS, v, v & ISP_PWR_STA_MASK, 10,
				 DOMAIN_TIMEOUT_US);
	if (!ret)
		ret = readl_poll_timeout(c->base[CAM_WIN_SPM] + SPM_PWR_STATUS_S, v, v & ISP_PWR_STA_MASK, 10,
					 DOMAIN_TIMEOUT_US);
	if (ret) {
		dev_err(c->dev, "ISP power domain did not come up (status %#x/%#x)\n",
			cam_rd(c, CAM_WIN_SPM, SPM_PWR_STATUS), cam_rd(c, CAM_WIN_SPM, SPM_PWR_STATUS_S));
		cam_rmw(c, CAM_WIN_SPM, SPM_ISP_PWR_CON, PWR_ON | PWR_ON_S, 0);
		return -ETIMEDOUT;
	}

	cam_rmw(c, CAM_WIN_SPM, SPM_ISP_PWR_CON, PWR_CLK_DIS, 0);
	cam_rmw(c, CAM_WIN_SPM, SPM_ISP_PWR_CON, PWR_ISO, 0);
	cam_rmw(c, CAM_WIN_SPM, SPM_ISP_PWR_CON, 0, PWR_RST_B);
	cam_rmw(c, CAM_WIN_SPM, SPM_ISP_PWR_CON, PWR_SRAM_PDN, 0);
	ret = readl_poll_timeout(c->base[CAM_WIN_SPM] + SPM_ISP_PWR_CON, v, !(v & PWR_SRAM_ACK), 10,
				 DOMAIN_TIMEOUT_US);
	if (ret)
		dev_warn(c->dev, "ISP SRAM power-up ack still set (PWR_CON %#x), continuing\n", v);

	c->domain_ours = true;
	dev_info(c->dev, "ISP power domain on (PWR_CON %#x)\n", cam_rd(c, CAM_WIN_SPM, SPM_ISP_PWR_CON));
	return 0;
}

static void isp_domain_off(struct cam_proto *c)
{
	u32 v;

	if (!c->domain_ours)
		return;
	if (cam_knobs.isp_domain_keep) {
		dev_info(c->dev, "ISP power domain kept on (isp_domain_keep)\n");
		return;
	}

	cam_rmw(c, CAM_WIN_SPM, SPM_ISP_PWR_CON, 0, PWR_SRAM_PDN);
	if (readl_poll_timeout(c->base[CAM_WIN_SPM] + SPM_ISP_PWR_CON, v, (v & PWR_SRAM_ACK) == PWR_SRAM_ACK, 10,
			       DOMAIN_TIMEOUT_US))
		dev_warn(c->dev, "ISP SRAM power-down ack missing (PWR_CON %#x), continuing\n", v);
	cam_rmw(c, CAM_WIN_SPM, SPM_ISP_PWR_CON, 0, PWR_ISO);
	cam_rmw(c, CAM_WIN_SPM, SPM_ISP_PWR_CON, PWR_RST_B, PWR_CLK_DIS);
	cam_rmw(c, CAM_WIN_SPM, SPM_ISP_PWR_CON, PWR_ON | PWR_ON_S, 0);
	if (readl_poll_timeout(c->base[CAM_WIN_SPM] + SPM_PWR_STATUS, v, !(v & ISP_PWR_STA_MASK), 10,
			       DOMAIN_TIMEOUT_US))
		dev_warn(c->dev, "ISP power domain still reports on (status %#x)\n", v);
	c->domain_ours = false;
	dev_info(c->dev, "ISP power domain off\n");
}

void cam_mclk(struct cam_proto *c, bool on)
{
	cam_rmw(c, CAM_WIN_SENINF, SENINF1_TG_PH_CNT, on ? 0 : TG_PH_CNT_ADCLK_EN, on ? TG_PH_CNT_ADCLK_EN : 0);
	c->mclk_on = on;
}

static int clocks_on(struct cam_proto *c)
{
	u32 cg;

	cam_wr(c, CAM_WIN_IMGSYS, IMG_CG_CLR, IMG_CG_CAM_MASK);
	cg = cam_rd(c, CAM_WIN_IMGSYS, IMG_CG_CON);
	if (cg & IMG_CG_CAM_MASK) {
		dev_err(c->dev, "IMG clock gates did not open: IMG_CG_CON %#x\n", cg);
		return -EIO;
	}
	c->clk_on = true;

	cam_rmw(c, CAM_WIN_TOPCKGEN, TOP_CLK_CFG_1, CAMTG_PDN | CAMTG_SEL_MASK, CAMTG_SEL_UNIVPLL_D26);
	cam_rmw(c, CAM_WIN_SENINF, SENINF_TOP_CTRL, 0, SENINF_TOP_TG1_CLK_EN);
	cam_wr(c, CAM_WIN_SENINF, SENINF1_TG_SEN_CK, TG_SEN_CK_24MHZ);
	cam_rmw(c, CAM_WIN_SENINF, SENINF1_TG_PH_CNT, TG_PH_CNT_CLR_MASK, TG_PH_CNT_PCEN | TG_PH_CNT_TGCLK_SEL);
	cam_rmw(c, CAM_WIN_ISP, ISP_TG_SEN_MODE, 0, TG_SEN_MODE_CMOS_EN);

	if (cam_knobs.mclk_pinmux)	/* CMMCLK pad (GPIO119) function 1; pins 115..118 keep their fields */
		cam_rmw(c, CAM_WIN_GPIO, GPIO_MODE_115_119, GENMASK(15, 12), 1U << 12);
	if (cam_knobs.io_drive >= 0)
		cam_rmw(c, CAM_WIN_GPIO, GPIO_DRV_117_120, GPIO_DRV_117_120_MASK,
			cam_knobs.io_drive & GPIO_DRV_117_120_MASK);

	dev_info(c->dev, "clocks on: IMG_CG %#x CLK_CFG_1 %#x PH_CNT %#x SEN_CK %#x\n", cg,
		 cam_rd(c, CAM_WIN_TOPCKGEN, TOP_CLK_CFG_1), cam_rd(c, CAM_WIN_SENINF, SENINF1_TG_PH_CNT),
		 cam_rd(c, CAM_WIN_SENINF, SENINF1_TG_SEN_CK));
	return 0;
}

static void clocks_off(struct cam_proto *c)
{
	if (!c->clk_on)
		return;
	cam_rmw(c, CAM_WIN_SENINF, SENINF1_TG_PH_CNT, TG_PH_CNT_PCEN | TG_PH_CNT_ADCLK_EN, 0);
	cam_rmw(c, CAM_WIN_TOPCKGEN, TOP_CLK_CFG_1, 0, CAMTG_PDN);
	cam_wr(c, CAM_WIN_IMGSYS, IMG_CG_SET, IMG_CG_CAM_MASK);
	c->clk_on = false;
	dev_info(c->dev, "clocks off\n");
}

static int rail(struct cam_proto *c, struct regulator *r, bool *flag, bool on)
{
	int ret;

	if (on == *flag)
		return 0;
	ret = on ? regulator_enable(r) : regulator_disable(r);
	if (ret) {
		dev_err(c->dev, "regulator %s failed: %d\n", on ? "enable" : "disable", ret);
		return ret;
	}
	*flag = on;
	return 0;
}

static void sensors_off(struct cam_proto *c)
{
	if (c->clk_on)
		cam_mclk(c, false);
	rail(c, c->vcama, &c->vcama_on, false);
	rail(c, c->vcamio, &c->vcamio_on, false);
	pdn_main(c, 1);
	rst_main(c, 0);
	pdn_sub(c, 1);
	rst_sub(c, 0);
}

/* Stock kd_camera_hw.c order for the rear SP2508. */
static int seq_sp2508(struct cam_proto *c)
{
	int ret;

	pdn_sub(c, 1);
	rst_sub(c, 0);
	cam_mclk(c, false);
	pdn_main(c, 0);
	msleep(1);
	rst_main(c, 0);
	msleep(5);
	ret = rail(c, c->vcama, &c->vcama_on, true);
	if (ret)
		return ret;
	msleep(5);
	ret = rail(c, c->vcamio, &c->vcamio_on, true);
	if (ret)
		return ret;
	msleep(1);
	cam_mclk(c, true);
	msleep(2);
	pdn_main(c, 0);
	msleep(1);
	pdn_main(c, 1);
	msleep(100);
	pdn_main(c, 0);
	msleep(20);
	rst_main(c, 1);
	msleep(5);
	return 0;
}

/* Stock kd_camera_hw.c order for the front SP0A20; its reset line is not toggled. */
static int seq_sp0a20(struct cam_proto *c)
{
	int ret;

	pdn_main(c, 1);
	rst_main(c, 0);
	cam_mclk(c, false);
	pdn_sub(c, 0);
	msleep(1);
	ret = rail(c, c->vcama, &c->vcama_on, true);
	if (ret)
		return ret;
	msleep(5);
	ret = rail(c, c->vcamio, &c->vcamio_on, true);
	if (ret)
		return ret;
	msleep(5);
	cam_mclk(c, true);
	msleep(5);
	pdn_sub(c, 0);
	msleep(1);
	pdn_sub(c, 1);
	msleep(100);
	pdn_sub(c, 0);
	msleep(5);
	return 0;
}

int cam_power_on(struct cam_proto *c)
{
	int ret;

	dev_info(c->dev, "stage 1: power for %s\n", c->sensor->name);

	ret = isp_domain_on(c);
	if (ret)
		return ret;
	ret = clocks_on(c);
	if (ret)
		return ret;

	sensors_off(c);
	msleep(20);
	ret = c->sensor->slot == CAM_SLOT_MAIN ? seq_sp2508(c) : seq_sp0a20(c);
	if (ret)
		return ret;
	dev_info(c->dev, "sensor rails and GPIOs sequenced, MCLK on (PH_CNT %#x)\n",
		 cam_rd(c, CAM_WIN_SENINF, SENINF1_TG_PH_CNT));
	return 0;
}

void cam_power_off(struct cam_proto *c)
{
	sensors_off(c);
	clocks_off(c);
	isp_domain_off(c);
}
