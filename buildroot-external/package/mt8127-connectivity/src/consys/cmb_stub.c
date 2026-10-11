// SPDX-License-Identifier: GPL-2.0
/*
 * cmb_stub: callback registry between the WMT core and platform consumers.
 */
#include <linux/module.h>
#include <linux/spinlock.h>
#include <mach/mtk_wcn_cmb_stub.h>

static DEFINE_SPINLOCK(cmb_stub_lock);
static CMB_STUB_CB cmb_stub_cb;
static int cmb_stub_chipid = 0x8127;

int mtk_wcn_cmb_stub_reg(P_CMB_STUB_CB p_stub_cb)
{
	unsigned long flags;

	if (!p_stub_cb || p_stub_cb->size != sizeof(CMB_STUB_CB)) {
		pr_err("[cmb_stub] invalid callback table %p\n", p_stub_cb);
		return -1;
	}

	spin_lock_irqsave(&cmb_stub_lock, flags);
	cmb_stub_cb = *p_stub_cb;
	spin_unlock_irqrestore(&cmb_stub_lock, flags);
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_cmb_stub_reg);

int mtk_wcn_cmb_stub_unreg(void)
{
	unsigned long flags;

	spin_lock_irqsave(&cmb_stub_lock, flags);
	memset(&cmb_stub_cb, 0, sizeof(cmb_stub_cb));
	spin_unlock_irqrestore(&cmb_stub_lock, flags);
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_cmb_stub_unreg);

int mtk_wcn_cmb_stub_aif_ctrl(CMB_STUB_AIF_X state, CMB_STUB_AIF_CTRL ctrl)
{
	wmt_aif_ctrl_cb cb;

	if (state >= CMB_STUB_AIF_MAX || ctrl >= CMB_STUB_AIF_CTRL_MAX)
		return -1;

	cb = READ_ONCE(cmb_stub_cb.aif_ctrl_cb);
	return cb ? cb(state, ctrl) : -2;
}
EXPORT_SYMBOL(mtk_wcn_cmb_stub_aif_ctrl);

void mtk_wcn_cmb_stub_func_ctrl(unsigned int type, unsigned int on)
{
	wmt_func_ctrl_cb cb = READ_ONCE(cmb_stub_cb.func_ctrl_cb);

	if (cb)
		cb(type, on);
	else
		pr_warn("[cmb_stub] func_ctrl callback not registered\n");
}
EXPORT_SYMBOL(mtk_wcn_cmb_stub_func_ctrl);

signed long mtk_wcn_cmb_stub_query_ctrl(void)
{
	wmt_thermal_query_cb cb = READ_ONCE(cmb_stub_cb.thermal_query_cb);

	return cb ? cb() : 0;
}
EXPORT_SYMBOL(mtk_wcn_cmb_stub_query_ctrl);

int mtk_wcn_cmb_stub_do_reset(unsigned int type)
{
	wmt_func_do_reset cb = READ_ONCE(cmb_stub_cb.wmt_do_reset_cb);

	return cb ? cb(type) : -1;
}
EXPORT_SYMBOL(mtk_wcn_cmb_stub_do_reset);

/* The SoC has no external bus whose deep-idle state needs arbitration. */
int mt_combo_plt_enter_deep_idle(COMBO_IF src)
{
	return src < COMBO_IF_MAX ? 0 : -1;
}
EXPORT_SYMBOL(mt_combo_plt_enter_deep_idle);

int mt_combo_plt_exit_deep_idle(COMBO_IF src)
{
	return src < COMBO_IF_MAX ? 0 : -1;
}
EXPORT_SYMBOL(mt_combo_plt_exit_deep_idle);

int mtk_wcn_wmt_chipid_query(void)
{
	return READ_ONCE(cmb_stub_chipid);
}
EXPORT_SYMBOL(mtk_wcn_wmt_chipid_query);

void mtk_wcn_wmt_set_chipid(int chipid)
{
	WRITE_ONCE(cmb_stub_chipid, chipid);
}
EXPORT_SYMBOL(mtk_wcn_wmt_set_chipid);
