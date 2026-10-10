/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CONSYS hardware control API exported by the mt8127_consys module.
 */
#ifndef _MTK_WCN_CONSYS_HW_H_
#define _MTK_WCN_CONSYS_HW_H_

#include "wmt_plat.h"

#define PLATFORM_SOC_CHIP 0x8127

/* Layout of the 1 MiB reserved region as seen by the CONSYS MCU (0xf0000000 window). */
#define KBYTE				(1024 * sizeof(char))
#define CONSYS_EMI_AP_PHY_OFFSET	(0x80000)
#define CONSYS_EMI_FW_PHY_BASE		(0xf0080000)
#define CONSYS_EMI_MEM_SIZE		(343 * KBYTE)
#define CONSYS_EMI_PAGED_TRACE_OFFSET	(0x400)
#define CONSYS_EMI_PAGED_DUMP_OFFSET	(0x8400)
#define CONSYS_EMI_FULL_DUMP_OFFSET	(0x10400)

#define CONSYS_PAGED_DUMP_START_ADDR	(0xf0088400)
#define CONSYS_PAGED_DUMP_SIZE		(32 * KBYTE)

#define CONSYS_FULL_DUMP_START_ADDR	(0xf0090400)
#define CONSYS_FULL_DUMP_DLM_LEN	(0x1f000)
#define CONSYS_FULL_DUMP_SYSB2_START	(CONSYS_FULL_DUMP_START_ADDR + CONSYS_FULL_DUMP_DLM_LEN)
#define CONSYS_FULL_DUMP_SYSB2_LEN	(0x6800)
#define CONSYS_FULL_DUMP_SYSB3_START	(CONSYS_FULL_DUMP_SYSB2_START + CONSYS_FULL_DUMP_SYSB2_LEN)
#define CONSYS_FULL_DUMP_SYSB3_LEN	(0x16800)

/* Force-assert pattern written to EXP_APMEM_CTRL_HOST_OUTBAND_ASSERT_W1. */
#define EXP_APMEM_HOST_OUTBAND_ASSERT_MAGIC_W1	(0x19b30bb1)

typedef enum _ENUM_EMI_CTRL_STATE_OFFSET_ {
	EXP_APMEM_CTRL_STATE = 0x0,
	EXP_APMEM_CTRL_HOST_SYNC_STATE = 0x4,
	EXP_APMEM_CTRL_HOST_SYNC_NUM = 0x8,
	EXP_APMEM_CTRL_CHIP_SYNC_STATE = 0xc,
	EXP_APMEM_CTRL_CHIP_SYNC_NUM = 0x10,
	EXP_APMEM_CTRL_CHIP_SYNC_ADDR = 0x14,
	EXP_APMEM_CTRL_CHIP_SYNC_LEN = 0x18,
	EXP_APMEM_CTRL_CHIP_PRINT_BUFF_START = 0x1c,
	EXP_APMEM_CTRL_CHIP_PRINT_BUFF_LEN = 0x20,
	EXP_APMEM_CTRL_CHIP_PRINT_BUFF_IDX = 0x24,
	EXP_APMEM_CTRL_CHIP_INT_STATUS = 0x28,
	EXP_APMEM_CTRL_CHIP_PAGED_DUMP_END = 0x2c,
	EXP_APMEM_CTRL_HOST_OUTBAND_ASSERT_W1 = 0x30,
	EXP_APMEM_CTRL_MAX
} ENUM_EMI_CTRL_STATE_OFFSET, *P_ENUM_EMI_CTRL_STATE_OFFSET;

extern INT32 mtk_wcn_consys_hw_init(VOID);
extern INT32 mtk_wcn_consys_hw_deinit(VOID);
extern INT32 mtk_wcn_consys_hw_pwr_off(VOID);
extern INT32 mtk_wcn_consys_hw_pwr_on(UINT32 co_clock_en);
extern INT32 mtk_wcn_consys_hw_rst(UINT32 co_clock_en);
extern INT32 mtk_wcn_consys_hw_reg_ctrl(UINT32 on, UINT32 co_clock_en);
extern INT32 mtk_wcn_consys_hw_bt_paldo_ctrl(UINT32 enable);
extern INT32 mtk_wcn_consys_hw_wifi_paldo_ctrl(UINT32 enable);
extern INT32 mtk_wcn_consys_hw_vcn28_ctrl(UINT32 enable);
extern INT32 mtk_wcn_consys_hw_state_show(VOID);
extern UINT8 *mtk_wcn_consys_emi_virt_addr_get(UINT32 ctrl_state_offset);
#if CONSYS_ENALBE_SET_JTAG
extern UINT32 mtk_wcn_consys_jtag_flag_ctrl(UINT32 en);
#endif
extern UINT32 mtk_wcn_consys_soc_chipid(VOID);

/* No SDIO slot exists on this SoC; the call is accepted and does nothing. */
extern INT32 wmt_plat_sdio_ctrl(UINT32 sdio_slot, ENUM_FUNC_STATE on);

#endif /* _MTK_WCN_CONSYS_HW_H_ */
