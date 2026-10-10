// SPDX-License-Identifier: GPL-2.0
/*
 * WMT platform glue: power control, BGF wake IRQ, wake lock and EMI handshake
 * helpers called by the WMT core.
 */
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/spinlock.h>

#include "mtk_wcn_consys_hw.h"
#include "consys_priv.h"

#ifdef DFT_TAG
#undef DFT_TAG
#endif
#define DFT_TAG "[WMT-PLAT]"

#define CONSYS_REG_READ(addr)		readl(addr)
#define CONSYS_REG_WRITE(addr, data)	writel(data, addr)

UINT32 wmtPlatLogLvl = WMT_PLAT_LOG_INFO;

static EMI_CTRL_STATE_OFFSET mtk_wcn_emi_state_off = {
	.emi_apmem_ctrl_state = EXP_APMEM_CTRL_STATE,
	.emi_apmem_ctrl_host_sync_state = EXP_APMEM_CTRL_HOST_SYNC_STATE,
	.emi_apmem_ctrl_host_sync_num = EXP_APMEM_CTRL_HOST_SYNC_NUM,
	.emi_apmem_ctrl_chip_sync_state = EXP_APMEM_CTRL_CHIP_SYNC_STATE,
	.emi_apmem_ctrl_chip_sync_num = EXP_APMEM_CTRL_CHIP_SYNC_NUM,
	.emi_apmem_ctrl_chip_sync_addr = EXP_APMEM_CTRL_CHIP_SYNC_ADDR,
	.emi_apmem_ctrl_chip_sync_len = EXP_APMEM_CTRL_CHIP_SYNC_LEN,
	.emi_apmem_ctrl_chip_print_buff_start = EXP_APMEM_CTRL_CHIP_PRINT_BUFF_START,
	.emi_apmem_ctrl_chip_print_buff_len = EXP_APMEM_CTRL_CHIP_PRINT_BUFF_LEN,
	.emi_apmem_ctrl_chip_print_buff_idx = EXP_APMEM_CTRL_CHIP_PRINT_BUFF_IDX,
	.emi_apmem_ctrl_chip_int_status = EXP_APMEM_CTRL_CHIP_INT_STATUS,
	.emi_apmem_ctrl_chip_paded_dump_end = EXP_APMEM_CTRL_CHIP_PAGED_DUMP_END,
	.emi_apmem_ctrl_host_outband_assert_w1 = EXP_APMEM_CTRL_HOST_OUTBAND_ASSERT_W1,
};

static CONSYS_EMI_ADDR_INFO mtk_wcn_emi_addr_info = {
	.emi_phy_addr = CONSYS_EMI_FW_PHY_BASE,
	.paged_trace_off = CONSYS_EMI_PAGED_TRACE_OFFSET,
	.paged_dump_off = CONSYS_EMI_PAGED_DUMP_OFFSET,
	.full_dump_off = CONSYS_EMI_FULL_DUMP_OFFSET,
	.p_emi_ctrl_state_offset = &mtk_wcn_emi_state_off,
};

static UINT32 gCoClockFlag;
static BGF_IRQ_BALANCE gbgfIrqBle = {
	.lock = __SPIN_LOCK_UNLOCKED(gbgfIrqBle.lock),
};
static bool bgf_irq_requested;

static irq_cb wmt_plat_bgf_irq_cb;
static device_audio_if_cb wmt_plat_audio_if_cb;
static func_ctrl_cb wmt_plat_func_ctrl_cb;
static thermal_query_ctrl_cb wmt_plat_thermal_query_ctrl_cb;
static deep_idle_ctrl_cb wmt_plat_deep_idle_ctrl_cb;

static DEFINE_MUTEX(wmt_wakelock_mutex);
static struct wakeup_source *wmt_ws;

INT32 wmt_plat_audio_ctrl(CMB_STUB_AIF_X state, CMB_STUB_AIF_CTRL ctrl)
{
	INT32 ret = 0;

	if (state >= CMB_STUB_AIF_MAX || ctrl >= CMB_STUB_AIF_CTRL_MAX)
		return -1;

	switch (state) {
	case CMB_STUB_AIF_0:
	case CMB_STUB_AIF_1:
		ret += wmt_plat_gpio_ctrl(PIN_I2S_GRP, PIN_STA_DEINIT);
		break;
	case CMB_STUB_AIF_2:
	case CMB_STUB_AIF_3:
		ret += wmt_plat_gpio_ctrl(PIN_I2S_GRP, PIN_STA_INIT);
		break;
	default:
		return -1;
	}

	if (ctrl == CMB_STUB_AIF_CTRL_EN) {
		if (wmt_plat_audio_if_cb)
			ret += wmt_plat_audio_if_cb(state, MTK_WCN_BOOL_FALSE);
		else
			ret -= 1;
	}
	return ret;
}
EXPORT_SYMBOL(wmt_plat_audio_ctrl);

static VOID wmt_plat_func_ctrl(UINT32 type, UINT32 on)
{
	if (wmt_plat_func_ctrl_cb)
		wmt_plat_func_ctrl_cb(on, type);
}

static long wmt_plat_thermal_ctrl(VOID)
{
	return wmt_plat_thermal_query_ctrl_cb ? wmt_plat_thermal_query_ctrl_cb() : 0;
}

static INT32 wmt_plat_deep_idle_ctrl(UINT32 ctrl)
{
	return wmt_plat_deep_idle_ctrl_cb ? wmt_plat_deep_idle_ctrl_cb(ctrl) : -1;
}

static irqreturn_t wmt_plat_bgf_irq_isr(int irq, void *arg)
{
#if CFG_WMT_PS_SUPPORT
	wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_EINT_DIS);
	if (wmt_plat_bgf_irq_cb)
		wmt_plat_bgf_irq_cb();
	else
		WMT_PLAT_WARN_FUNC("wmt_plat_bgf_irq_cb not registered\n");
#endif
	return IRQ_HANDLED;
}

VOID wmt_plat_irq_cb_reg(irq_cb bgf_irq_cb)
{
	wmt_plat_bgf_irq_cb = bgf_irq_cb;
}
EXPORT_SYMBOL(wmt_plat_irq_cb_reg);

VOID wmt_plat_aif_cb_reg(device_audio_if_cb aif_ctrl_cb)
{
	wmt_plat_audio_if_cb = aif_ctrl_cb;
}
EXPORT_SYMBOL(wmt_plat_aif_cb_reg);

VOID wmt_plat_func_ctrl_cb_reg(func_ctrl_cb subsys_func_ctrl)
{
	wmt_plat_func_ctrl_cb = subsys_func_ctrl;
}
EXPORT_SYMBOL(wmt_plat_func_ctrl_cb_reg);

VOID wmt_plat_thermal_ctrl_cb_reg(thermal_query_ctrl_cb thermal_query_ctrl)
{
	wmt_plat_thermal_query_ctrl_cb = thermal_query_ctrl;
}
EXPORT_SYMBOL(wmt_plat_thermal_ctrl_cb_reg);

VOID wmt_plat_deep_idle_ctrl_cb_reg(deep_idle_ctrl_cb deep_idle_ctrl)
{
	wmt_plat_deep_idle_ctrl_cb = deep_idle_ctrl;
}
EXPORT_SYMBOL(wmt_plat_deep_idle_ctrl_cb_reg);

INT32 wmt_plat_init(UINT32 co_clock_en)
{
	CMB_STUB_CB stub_cb = { };
	INT32 ret;

	gCoClockFlag = co_clock_en ? 1 : 0;

	stub_cb.size = sizeof(stub_cb);
	stub_cb.aif_ctrl_cb = wmt_plat_audio_ctrl;
	stub_cb.func_ctrl_cb = wmt_plat_func_ctrl;
	stub_cb.thermal_query_cb = wmt_plat_thermal_ctrl;
	stub_cb.deep_idle_ctrl_cb = wmt_plat_deep_idle_ctrl;
	ret = mtk_wcn_cmb_stub_reg(&stub_cb);
	if (ret)
		return ret;

	wmt_ws = wakeup_source_register(NULL, "wmtFuncCtrl");
	if (!wmt_ws) {
		mtk_wcn_cmb_stub_unreg();
		return -ENOMEM;
	}

	ret = mtk_wcn_consys_hw_init();
	if (ret) {
		wakeup_source_unregister(wmt_ws);
		wmt_ws = NULL;
		mtk_wcn_cmb_stub_unreg();
		return ret;
	}

	spin_lock_init(&gbgfIrqBle.lock);
	return 0;
}
EXPORT_SYMBOL(wmt_plat_init);

INT32 wmt_plat_deinit(VOID)
{
	INT32 ret;

	ret = mtk_wcn_cmb_stub_unreg();
	if (wmt_ws) {
		wakeup_source_unregister(wmt_ws);
		wmt_ws = NULL;
	}
	ret += mtk_wcn_consys_hw_deinit();
	return ret;
}
EXPORT_SYMBOL(wmt_plat_deinit);

INT32 wmt_plat_pwr_ctrl(ENUM_FUNC_STATE state)
{
	switch (state) {
	case FUNC_ON:
		return mtk_wcn_consys_hw_pwr_on(gCoClockFlag);
	case FUNC_OFF:
		return mtk_wcn_consys_hw_pwr_off();
	case FUNC_RST:
		return mtk_wcn_consys_hw_rst(gCoClockFlag);
	case FUNC_STAT:
		return mtk_wcn_consys_hw_state_show();
	default:
		WMT_PLAT_WARN_FUNC("invalid state(%d) in pwr_ctrl\n", state);
		return -1;
	}
}
EXPORT_SYMBOL(wmt_plat_pwr_ctrl);

INT32 wmt_plat_eirq_ctrl(ENUM_PIN_ID id, ENUM_PIN_STATE state)
{
	unsigned long flags;
	INT32 ret = 0;
	int irq;

	if (state != PIN_STA_INIT && state != PIN_STA_DEINIT &&
	    state != PIN_STA_EINT_EN && state != PIN_STA_EINT_DIS) {
		WMT_PLAT_WARN_FUNC("invalid PIN_STATE(%d) in eirq_ctrl for PIN(%d)\n", state, id);
		return -1;
	}
	if (id != PIN_BGF_EINT) {
		WMT_PLAT_WARN_FUNC("unsupported EIRQ(PIN_ID:%d) in eirq_ctrl\n", id);
		return -1;
	}

	irq = consys_hw_bgf_irq();
	if (irq < 0)
		return irq;

	switch (state) {
	case PIN_STA_INIT:
		if (bgf_irq_requested)
			break;
		/* Trigger type comes from the device tree. */
		ret = request_irq(irq, wmt_plat_bgf_irq_isr, 0, "BTIF_WAKEUP_IRQ", NULL);
		if (ret) {
			WMT_PLAT_ERR_FUNC("request_irq(%d) failed: %d\n", irq, ret);
			return ret;
		}
		bgf_irq_requested = true;
		gbgfIrqBle.counter = 1;
		break;
	case PIN_STA_EINT_EN:
		if (!bgf_irq_requested)
			break;
		spin_lock_irqsave(&gbgfIrqBle.lock, flags);
		if (!gbgfIrqBle.counter) {
			enable_irq(irq);
			gbgfIrqBle.counter++;
		}
		spin_unlock_irqrestore(&gbgfIrqBle.lock, flags);
		break;
	case PIN_STA_EINT_DIS:
		if (!bgf_irq_requested)
			break;
		spin_lock_irqsave(&gbgfIrqBle.lock, flags);
		if (gbgfIrqBle.counter) {
			disable_irq_nosync(irq);
			gbgfIrqBle.counter--;
		}
		spin_unlock_irqrestore(&gbgfIrqBle.lock, flags);
		break;
	default: /* PIN_STA_DEINIT */
		if (bgf_irq_requested) {
			free_irq(irq, NULL);
			bgf_irq_requested = false;
			gbgfIrqBle.counter = 0;
		}
		break;
	}
	return 0;
}
EXPORT_SYMBOL(wmt_plat_eirq_ctrl);

/* The WiFi/BT paths have no host pins to configure; every pin group is a no-op. */
INT32 wmt_plat_gpio_ctrl(ENUM_PIN_ID id, ENUM_PIN_STATE state)
{
	if (id >= PIN_ID_MAX || state >= PIN_STA_MAX)
		return -1;
	return 0;
}
EXPORT_SYMBOL(wmt_plat_gpio_ctrl);

/* The WiFi function is memory-mapped; there is no SDIO slot to power. */
INT32 wmt_plat_sdio_ctrl(UINT32 sdio_slot, ENUM_FUNC_STATE on)
{
	return 0;
}
EXPORT_SYMBOL(wmt_plat_sdio_ctrl);

INT32 wmt_plat_wake_lock_ctrl(ENUM_WL_OP opId)
{
	static INT32 counter;
	INT32 ret;

	if (!wmt_ws)
		return -1;

	ret = mutex_lock_killable(&wmt_wakelock_mutex);
	if (ret)
		return ret;

	if (opId == WL_OP_GET) {
		if (++counter == 1)
			__pm_stay_awake(wmt_ws);
	} else if (opId == WL_OP_PUT) {
		if (counter > 0 && --counter == 0)
			__pm_relax(wmt_ws);
	}
	mutex_unlock(&wmt_wakelock_mutex);
	return 0;
}
EXPORT_SYMBOL(wmt_plat_wake_lock_ctrl);

INT32 wmt_plat_soc_paldo_ctrl(ENUM_PALDO_TYPE ePt, ENUM_PALDO_OP ePo)
{
	switch (ePt) {
	case BT_PALDO:
		return mtk_wcn_consys_hw_bt_paldo_ctrl(ePo);
	case WIFI_PALDO:
		return mtk_wcn_consys_hw_wifi_paldo_ctrl(ePo);
	case FM_PALDO:
	case GPS_PALDO:
		return mtk_wcn_consys_hw_vcn28_ctrl(ePo);
	default:
		WMT_PLAT_WARN_FUNC("invalid type(%d) in paldo_ctrl\n", ePt);
		return 0;
	}
}
EXPORT_SYMBOL(wmt_plat_soc_paldo_ctrl);

UINT8 *wmt_plat_get_emi_virt_add(UINT32 offset)
{
	return mtk_wcn_consys_emi_virt_addr_get(offset);
}
EXPORT_SYMBOL(wmt_plat_get_emi_virt_add);

P_CONSYS_EMI_ADDR_INFO wmt_plat_get_emi_phy_add(VOID)
{
	return &mtk_wcn_emi_addr_info;
}
EXPORT_SYMBOL(wmt_plat_get_emi_phy_add);

#if CONSYS_ENALBE_SET_JTAG
UINT32 wmt_plat_jtag_flag_ctrl(UINT32 en)
{
	return mtk_wcn_consys_jtag_flag_ctrl(en);
}
EXPORT_SYMBOL(wmt_plat_jtag_flag_ctrl);
#endif

UINT32 wmt_plat_read_cpupcr(VOID)
{
	return consys_hw_read_cpupcr();
}
EXPORT_SYMBOL(wmt_plat_read_cpupcr);

UINT32 wmt_plat_read_dmaregs(UINT32 type)
{
	return 0;
}
EXPORT_SYMBOL(wmt_plat_read_dmaregs);

INT32 wmt_plat_set_host_dump_state(ENUM_HOST_DUMP_STATE state)
{
	UINT8 *p = wmt_plat_get_emi_virt_add(EXP_APMEM_CTRL_HOST_SYNC_STATE);

	if (!p)
		return -1;
	CONSYS_REG_WRITE(p, state);
	return 0;
}
EXPORT_SYMBOL(wmt_plat_set_host_dump_state);

UINT32 wmt_plat_force_trigger_assert(ENUM_FORCE_TRG_ASSERT_T type)
{
	UINT8 *p;

	switch (type) {
	case STP_FORCE_TRG_ASSERT_EMI:
		p = wmt_plat_get_emi_virt_add(EXP_APMEM_CTRL_HOST_OUTBAND_ASSERT_W1);
		if (!p)
			return -1;
		CONSYS_REG_WRITE(p, EXP_APMEM_HOST_OUTBAND_ASSERT_MAGIC_W1);
		break;
	case STP_FORCE_TRG_ASSERT_DEBUG_PIN:
		return consys_hw_pulse_ap2conn_wakeup() ? -1 : 0;
	default:
		WMT_PLAT_ERR_FUNC("unknown force trigger assert type\n");
		break;
	}
	return 0;
}
EXPORT_SYMBOL(wmt_plat_force_trigger_assert);

INT32 wmt_plat_update_host_sync_num(VOID)
{
	UINT8 *p = wmt_plat_get_emi_virt_add(EXP_APMEM_CTRL_HOST_SYNC_NUM);

	if (!p)
		return -1;
	CONSYS_REG_WRITE(p, CONSYS_REG_READ(p) + 1);
	return 0;
}
EXPORT_SYMBOL(wmt_plat_update_host_sync_num);

INT32 wmt_plat_get_dump_info(UINT32 offset)
{
	UINT8 *p = wmt_plat_get_emi_virt_add(offset);

	if (!p)
		return -1;
	return CONSYS_REG_READ(p);
}
EXPORT_SYMBOL(wmt_plat_get_dump_info);

UINT32 wmt_plat_get_soc_chipid(VOID)
{
	return mtk_wcn_consys_soc_chipid();
}
EXPORT_SYMBOL(wmt_plat_get_soc_chipid);

#if CFG_WMT_LTE_COEX_HANDLING
INT32 wmt_plat_get_tdm_antsel_index(VOID)
{
	return 0;
}
EXPORT_SYMBOL(wmt_plat_get_tdm_antsel_index);
#endif
