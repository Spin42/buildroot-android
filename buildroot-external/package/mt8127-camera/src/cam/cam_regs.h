/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register offsets and values of the MT8127 camera pipeline.
 *
 * Sources: docs/seninf-mipi.md, docs/isp-capture.md and docs/hardware.md of this repository. Offsets are relative
 * to the window named in each group. Values whose meaning is inferred rather than read from the stock code are
 * marked "[I]" in the docs and are exposed as module parameters where they are open questions.
 */
#ifndef CAM_REGS_H
#define CAM_REGS_H

#include <linux/bits.h>

/* IMGSYS clock gates (window "imgsys", PA 0x15000000) */
#define IMG_CG_CON		0x000
#define IMG_CG_SET		0x004
#define IMG_CG_CLR		0x008
/* LARB2, CAM_SMI, CAM_CAM, SEN_TG, SEN_CAM */
#define IMG_CG_CAM_MASK		(BIT(0) | BIT(5) | BIT(6) | BIT(7) | BIT(8))

/* SPM (window "spm", PA 0x10006000): ISP power domain */
#define SPM_ISP_PWR_CON		0x238
#define SPM_PWR_STATUS		0x60c
#define SPM_PWR_STATUS_S	0x610
#define ISP_PWR_STA_MASK	BIT(5)
#define PWR_RST_B		BIT(0)
#define PWR_ISO			BIT(1)
#define PWR_ON			BIT(2)
#define PWR_ON_S		BIT(3)
#define PWR_CLK_DIS		BIT(4)
#define PWR_SRAM_PDN		GENMASK(11, 8)
#define PWR_SRAM_ACK		GENMASK(13, 12)

/* TOPCKGEN (window "topckgen", PA 0x10000000) */
#define TOP_CLK_CFG_1		0x050
#define CAMTG_SEL_MASK		GENMASK(26, 24)
#define CAMTG_SEL_UNIVPLL_D26	(1U << 24)	/* 48 MHz */
#define CAMTG_PDN		BIT(31)

/* GPIO (window "gpio", PA 0x10005000): pin mode registers, 5 pins of 3 bits each */
#define GPIO_MODE_100_104	0x740
#define GPIO_MODE_105_109	0x750
#define GPIO_MODE_110_114	0x760
#define GPIO_MODE_115_119	0x770
#define GPIO_IES_117_120	0x910
#define GPIO_IES_117_120_BIT	BIT(7)
#define GPIO_DRV_117_120	0xb60
#define GPIO_DRV_117_120_MASK	GENMASK(15, 12)

/* SENINF (window "seninf", PA 0x15008000) */
#define SENINF_TOP_CTRL		0x000
#define SENINF_TOP_TG1_CLK_EN	BIT(10)		/* [I] */
#define SENINF1_CTRL		0x010
#define SENINF1_CTRL_EN		BIT(31)
#define SENINF1_CTRL_PAD_SHIFT	28
#define SENINF1_CTRL_PAD_MASK	GENMASK(30, 28)
#define SENINF1_CTRL_SRC_MASK	GENMASK(15, 12)
#define SENINF1_CTRL_CONST_MASK	GENMASK(27, 16)
#define SENINF1_CTRL_CONST	0x06df0000	/* [27:16] = 0x6df */
#define SENINF1_CTRL_JPEG	BIT(8)
#define SENINF1_CTRL_SYNC_B	BIT(9)
#define SENINF1_CTRL_SYNC_A	BIT(10)
#define SENINF1_CTRL_RST	GENMASK(1, 0)
#define SENINF1_INTEN		0x014
#define SENINF1_INTSTA		0x018
#define SENINF1_SIZE		0x02c	/* detected size, [31:16] / [15:0] */
#define SENINF1_CSI2_CTRL	0x100
#define SENINF1_CSI2_DELAY	0x104
#define SENINF1_CSI2_REG108	0x108
#define SENINF1_CSI2_REG10C	0x10c
#define SENINF1_CSI2_STA_FIRST	0x110
#define SENINF1_CSI2_STA_LAST	0x124
#define SENINF1_CSI2_LNMUX	0x128
#define CSI2_CTRL_BASE		0x411		/* bit0 enable, bit4 ECC, bit10 CRC [I] */
#define CSI2_CTRL_ECC_ORDER_SHIFT 5
#define CSI2_CTRL_VSYNC_SHIFT	13
#define CSI2_CTRL_FLOW_SHIFT	17
#define CSI2_CTRL_RST		BIT(12)
#define CSI2_DELAY_SETTLE_SHIFT	16
#define SENINF1_TG_PH_CNT	0x300
#define SENINF1_TG_SEN_CK	0x304
#define TG_PH_CNT_PCEN		BIT(31)
#define TG_PH_CNT_ADCLK_EN	BIT(29)		/* MCLK output gate */
#define TG_PH_CNT_CLKPOL	BIT(28)
#define TG_PH_CNT_PAD_PCLK_INV	BIT(6)
#define TG_PH_CNT_CLKFL_POL	BIT(2)
#define TG_PH_CNT_TGCLK_SEL	BIT(0)
#define TG_PH_CNT_CLR_MASK	0xb0000047
#define TG_SEN_CK_24MHZ		0x00010001	/* CLKFL = 1, CLKCNT = 1: 48 MHz / 2 */
#define SENINF_NCSI2_INTSTA	0x618		/* [I] error flags, 0xfb8 mask */
#define SENINF_NCSI2_ERR_MASK	0xfb8

/* MIPI RX digital (window "mipi-rx", PA 0x1500C000) */
#define RX_LANE_MAP		0x24
#define RX_LANE_MAP_MASK	0xff000000
#define RX_LANE_MAP_BIT19	BIT(19)
#define RX_CAL_CTRL		0x38
#define RX_CAL_DATA		0x3c
#define RX_CAL_DATA_VAL		0x1541
#define RX_STA0			0x44
#define RX_STA1			0x48
#define RX_STA0_READY		0x10101
#define RX_STA1_READY		0x101
#define RX_LANEMAP_CSI0		0xe4
#define RX_LANEMAP_CSI1		0x1b

/* MIPI RX analog (window "mipi-ana", PA 0x10010800) */
#define ANA_CON00		0x00	/* clock lane */
#define ANA_CON04		0x04	/* data lanes 04, 08, 0c, 10 */
#define ANA_CON08		0x08
#define ANA_CON0C		0x0c
#define ANA_CON10		0x10
#define ANA_CON20		0x20
#define ANA_CON24		0x24
#define ANA_CON48		0x48
#define ANA_CON4C		0x4c
#define ANA_CON50		0x50
#define ANA_LANE_EN		BIT(0)
#define ANA_LANE_CSI0_SEL	BIT(3)
#define ANA_LANE_CSI1_SEL	BIT(4)
#define ANA_LANE_BIT22		BIT(22)
#define ANA48_PD_MASK		0x3c0
#define ANA4C_PD_CLR_MASK	0xfefbefbe	/* AND mask used to release power down */
#define ANA4C_PD_SET		0x01041041	/* OR value used for power down */
#define ANA50_PD		BIT(0)
#define ANA20_CSI1_SEL		GENMASK(3, 2)

/* ISP (window "isp", PA 0x15004000) */
#define ISP_CTL_EN1		0x004
#define ISP_CTL_EN2		0x008
#define ISP_CTL_DMA_EN		0x00c
#define ISP_CTL_FMT_SEL		0x010
#define ISP_CTL_SEL		0x018
#define ISP_CTL_INT_EN		0x020
#define ISP_CTL_INT_STATUS	0x024	/* read to clear */
#define ISP_CTL_DMA_INT		0x028	/* read to clear */
#define ISP_CTL_INTB_EN		0x02c
#define ISP_CTL_INTC_EN		0x038
#define ISP_CTL_SW_CTL		0x05c
#define ISP_REG_06C		0x06c
#define ISP_EN1_SET		0x080
#define ISP_EN1_CLR		0x084
#define ISP_EN2_SET		0x088
#define ISP_EN2_CLR		0x08c
#define ISP_DMA_EN_SET		0x090
#define ISP_DMA_EN_CLR		0x094
#define ISP_FMT_SEL_SET		0x098
#define ISP_FMT_SEL_CLR		0x09c
#define ISP_CTL_SEL_SET		0x0a0
#define ISP_CTL_SEL_CLR		0x0a4
#define ISP_CQ0C_BASE		0x0bc
#define ISP_MUX_SEL_SET		0x0c0
#define ISP_MUX_SEL_CLR		0x0c4
#define ISP_MUX_SEL2_SET	0x0c8
#define ISP_MUX_SEL2_CLR	0x0cc
#define ISP_MUX_CONF_SET	0x0d0
#define ISP_MUX_CONF_CLR	0x0d4
#define ISP_PIX_ID_SET		0x0d8
#define ISP_PIX_ID_CLR		0x0dc
#define ISP_IMGO_FBC		0x0f4
#define ISP_FBC_INT		0x0fc
#define ISP_REG_150		0x150
#define ISP_DBG_SET		0x160
#define ISP_REG_280		0x280
#define ISP_REG_284		0x284
#define ISP_IMGO_BASE_ADDR	0x300
#define ISP_IMGO_OFST_ADDR	0x304
#define ISP_IMGO_XSIZE		0x308
#define ISP_IMGO_YSIZE		0x30c
#define ISP_IMGO_STRIDE		0x310
#define ISP_IMGO_CON		0x314
#define ISP_IMGO_CON2		0x318
#define ISP_IMGO_CROP		0x31c
#define ISP_REG_380		0x380
#define ISP_REG_384		0x384
#define ISP_REG_39C		0x39c
#define ISP_REG_3A0		0x3a0
#define ISP_DMA_ERR_CTRL	0x3a4
#define ISP_IMGO_ERR_STAT	0x3c4
#define ISP_TG_SEN_MODE		0x410
#define ISP_TG_VF_CON		0x414
#define ISP_TG_SEN_GRAB_PXL	0x418
#define ISP_TG_SEN_GRAB_LIN	0x41c
#define ISP_TG_PATH_CFG		0x420
#define ISP_TG_INTER_ST		0x44c
#define ISP_REG_4F4		0x4f4
#define ISP_REG_1600		0x1600

#define SW_RST_TRIG		BIT(0)
#define SW_RST_STATUS		BIT(1)
#define SW_HW_RST		BIT(2)

#define TG_SEN_MODE_CMOS_EN	BIT(0)
#define TG_SEN_MODE_JPEG	BIT(1)
#define TG_SEN_MODE_SYNC	BIT(2)
#define TG_SEN_MODE_PARALLEL	BIT(8)
#define TG_SEN_MODE_MIPI_CLR	GENMASK(9, 8)
#define TG_VF_VFDATA_EN		BIT(0)
#define TG_VF_SINGLE_MODE	BIT(1)
#define TG_VF_DELAY_SHIFT	8
#define TG_VF_ALWAYS_SET	BIT(12)
#define TG_VF_CLR_MASK		0x1f02
#define TG_INTER_ST_MASK	GENMASK(13, 8)
#define TG_INTER_ST_IDLE	1

#define FMT_SEL_TG1_FMT_MASK	GENMASK(18, 16)
#define FMT_SEL_TG1_FMT_SHIFT	16
#define FMT_SEL_TG1_JPEG	BIT(24)

#define INT_VS1			BIT(0)
#define INT_TG1_ERR		BIT(4)
#define INT_PASS1_TG1_DON	BIT(10)
#define INT_SOF1		BIT(12)
#define INT_IMGO_ERR		BIT(20)
#define INT_DMA_ERR		BIT(30)
#define INT_ERR_MASK		(INT_TG1_ERR | INT_IMGO_ERR | INT_DMA_ERR)

#define FBC_CNT_MASK		GENMASK(3, 0)
#define FBC_DROP_INT_EN		BIT(4)
#define FBC_RCNT_INC		BIT(11)
#define FBC_EN			BIT(14)
#define FBC_FB_NUM_SHIFT	16
#define FBC_FB_NUM_MASK		GENMASK(19, 16)

#define EMI_BWL_REG		0x120		/* optional window "emi" */
#define EMI_BWL_TG_MASK		0x3f

/* pass-1 constants written once by ISP_TOP_CTRL::_config (docs/isp-capture.md 3.4); meaning unknown */
#define ISP_EN1_DEFAULT		0x40000000
#define ISP_EN2_KEEP_MASK	0x04000000
#define ISP_V06C		0x00000080
#define ISP_V150		0x0001ffff
#define ISP_V380		0x04000008
#define ISP_V39C		0x08000020
#define ISP_V280		0x08000010
#define ISP_DBG_CQ_STATUS	0x6000

#endif
