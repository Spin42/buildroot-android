/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Combo-chip stub API between the WMT core (callback provider) and the
 * platform consumers (audio, UART/BTIF deep-idle control). Implemented by
 * the mt8127_consys module (consys/cmb_stub.c).
 */
#ifndef _MTK_WCN_CMB_STUB_H_
#define _MTK_WCN_CMB_STUB_H_

#define MTK_WCN_CMB_AUD_IO_NAMING_STYLE_1 1

typedef enum {
	CMB_STUB_AIF_0 = 0, /* BT_PCM_OFF & FM analog (line in/out) */
	CMB_STUB_AIF_1 = 1, /* BT_PCM_ON & FM analog (in/out) */
	CMB_STUB_AIF_2 = 2, /* BT_PCM_OFF & FM digital (I2S) */
	CMB_STUB_AIF_3 = 3, /* BT_PCM_ON & FM digital (I2S) */
	CMB_STUB_AIF_MAX = 4,
} CMB_STUB_AIF_X;

typedef enum {
	CMB_STUB_AIF_CTRL_DIS = 0,
	CMB_STUB_AIF_CTRL_EN = 1,
	CMB_STUB_AIF_CTRL_MAX = 2,
} CMB_STUB_AIF_CTRL;

typedef enum {
	COMBO_FUNC_TYPE_BT = 0,
	COMBO_FUNC_TYPE_FM = 1,
	COMBO_FUNC_TYPE_GPS = 2,
	COMBO_FUNC_TYPE_WIFI = 3,
	COMBO_FUNC_TYPE_WMT = 4,
	COMBO_FUNC_TYPE_STP = 5,
	COMBO_FUNC_TYPE_NUM = 6
} COMBO_FUNC_TYPE;

typedef enum {
	COMBO_IF_UART = 0,
	COMBO_IF_MSDC = 1,
	COMBO_IF_BTIF = 2,
	COMBO_IF_MAX,
} COMBO_IF;

typedef void (*wmt_bgf_eirq_cb)(void);
typedef int (*wmt_aif_ctrl_cb)(CMB_STUB_AIF_X, CMB_STUB_AIF_CTRL);
typedef void (*wmt_func_ctrl_cb)(unsigned int, unsigned int);
typedef signed long (*wmt_thermal_query_cb)(void);
typedef int (*wmt_deep_idle_ctrl_cb)(unsigned int);
typedef int (*wmt_func_do_reset)(unsigned int);

typedef struct _CMB_STUB_CB_ {
	unsigned int size; /* sizeof(CMB_STUB_CB) */
	wmt_aif_ctrl_cb aif_ctrl_cb;
	wmt_func_ctrl_cb func_ctrl_cb;
	wmt_thermal_query_cb thermal_query_cb;
	wmt_deep_idle_ctrl_cb deep_idle_ctrl_cb;
	wmt_func_do_reset wmt_do_reset_cb;
} CMB_STUB_CB, *P_CMB_STUB_CB;

extern int mtk_wcn_cmb_stub_reg(P_CMB_STUB_CB p_stub_cb);
extern int mtk_wcn_cmb_stub_unreg(void);

extern int mtk_wcn_cmb_stub_aif_ctrl(CMB_STUB_AIF_X state, CMB_STUB_AIF_CTRL ctrl);

static inline int mtk_wcn_cmb_stub_audio_ctrl(CMB_STUB_AIF_X state)
{
	return mtk_wcn_cmb_stub_aif_ctrl(state, 1);
}

extern int mt_combo_plt_enter_deep_idle(COMBO_IF src);
extern int mt_combo_plt_exit_deep_idle(COMBO_IF src);

extern void mtk_wcn_cmb_stub_func_ctrl(unsigned int type, unsigned int on);
extern signed long mtk_wcn_cmb_stub_query_ctrl(void);
extern int mtk_wcn_cmb_stub_do_reset(unsigned int type);

extern int mtk_wcn_wmt_chipid_query(void);
extern void mtk_wcn_wmt_set_chipid(int chipid);

#endif /* _MTK_WCN_CMB_STUB_H_ */
