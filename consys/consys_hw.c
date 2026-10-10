// SPDX-License-Identifier: GPL-2.0
/*
 * MT8127 CONSYS platform driver: power sequencing, reserved-memory handshake
 * window and the hardware API used by the WMT core and the WiFi driver.
 */
#include <linux/bits.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>

#include "mtk_wcn_consys_hw.h"
#include "consys_priv.h"

#ifdef DFT_TAG
#undef DFT_TAG
#endif
#define DFT_TAG "[WMT-CONSYS-HW]"

/* SPM (reg resource "spm") */
#define SPM_CONN_PWR_CON	0x280
#define SPM_PWR_STATUS		0x60c
#define SPM_PWR_STATUS_S	0x610
#define CONN_PWR_STA		BIT(1)

#define PWR_RST_B		BIT(0)
#define PWR_ISO			BIT(1)
#define PWR_ON			BIT(2)
#define PWR_ON_S		BIT(3)
#define PWR_CLK_DIS		BIT(4)
#define PWR_SRAM_PDN		BIT(8)

/* INFRACFG_AO (syscon "mediatek,infracfg") */
#define INFRA_PDN_SET		0x40
#define INFRA_PDN_CLR		0x44
#define INFRA_CONNMCU_GATE	BIT(12)
#define INFRA_TOPAXI_PROT_EN	0x220
#define INFRA_TOPAXI_PROT_STA1	0x228
#define CONN_PROT_MASK		0x104
#define INFRA_EMI_MAPPING	0x310
#define INFRA_EMI_MAPPING_EN	BIT(12)
#define INFRA_AP2CONN_OSC_EN	0x800
#define AP2CONN_WAKEUP_BIT	BIT(17)

/* CONN_MCU_CONFIG (reg resource "mcu-config") */
#define MCU_CHIP_ID		0x08
#define MCU_CPUPCR		0x160

struct consys_hw {
	struct device *dev;
	void __iomem *spm;
	void __iomem *mcu;
	struct regmap *infra;
	void __iomem *emi;
	phys_addr_t emi_phys;
	struct regulator *vcn18;
	struct regulator *vcn28;
	struct regulator *vcn33_wifi;
	struct regulator *vcn33_bt;
	bool vcn18_on;
	bool vcn28_on;
	bool wifi_on;
	bool bt_on;
	bool domain_on;
	int bgf_irq;
	u32 jtag;
	struct mutex lock;
	struct dentry *dbg;
	struct debugfs_blob_wrapper emi_blob;
	struct debugfs_blob_wrapper emi_full_blob;
	void __iomem *emi_full;
};

static struct consys_hw *chw;

static void spm_rmw(struct consys_hw *h, u32 off, u32 clr, u32 set)
{
	u32 v = readl(h->spm + off);

	writel((v & ~clr) | set, h->spm + off);
}

static int consys_reg_set(struct regulator *r, bool *state, bool on)
{
	int ret;

	if (on == *state)
		return 0;
	ret = on ? regulator_enable(r) : regulator_disable(r);
	if (!ret)
		*state = on;
	return ret;
}

static bool conn_domain_acked(struct consys_hw *h, bool on)
{
	u32 s = readl(h->spm + SPM_PWR_STATUS);
	u32 ss = readl(h->spm + SPM_PWR_STATUS_S);

	if (on)
		return (s & CONN_PWR_STA) && (ss & CONN_PWR_STA);
	return !(s & CONN_PWR_STA) && !(ss & CONN_PWR_STA);
}

static int conn_domain_ctrl(struct consys_hw *h, bool on)
{
	int i;

	if (!on) {
		regmap_set_bits(h->infra, INFRA_TOPAXI_PROT_EN, CONN_PROT_MASK);
		for (i = 0; i < 1000; i++) {
			unsigned int sta;

			regmap_read(h->infra, INFRA_TOPAXI_PROT_STA1, &sta);
			if ((sta & CONN_PROT_MASK) == CONN_PROT_MASK)
				break;
			udelay(1);
		}

		spm_rmw(h, SPM_CONN_PWR_CON, 0, PWR_SRAM_PDN);
		spm_rmw(h, SPM_CONN_PWR_CON, 0, PWR_ISO);
		spm_rmw(h, SPM_CONN_PWR_CON, PWR_RST_B, PWR_CLK_DIS);
		spm_rmw(h, SPM_CONN_PWR_CON, PWR_ON | PWR_ON_S, 0);
	} else {
		spm_rmw(h, SPM_CONN_PWR_CON, 0, PWR_ON);
		spm_rmw(h, SPM_CONN_PWR_CON, 0, PWR_ON_S);
	}

	for (i = 0; i < 1000; i++) {
		if (conn_domain_acked(h, on))
			break;
		udelay(10);
	}
	if (i == 1000) {
		dev_err(h->dev, "CONN power domain %s timeout\n", on ? "up" : "down");
		return -ETIMEDOUT;
	}

	if (on) {
		spm_rmw(h, SPM_CONN_PWR_CON, PWR_CLK_DIS, 0);
		spm_rmw(h, SPM_CONN_PWR_CON, PWR_ISO, 0);
		spm_rmw(h, SPM_CONN_PWR_CON, 0, PWR_RST_B);
		spm_rmw(h, SPM_CONN_PWR_CON, PWR_SRAM_PDN, 0);

		regmap_clear_bits(h->infra, INFRA_TOPAXI_PROT_EN, CONN_PROT_MASK);
		for (i = 0; i < 1000; i++) {
			unsigned int sta;

			regmap_read(h->infra, INFRA_TOPAXI_PROT_STA1, &sta);
			if (!(sta & CONN_PROT_MASK))
				break;
			udelay(1);
		}
	}

	h->domain_on = on;
	return 0;
}

static bool chip_id_valid(u32 id)
{
	return id == 0x8127 || id == 0x6582 || id == 0x6572;
}

static int consys_hw_power_up(struct consys_hw *h, u32 co_clock_en)
{
	unsigned int retry = 10;
	u32 id = 0;
	int ret;

	/* VCN18: leave low-power mode first; boards without mode support keep the default. */
	if (regulator_set_mode(h->vcn18, REGULATOR_MODE_NORMAL))
		dev_dbg(h->dev, "vcn18 mode switch not available\n");
	ret = consys_reg_set(h->vcn18, &h->vcn18_on, true);
	if (ret)
		return ret;
	udelay(150);

	/* In co-clock mode VCN28 is only switched on when FM/GPS start. */
	if (!co_clock_en) {
		ret = consys_reg_set(h->vcn28, &h->vcn28_on, true);
		if (ret)
			goto err_vcn18;
	}

	ret = conn_domain_ctrl(h, true);
	if (ret)
		goto err_vcn28;

	/* The 26 MHz clock is stable 10 us after the domain is up. */
	udelay(10);
	regmap_write(h->infra, INFRA_PDN_CLR, INFRA_CONNMCU_GATE);

	while (retry-- > 0) {
		id = readl(h->mcu + MCU_CHIP_ID);
		if (chip_id_valid(id))
			break;
		msleep(20);
	}
	if (!chip_id_valid(id)) {
		dev_err(h->dev, "unexpected CONSYS chip id 0x%08x\n", id);
		ret = -ENODEV;
		goto err_domain;
	}
	dev_info(h->dev, "CONSYS chip id 0x%08x\n", id);
	msleep(5);
	return 0;

err_domain:
	regmap_write(h->infra, INFRA_PDN_SET, INFRA_CONNMCU_GATE);
	conn_domain_ctrl(h, false);
err_vcn28:
	consys_reg_set(h->vcn28, &h->vcn28_on, false);
err_vcn18:
	consys_reg_set(h->vcn18, &h->vcn18_on, false);
	return ret;
}

static void consys_hw_power_down(struct consys_hw *h)
{
	if (h->domain_on) {
		regmap_write(h->infra, INFRA_PDN_SET, INFRA_CONNMCU_GATE);
		conn_domain_ctrl(h, false);
	}
	consys_reg_set(h->vcn28, &h->vcn28_on, false);
	consys_reg_set(h->vcn18, &h->vcn18_on, false);
}

INT32 mtk_wcn_consys_hw_reg_ctrl(UINT32 on, UINT32 co_clock_en)
{
	struct consys_hw *h = chw;
	int ret = 0;

	if (!h)
		return -ENODEV;

	WMT_PLAT_INFO_FUNC("CONSYS-HW-REG-CTRL(0x%08x),start\n", on);
	mutex_lock(&h->lock);
	if (on) {
		if (!h->domain_on)
			ret = consys_hw_power_up(h, co_clock_en);
	} else {
		consys_hw_power_down(h);
	}
	mutex_unlock(&h->lock);
	WMT_PLAT_INFO_FUNC("CONSYS-HW-REG-CTRL(0x%08x),finish(%d)\n", on, ret);
	return ret;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_reg_ctrl);

/* No GPIO is involved on this board: only the BGF wake IRQ is set up. */
static INT32 consys_hw_gpio_ctrl(UINT32 on)
{
	INT32 ret = 0;

	if (on) {
		ret += wmt_plat_gpio_ctrl(PIN_GPS_SYNC, PIN_STA_INIT);
		ret += wmt_plat_gpio_ctrl(PIN_GPS_LNA, PIN_STA_INIT);
		ret += wmt_plat_gpio_ctrl(PIN_I2S_GRP, PIN_STA_INIT);
		ret += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_INIT);
		ret += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_EINT_DIS);
	} else {
		ret += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_EINT_DIS);
		ret += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_DEINIT);
		ret += wmt_plat_gpio_ctrl(PIN_GPS_SYNC, PIN_STA_DEINIT);
		ret += wmt_plat_gpio_ctrl(PIN_I2S_GRP, PIN_STA_DEINIT);
		ret += wmt_plat_gpio_ctrl(PIN_GPS_LNA, PIN_STA_DEINIT);
	}
	return ret;
}

INT32 mtk_wcn_consys_hw_pwr_on(UINT32 co_clock_en)
{
	INT32 ret;

	ret = mtk_wcn_consys_hw_reg_ctrl(1, co_clock_en);
	if (ret)
		return ret;
	ret = consys_hw_gpio_ctrl(1);
#if CONSYS_ENALBE_SET_JTAG
	if (chw && chw->jtag)
		WMT_PLAT_WARN_FUNC("MCU JTAG pin setup is not supported\n");
#endif
	return ret;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_pwr_on);

INT32 mtk_wcn_consys_hw_pwr_off(VOID)
{
	INT32 ret = 0;

	ret += mtk_wcn_consys_hw_reg_ctrl(0, 0);
	ret += consys_hw_gpio_ctrl(0);
	return ret;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_pwr_off);

INT32 mtk_wcn_consys_hw_rst(UINT32 co_clock_en)
{
	INT32 ret = 0;

	ret += mtk_wcn_consys_hw_reg_ctrl(0, co_clock_en);
	ret += mtk_wcn_consys_hw_reg_ctrl(1, co_clock_en);
	return ret;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_rst);

static INT32 consys_vcn33_ctrl(struct regulator *r, bool *state, UINT32 enable)
{
	struct consys_hw *h = chw;
	int ret;

	if (!h)
		return -ENODEV;

	mutex_lock(&h->lock);
	if (enable)
		regulator_set_voltage(r, 3300000, 3300000);
	ret = consys_reg_set(r, state, !!enable);
	mutex_unlock(&h->lock);
	return ret;
}

INT32 mtk_wcn_consys_hw_bt_paldo_ctrl(UINT32 enable)
{
	if (!chw)
		return -ENODEV;
	return consys_vcn33_ctrl(chw->vcn33_bt, &chw->bt_on, enable);
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_bt_paldo_ctrl);

INT32 mtk_wcn_consys_hw_wifi_paldo_ctrl(UINT32 enable)
{
	if (!chw)
		return -ENODEV;
	return consys_vcn33_ctrl(chw->vcn33_wifi, &chw->wifi_on, enable);
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_wifi_paldo_ctrl);

INT32 mtk_wcn_consys_hw_vcn28_ctrl(UINT32 enable)
{
	struct consys_hw *h = chw;
	int ret;

	if (!h)
		return -ENODEV;

	mutex_lock(&h->lock);
	ret = consys_reg_set(h->vcn28, &h->vcn28_on, !!enable);
	mutex_unlock(&h->lock);
	return ret;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_vcn28_ctrl);

INT32 mtk_wcn_consys_hw_state_show(VOID)
{
	struct consys_hw *h = chw;

	if (!h)
		return -ENODEV;
	WMT_PLAT_INFO_FUNC("domain=%d vcn18=%d vcn28=%d vcn33_wifi=%d vcn33_bt=%d\n",
			   h->domain_on, h->vcn18_on, h->vcn28_on, h->wifi_on, h->bt_on);
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_state_show);

/*
 * Map the reserved region into the CONSYS MCU address space (window index in the
 * low 12 bits, enable in bit 12) and clear the handshake area.
 */
static int consys_emi_setup(struct consys_hw *h)
{
	u32 val = (u32)(h->emi_phys >> 20) | INFRA_EMI_MAPPING_EN;
	unsigned int rb = 0;

	regmap_set_bits(h->infra, INFRA_EMI_MAPPING, val);
	regmap_read(h->infra, INFRA_EMI_MAPPING, &rb);
	WMT_PLAT_INFO_FUNC("CONSYS_EMI_MAPPING 0x%08x\n", rb);

	memset_io(h->emi, 0, CONSYS_EMI_MEM_SIZE);
	return 0;
}

INT32 mtk_wcn_consys_hw_init(VOID)
{
	if (!chw) {
		WMT_PLAT_ERR_FUNC("mt8127_consys device is not probed\n");
		return -ENODEV;
	}
	return consys_emi_setup(chw);
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_init);

INT32 mtk_wcn_consys_hw_deinit(VOID)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_deinit);

UINT8 *mtk_wcn_consys_emi_virt_addr_get(UINT32 ctrl_state_offset)
{
	if (!chw || !chw->emi) {
		WMT_PLAT_ERR_FUNC("EMI base address is NULL\n");
		return NULL;
	}
	if (ctrl_state_offset >= CONSYS_EMI_MEM_SIZE)
		return NULL;
	return (UINT8 *)chw->emi + ctrl_state_offset;
}
EXPORT_SYMBOL(mtk_wcn_consys_emi_virt_addr_get);

#if CONSYS_ENALBE_SET_JTAG
UINT32 mtk_wcn_consys_jtag_flag_ctrl(UINT32 en)
{
	if (chw)
		chw->jtag = en;
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_jtag_flag_ctrl);
#endif

UINT32 mtk_wcn_consys_soc_chipid(VOID)
{
	return PLATFORM_SOC_CHIP;
}
EXPORT_SYMBOL(mtk_wcn_consys_soc_chipid);

u32 consys_hw_read_cpupcr(void)
{
	if (!chw || !chw->domain_on)
		return 0;
	return readl(chw->mcu + MCU_CPUPCR);
}

int consys_hw_pulse_ap2conn_wakeup(void)
{
	struct consys_hw *h = chw;

	if (!h || !h->domain_on)
		return -ENODEV;

	regmap_clear_bits(h->infra, INFRA_AP2CONN_OSC_EN, AP2CONN_WAKEUP_BIT);
	usleep_range(64, 96);
	regmap_set_bits(h->infra, INFRA_AP2CONN_OSC_EN, AP2CONN_WAKEUP_BIT);
	return 0;
}

int consys_hw_bgf_irq(void)
{
	return chw ? chw->bgf_irq : -ENODEV;
}

static int consys_dbg_power_set(void *data, u64 val)
{
	return mtk_wcn_consys_hw_reg_ctrl(!!val, 0);
}

static int consys_dbg_power_get(void *data, u64 *val)
{
	struct consys_hw *h = data;

	*val = h->domain_on;
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(consys_dbg_power_fops, consys_dbg_power_get,
			 consys_dbg_power_set, "%llu\n");

static int consys_dbg_chipid_get(void *data, u64 *val)
{
	struct consys_hw *h = data;

	if (!h->domain_on)
		return -ENODEV;
	*val = readl(h->mcu + MCU_CHIP_ID);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(consys_dbg_chipid_fops, consys_dbg_chipid_get, NULL, "0x%08llx\n");

static struct regulator *consys_get_supply(struct device *dev, const char *id)
{
	struct regulator *r = devm_regulator_get(dev, id);

	if (IS_ERR(r))
		dev_err_probe(dev, PTR_ERR(r), "supply %s\n", id);
	return r;
}

static int consys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *mem_np;
	struct resource *res, emi_res;
	struct consys_hw *h;
	int ret;

	if (chw)
		return -EBUSY;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->dev = dev;
	mutex_init(&h->lock);

	h->infra = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(h->infra))
		return dev_err_probe(dev, PTR_ERR(h->infra), "mediatek,infracfg\n");

	/* The SPM block is shared with the rest of the SoC, so it is not claimed exclusively. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "spm");
	if (!res)
		return dev_err_probe(dev, -EINVAL, "missing spm reg\n");
	h->spm = devm_ioremap(dev, res->start, resource_size(res));
	if (!h->spm)
		return -ENOMEM;

	h->mcu = devm_platform_ioremap_resource_byname(pdev, "mcu-config");
	if (IS_ERR(h->mcu))
		return PTR_ERR(h->mcu);

	h->bgf_irq = platform_get_irq_byname(pdev, "bgf");
	if (h->bgf_irq < 0)
		return h->bgf_irq;

	mem_np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!mem_np)
		return dev_err_probe(dev, -EINVAL, "missing memory-region\n");
	ret = of_address_to_resource(mem_np, 0, &emi_res);
	of_node_put(mem_np);
	if (ret)
		return ret;
	if (!IS_ALIGNED(emi_res.start, SZ_1M) || resource_size(&emi_res) < SZ_1M)
		return dev_err_probe(dev, -EINVAL,
				     "memory-region must be 1 MiB aligned and at least 1 MiB\n");
	h->emi_phys = emi_res.start;
	h->emi = devm_ioremap_wc(dev, h->emi_phys + CONSYS_EMI_AP_PHY_OFFSET,
				 CONSYS_EMI_MEM_SIZE);
	if (!h->emi)
		return -ENOMEM;

	h->vcn18 = consys_get_supply(dev, "vcn18");
	if (IS_ERR(h->vcn18))
		return PTR_ERR(h->vcn18);
	h->vcn28 = consys_get_supply(dev, "vcn28");
	if (IS_ERR(h->vcn28))
		return PTR_ERR(h->vcn28);
	h->vcn33_wifi = consys_get_supply(dev, "vcn33-wifi");
	if (IS_ERR(h->vcn33_wifi))
		return PTR_ERR(h->vcn33_wifi);
	h->vcn33_bt = consys_get_supply(dev, "vcn33-bt");
	if (IS_ERR(h->vcn33_bt))
		return PTR_ERR(h->vcn33_bt);

	ret = consys_emi_setup(h);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, h);
	h->dbg = debugfs_create_dir("mt8127_consys", NULL);
	debugfs_create_file_unsafe("power", 0644, h->dbg, h, &consys_dbg_power_fops);
	debugfs_create_file_unsafe("chipid", 0444, h->dbg, h, &consys_dbg_chipid_fops);
	/* The handshake and dump area shared with the CONSYS MCU, for reading the firmware trace. */
	h->emi_blob.data = (void __force *)h->emi;
	h->emi_blob.size = CONSYS_EMI_MEM_SIZE;
	debugfs_create_blob("emi", 0400, h->dbg, &h->emi_blob);
	h->emi_full = devm_ioremap_wc(h->dev, h->emi_phys, SZ_1M);
	if (h->emi_full) {
		h->emi_full_blob.data = (void __force *)h->emi_full;
		h->emi_full_blob.size = SZ_1M;
		debugfs_create_blob("emi_full", 0400, h->dbg, &h->emi_full_blob);
	}

	chw = h;
	return 0;
}

static void consys_remove(struct platform_device *pdev)
{
	struct consys_hw *h = platform_get_drvdata(pdev);

	debugfs_remove_recursive(h->dbg);
	mutex_lock(&h->lock);
	consys_hw_power_down(h);
	mutex_unlock(&h->lock);
	chw = NULL;
}

static const struct of_device_id consys_of_match[] = {
	{ .compatible = "mediatek,mt8127-consys" },
	{ }
};
MODULE_DEVICE_TABLE(of, consys_of_match);

static struct platform_driver consys_driver = {
	.probe = consys_probe,
	.remove = consys_remove,
	.driver = {
		.name = "mt8127-consys",
		.of_match_table = consys_of_match,
	},
};
module_platform_driver(consys_driver);

MODULE_DESCRIPTION("MT8127 CONSYS platform driver");
MODULE_LICENSE("GPL");
