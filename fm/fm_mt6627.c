// SPDX-License-Identifier: GPL-2.0
/*
 * Command sequences of the MT6627-class FM core of the MT8127 CONSYS.
 *
 * The sequences, register values and tables come from the vendor 3.10
 * driver (drivers/misc/mediatek/fmradio/mt6627/pub/mt6627_fm_{cmd,lib}.c).
 * Three register spaces are reachable through the firmware:
 *  - FM registers (8-bit address, 16-bit value): FSPI read/write and the
 *    "basic operation" scripts below,
 *  - "top" registers of the A-die (16-bit address, 32-bit value): CSPI,
 *  - CONSYS host registers (32-bit address and value).
 * Basic operation scripts let the firmware run a register sequence
 * (writes, read-modify-writes, delays, polls) in one command.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "osal_typedef.h"
#include "wmt_exp.h"

#include "mtk_fm.h"

/* FM registers */
#define FM_MAIN_CG2_CTRL	0x61
#define FM_MAIN_HWVER		0x62
#define FM_MAIN_CTRL		0x63
#define FM_CHANNEL_SET		0x65
#define FM_MAIN_INTR		0x69
#define FM_MAIN_INTRMASK	0x6a
#define FM_MAIN_EXTINTRMASK	0x6b
#define FM_RSSI_IND		0x6c
#define FM_RDS_CFG0		0x80
#define FM_DSP_PATCH_CTRL	0x90
#define FM_DSP_PATCH_DATA	0x92
#define FM_I2S_TX_CFG		0x9b
#define FM_AUDIO_MUTE		0x9c
#define FM_VOLUME		0x7d

/* FM_MAIN_CTRL */
#define FM_CTRL_TUNE		BIT(0)
#define FM_CTRL_RDS		BIT(4)
#define FM_CTRL_RAMP_DOWN	BIT(8)
/* FM_MAIN_INTR */
#define FM_INTR_STC_DONE	BIT(0)
#define FM_INTR_RDS		BIT(5)
/* FM_MAIN_CG2_CTRL */
#define FM_CG2_DEEMPHASIS_75US	BIT(12)
/* FM_RSSI_IND */
#define FM_RSSI_STEREO		BIT(12)
#define FM_RSSI_MASK		GENMASK(9, 0)

/* I2S output of the FM core and its receiver in CONSYS */
#define FM_I2S_TX_ON		0xf9ab
#define CONSYS_FM_I2S_RX	0x80101054
#define CONSYS_FM_I2S_RX_ON	0x00003f35
#define CONSYS_FM_I2S_RX_PATH	GENMASK(14, 13)
/* CONSYS clock control of the A-die */
#define CONSYS_TOP_CK_CTRL	0x80101030
#define CONSYS_TOP_CK_EN_ADIE	BIT(1)
#define CONSYS_CSPI_CFG		0x8013000c
#define CONSYS_CSPI_CFG_VAL	0x0000801f

/* Basic operations */
#define FM_BOP_WRITE		0x80
#define FM_BOP_UDELAY		0x81
#define FM_BOP_RD_UNTIL		0x82
#define FM_BOP_MODIFY		0x83
#define FM_BOP_TOP_WRITE	0x85
#define FM_BOP_TOP_RD_UNTIL	0x86
/* CSPI target of the top register operations */
#define FM_CSPI_TOP		0x04

/* Timeouts of the vendor driver, in ms */
#define FM_TMO_CMD		3000
#define FM_TMO_SM_TUNE		6000

#define FM_PATCH_SEG_LEN	512
#define FM_PATCH_MAX		(4096 * 6)
#define FM_ROM_VERSIONS		5

/* Thresholds of the stock mt6627_fm_cust.cfg and mt6627_fm_cust_cfg.h */
#define FM_TH_RSSI		(-296)
#define FM_TH_DESENSE_RSSI	(-240)
#define FM_TH_PAMD		(-12)
#define FM_TH_MR		(-67)
#define FM_TH_ATDC		3496
#define FM_TH_PRX		64
#define FM_TH_SMG		16421

static const u16 mt6627_volume[FM_VOLUME_MAX + 1] = {
	0x0000, 0x0519, 0x066a, 0x0814, 0x0a2b, 0x0ccd, 0x101d, 0x1449,
	0x198a, 0x2027, 0x287a, 0x32f5, 0x4027, 0x50c3, 0x65ad, 0x7fff,
};

/* HL side / FA / ATJ parameter per 50 kHz channel from 65.00 MHz */
static const u8 mt6627_chan_para_map[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	8, 0, 2, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0,
	0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0,
	0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	8, 0, 2, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0,
	0,
};

/*
 * Channels that use the TDD solution of register 0x30: one bit per 50 kHz
 * channel from 65.00 MHz, four channels per entry (masks 0x0001, 0x0010,
 * 0x0100, 0x1000).
 */
static const u16 mt6627_tdd[] = {
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0101, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0101, 0x0101, 0x0101,
	0x0101, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0100,
	0x0101, 0x0101, 0x0101, 0x0101, 0x0101,
	0x0101, 0x0101, 0x0001, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0100,
	0x0101, 0x0101, 0x0101, 0x0101, 0x0101,
	0x0001,
};

/* Channels where spurs of the SoC desensitize the receiver */
static const u16 mt6627_desense_chan[] = {
	6910, 7680, 7800, 9210, 9220, 9230, 9600, 9980, 9990, 10400, 10750, 10760,
};

/* --- packet building --- */

struct fm_pkt {
	u8 *buf;
	size_t len;
	bool overflow;
};

static void fm_pkt_init(struct mtk_fm *fm, struct fm_pkt *pkt, u8 op)
{
	pkt->buf = fm->tx;
	pkt->buf[0] = FM_PKT_CMD;
	pkt->buf[1] = op;
	pkt->len = FM_PKT_HDR_LEN;
	pkt->overflow = false;
}

static u8 *fm_pkt_put(struct fm_pkt *pkt, size_t n)
{
	u8 *p;

	if (pkt->overflow || pkt->len + n > FM_TX_BUF_SIZE) {
		pkt->overflow = true;
		return NULL;
	}
	p = &pkt->buf[pkt->len];
	pkt->len += n;
	return p;
}

static void fm_pkt_u8(struct fm_pkt *pkt, u8 v)
{
	u8 *p = fm_pkt_put(pkt, 1);

	if (p)
		*p = v;
}

static void fm_pkt_le16(struct fm_pkt *pkt, u16 v)
{
	u8 *p = fm_pkt_put(pkt, 2);

	if (p)
		put_unaligned_le16(v, p);
}

static void fm_pkt_le32(struct fm_pkt *pkt, u32 v)
{
	u8 *p = fm_pkt_put(pkt, 4);

	if (p)
		put_unaligned_le32(v, p);
}

/* Returns the packet length, or 0 if it did not fit. */
static size_t fm_pkt_finish(struct fm_pkt *pkt)
{
	if (pkt->overflow)
		return 0;
	put_unaligned_le16(pkt->len - FM_PKT_HDR_LEN, &pkt->buf[2]);
	return pkt->len;
}

static void bop_write(struct fm_pkt *pkt, u8 addr, u16 val)
{
	fm_pkt_u8(pkt, FM_BOP_WRITE);
	fm_pkt_u8(pkt, 3);
	fm_pkt_u8(pkt, addr);
	fm_pkt_le16(pkt, val);
}

static void bop_udelay(struct fm_pkt *pkt, u32 us)
{
	fm_pkt_u8(pkt, FM_BOP_UDELAY);
	fm_pkt_u8(pkt, 4);
	fm_pkt_le32(pkt, us);
}

static void bop_rd_until(struct fm_pkt *pkt, u8 addr, u16 mask, u16 val)
{
	fm_pkt_u8(pkt, FM_BOP_RD_UNTIL);
	fm_pkt_u8(pkt, 5);
	fm_pkt_u8(pkt, addr);
	fm_pkt_le16(pkt, mask);
	fm_pkt_le16(pkt, val);
}

/* reg = (reg & and_mask) | or_mask */
static void bop_modify(struct fm_pkt *pkt, u8 addr, u16 and_mask, u16 or_mask)
{
	fm_pkt_u8(pkt, FM_BOP_MODIFY);
	fm_pkt_u8(pkt, 5);
	fm_pkt_u8(pkt, addr);
	fm_pkt_le16(pkt, and_mask);
	fm_pkt_le16(pkt, or_mask);
}

static void bop_top_write(struct fm_pkt *pkt, u16 addr, u32 val)
{
	fm_pkt_u8(pkt, FM_BOP_TOP_WRITE);
	fm_pkt_u8(pkt, 7);
	fm_pkt_u8(pkt, FM_CSPI_TOP);
	fm_pkt_le16(pkt, addr);
	fm_pkt_le32(pkt, val);
}

static void bop_top_rd_until(struct fm_pkt *pkt, u16 addr, u32 mask, u32 val)
{
	fm_pkt_u8(pkt, FM_BOP_TOP_RD_UNTIL);
	fm_pkt_u8(pkt, 11);
	fm_pkt_u8(pkt, FM_CSPI_TOP);
	fm_pkt_le16(pkt, addr);
	fm_pkt_le32(pkt, mask);
	fm_pkt_le32(pkt, val);
}

static int fm_pkt_send(struct mtk_fm *fm, struct fm_pkt *pkt, u32 mask,
		       unsigned int timeout_ms)
{
	size_t len = fm_pkt_finish(pkt);

	if (!len)
		return -EOVERFLOW;
	return mtk_fm_cmd(fm, len, mask, timeout_ms);
}

/* --- register access --- */

int mt6627_read(struct mtk_fm *fm, u8 addr, u16 *val)
{
	struct fm_pkt pkt;
	int ret;

	fm_pkt_init(fm, &pkt, FM_OP_FSPI_READ);
	fm_pkt_u8(&pkt, addr);
	ret = fm_pkt_send(fm, &pkt, FM_FLAG(FM_OP_FSPI_READ), FM_TMO_CMD);
	if (!ret)
		*val = fm->fspi_rd;
	return ret;
}

int mt6627_write(struct mtk_fm *fm, u8 addr, u16 val)
{
	struct fm_pkt pkt;

	fm_pkt_init(fm, &pkt, FM_OP_FSPI_WRITE);
	fm_pkt_u8(&pkt, addr);
	fm_pkt_le16(&pkt, val);
	return fm_pkt_send(fm, &pkt, FM_FLAG(FM_OP_FSPI_WRITE), FM_TMO_CMD);
}

/* reg = (reg & mask) | bits, run by the firmware */
int mt6627_set_bits(struct mtk_fm *fm, u8 addr, u16 bits, u16 mask)
{
	struct fm_pkt pkt;

	fm_pkt_init(fm, &pkt, FM_OP_MODIFY);
	bop_modify(&pkt, addr, mask, bits);
	return fm_pkt_send(fm, &pkt, FM_FLAG(FM_OP_MODIFY), FM_TMO_CMD);
}

int mt6627_top_read(struct mtk_fm *fm, u16 addr, u32 *val)
{
	struct fm_pkt pkt;
	int ret;

	fm_pkt_init(fm, &pkt, FM_OP_CSPI_READ);
	fm_pkt_u8(&pkt, FM_CSPI_TOP);
	fm_pkt_le16(&pkt, addr);
	ret = fm_pkt_send(fm, &pkt, FM_FLAG_CSPI_READ, FM_TMO_CMD);
	if (!ret)
		*val = fm->cspi_rd;
	return ret;
}

int mt6627_top_write(struct mtk_fm *fm, u16 addr, u32 val)
{
	struct fm_pkt pkt;

	fm_pkt_init(fm, &pkt, FM_OP_CSPI_WRITE);
	fm_pkt_u8(&pkt, FM_CSPI_TOP);
	fm_pkt_le16(&pkt, addr);
	fm_pkt_le32(&pkt, val);
	return fm_pkt_send(fm, &pkt, FM_FLAG_CSPI_WRITE, FM_TMO_CMD);
}

int mt6627_host_read(struct mtk_fm *fm, u32 addr, u32 *val)
{
	struct fm_pkt pkt;
	int ret;

	fm_pkt_init(fm, &pkt, FM_OP_HOST_READ);
	fm_pkt_le32(&pkt, addr);
	ret = fm_pkt_send(fm, &pkt, FM_FLAG(FM_OP_HOST_READ), FM_TMO_CMD);
	if (!ret)
		*val = fm->cspi_rd;
	return ret;
}

int mt6627_host_write(struct mtk_fm *fm, u32 addr, u32 val)
{
	struct fm_pkt pkt;

	fm_pkt_init(fm, &pkt, FM_OP_HOST_WRITE);
	fm_pkt_le32(&pkt, addr);
	fm_pkt_le32(&pkt, val);
	return fm_pkt_send(fm, &pkt, FM_FLAG(FM_OP_HOST_WRITE), FM_TMO_CMD);
}

/* --- power sequences --- */

static int mt6627_clock_on(struct mtk_fm *fm)
{
	struct fm_pkt pkt;

	fm_pkt_init(fm, &pkt, FM_OP_ENABLE);
	/* band-gap LDO and top clocks */
	bop_top_write(&pkt, 0x00c0, 0x38200000);
	bop_top_write(&pkt, 0x0a10, 0xffffffff);
	/* MTCMOS on */
	bop_top_write(&pkt, 0x0060, 0x00000030);
	bop_top_write(&pkt, 0x0060, 0x00000035);
	bop_top_rd_until(&pkt, 0x0060, 0x0000000a, 0x0000000a);
	bop_top_write(&pkt, 0x0060, 0x00000015);
	bop_top_write(&pkt, 0x0060, 0x00000005);
	bop_udelay(&pkt, 10);
	bop_top_write(&pkt, 0x0060, 0x00000045);
	/* CSPI FM slave dummy count */
	bop_top_write(&pkt, 0x0068, 0x0000003f);
	/* digital oscillator, then its clock output to FM, then HW gating */
	bop_top_write(&pkt, 0x0050, 0x00000001);
	bop_udelay(&pkt, 3000);
	bop_top_write(&pkt, 0x0050, 0x00000003);
	bop_top_write(&pkt, 0x0050, 0x00000007);
	/* I2S current driving */
	bop_top_write(&pkt, 0x0000, 0x00000000);
	/* DSP auto clock gating */
	bop_write(&pkt, 0x70, 0x0040);
	bop_modify(&pkt, FM_MAIN_CG2_CTRL, (u16)~FM_CG2_DEEMPHASIS_75US,
		   fm->deemph_75us ? FM_CG2_DEEMPHASIS_75US : 0);

	return fm_pkt_send(fm, &pkt, FM_FLAG(FM_OP_ENABLE), FM_TMO_CMD);
}

static int mt6627_digital_init(struct mtk_fm *fm)
{
	struct fm_pkt pkt;

	fm_pkt_init(fm, &pkt, FM_OP_ENABLE);
	/* ADPLL divider, SDM coefficients, 26 MHz clock */
	bop_write(&pkt, 0x21, 0xe000);
	bop_write(&pkt, 0xd8, 0x03f0);
	bop_write(&pkt, 0xd9, 0x3f04);
	bop_write(&pkt, 0xda, 0x0014);
	bop_write(&pkt, 0xdb, 0x2a38);
	bop_write(&pkt, 0x23, 0x4000);
	/* fm_rgf_maincon */
	bop_write(&pkt, FM_MAIN_INTRMASK, 0x0021);
	bop_write(&pkt, FM_MAIN_EXTINTRMASK, 0x0021);
	bop_top_write(&pkt, 0x0050, 0x0000000f);
	bop_modify(&pkt, FM_MAIN_CG2_CTRL, 0xfffd, 0x0002);
	bop_modify(&pkt, FM_MAIN_CG2_CTRL, 0xfffe, 0x0000);
	bop_udelay(&pkt, 100000);
	bop_rd_until(&pkt, 0x64, 0x001f, 0x0002);

	return fm_pkt_send(fm, &pkt, FM_FLAG(FM_OP_ENABLE), FM_TMO_CMD);
}

static int mt6627_fine_tune(struct mtk_fm *fm)
{
	struct fm_pkt pkt;

	fm_pkt_init(fm, &pkt, FM_OP_ENABLE);
	/* host controls the RF registers */
	bop_top_write(&pkt, 0x0050, 0x00000007);
	bop_write(&pkt, 0x01, 0xbee8);
	bop_write(&pkt, 0x03, 0xf6ed);
	bop_write(&pkt, 0x15, 0x0d80);
	bop_write(&pkt, 0x16, 0x0068);
	bop_write(&pkt, 0x17, 0x092a);
	bop_write(&pkt, 0x34, 0x807f);
	bop_write(&pkt, 0x35, 0x311e);
	/* DSP controls the RF registers */
	bop_top_write(&pkt, 0x0050, 0x0000000f);

	return fm_pkt_send(fm, &pkt, FM_FLAG(FM_OP_ENABLE), FM_TMO_CMD);
}

static int mt6627_power_off_seq(struct mtk_fm *fm)
{
	struct fm_pkt pkt;
	int i;

	fm_pkt_init(fm, &pkt, FM_OP_ENABLE);
	/* I2S TX off */
	bop_modify(&pkt, FM_I2S_TX_CFG, 0xfff8, 0x0000);
	/* HW clock control off, ASIP reset, digital core and RGF reset */
	bop_top_write(&pkt, 0x0050, 0x0000330f);
	bop_write(&pkt, FM_MAIN_CG2_CTRL, 0x0001);
	for (i = 0; i < 4; i++)
		bop_modify(&pkt, 0x6e, 0xfff8, 0x0000);
	/* all clocks off, RGFRF reset */
	bop_top_write(&pkt, 0x0050, 0x00000000);
	bop_top_write(&pkt, 0x0050, 0x00004000);
	bop_top_write(&pkt, 0x0050, 0x00000000);
	/* MTCMOS off */
	bop_top_write(&pkt, 0x0060, 0x00000005);
	bop_top_write(&pkt, 0x0060, 0x00000015);
	bop_top_write(&pkt, 0x0060, 0x00000035);
	bop_top_write(&pkt, 0x0060, 0x00000030);
	bop_top_rd_until(&pkt, 0x0060, 0x0000000a, 0x00000000);

	return fm_pkt_send(fm, &pkt, FM_FLAG(FM_OP_ENABLE), FM_TMO_CMD);
}

static int mt6627_get_rom_version(struct mtk_fm *fm)
{
	u16 val;
	int ret, i;

	/* request the DSP ROM version, release the ASIP reset and power it */
	ret = mt6627_set_bits(fm, FM_MAIN_CG2_CTRL, 0x8000, 0x7fff);
	if (!ret)
		ret = mt6627_set_bits(fm, FM_MAIN_CG2_CTRL, 0x0002, 0xfffd);
	if (!ret)
		ret = mt6627_set_bits(fm, FM_MAIN_CG2_CTRL, 0x0000, 0xfffe);
	if (ret)
		return ret;

	for (i = 0; i < 100; i++) {
		usleep_range(1000, 1500);
		ret = mt6627_read(fm, 0x84, &val);
		if (ret)
			return ret;
		if (val == 0x0001)
			break;
	}
	if (val != 0x0001) {
		dev_err(fm->dev, "DSP ROM version not ready (0x84 = 0x%04x)\n",
			val);
		return -ETIMEDOUT;
	}

	ret = mt6627_read(fm, 0x83, &val);
	if (ret)
		return ret;

	/* version request off, ASIP back in reset */
	mt6627_set_bits(fm, FM_MAIN_CG2_CTRL, 0x0000, 0x7fff);
	mt6627_set_bits(fm, FM_MAIN_CG2_CTRL, 0x0001, 0xfffc);

	return val >> 8;
}

/* Sends a patch or coefficient image in 512-byte segments. */
static int mt6627_dsp_download(struct mtk_fm *fm, u8 op, const u8 *img,
			       size_t len)
{
	unsigned int seg_num = len / FM_PATCH_SEG_LEN + 1;
	unsigned int seg;
	int ret;

	for (seg = 0; seg < seg_num; seg++) {
		size_t seg_len = (seg + 1 < seg_num) ? FM_PATCH_SEG_LEN :
						      len % FM_PATCH_SEG_LEN;
		struct fm_pkt pkt;
		u8 *p;

		fm_pkt_init(fm, &pkt, op);
		fm_pkt_u8(&pkt, seg_num);
		fm_pkt_u8(&pkt, seg);
		p = fm_pkt_put(&pkt, seg_len);
		if (p)
			memcpy(p, img + seg * FM_PATCH_SEG_LEN, seg_len);
		ret = fm_pkt_send(fm, &pkt, FM_FLAG(op), FM_TMO_CMD);
		if (ret) {
			dev_err(fm->dev, "segment %u/%u of op 0x%02x failed\n",
				seg, seg_num, op);
			return ret;
		}
	}

	return 0;
}

static int mt6627_request_fw(struct mtk_fm *fm, const char *kind, int ver,
			     const struct firmware **fw)
{
	char name[64];
	int ret;

	snprintf(name, sizeof(name), "mediatek/mt6627_fm_v%d_%s.bin", ver,
		 kind);
	ret = firmware_request_nowarn(fw, name, fm->dev);
	if (ret)
		return ret;
	if (!(*fw)->size || (*fw)->size > FM_PATCH_MAX) {
		release_firmware(*fw);
		*fw = NULL;
		return -EINVAL;
	}
	dev_dbg(fm->dev, "%s: %zu bytes\n", name, (*fw)->size);
	return 0;
}

/*
 * Downloads the DSP patch and coefficients. The files are numbered after
 * the ROM version (ROM 0 uses v1). Without a file for this ROM, the stock
 * driver uses the newest available pair and clears bytes 4-5 of the
 * coefficients, which disables the patch.
 */
static int mt6627_download_patch(struct mtk_fm *fm)
{
	const struct firmware *patch = NULL, *coeff = NULL;
	int ver = fm->rom_ver + 1;
	bool fallback = false;
	u8 *buf;
	int ret;

	if (mt6627_request_fw(fm, "patch", ver, &patch) ||
	    mt6627_request_fw(fm, "coeff", ver, &coeff)) {
		release_firmware(patch);
		release_firmware(coeff);
		patch = NULL;
		coeff = NULL;
		fallback = true;
		for (ver = FM_ROM_VERSIONS; ver > 0; ver--) {
			if (mt6627_request_fw(fm, "patch", ver, &patch))
				continue;
			if (!mt6627_request_fw(fm, "coeff", ver, &coeff))
				break;
			release_firmware(patch);
			patch = NULL;
		}
		if (!ver) {
			dev_err(fm->dev,
				"no mediatek/mt6627_fm_vN_{patch,coeff}.bin firmware\n");
			return -ENOENT;
		}
		dev_warn(fm->dev, "no firmware for ROM v%d, using v%d without patch\n",
			 fm->rom_ver + 1, ver);
	}

	ret = mt6627_dsp_download(fm, FM_OP_PATCH, patch->data, patch->size);
	if (ret)
		goto out;

	buf = kmemdup(coeff->data, coeff->size, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}
	if (coeff->size >= 40)
		fm->patch_ver = get_unaligned_le16(&buf[38]);
	if (fallback && coeff->size >= 6) {
		buf[4] = 0;
		buf[5] = 0;
	}
	ret = mt6627_dsp_download(fm, FM_OP_COEFF, buf, coeff->size);
	kfree(buf);
	if (ret)
		goto out;

	dev_info(fm->dev, "firmware v%d loaded (patch version 0x%04x)\n", ver,
		 fm->patch_ver);
out:
	release_firmware(patch);
	release_firmware(coeff);
	return ret;
}

/* Called with the FM function of WMT on. */
int mt6627_power_up(struct mtk_fm *fm)
{
	u32 host;
	u16 id;
	int ret;

	/* common SPI parameters, A-die top clock */
	ret = mt6627_host_write(fm, CONSYS_CSPI_CFG, CONSYS_CSPI_CFG_VAL);
	if (ret)
		return ret;
	ret = mt6627_host_read(fm, CONSYS_TOP_CK_CTRL, &host);
	if (ret)
		return ret;
	ret = mt6627_host_write(fm, CONSYS_TOP_CK_CTRL,
				host | CONSYS_TOP_CK_EN_ADIE);
	if (ret)
		return ret;

	ret = mt6627_clock_on(fm);
	if (ret)
		return ret;

	ret = mt6627_read(fm, FM_MAIN_HWVER, &id);
	if (ret)
		return ret;
	fm->hw_id = id;
	if (id != 0x6627 && id != 0x6625) {
		dev_err(fm->dev, "unexpected FM core id 0x%04x\n", id);
		return -ENODEV;
	}
	fm->eco_ver = mtk_wcn_wmt_hwver_get();

	ret = mt6627_get_rom_version(fm);
	if (ret < 0)
		return ret;
	fm->rom_ver = ret;
	dev_info(fm->dev, "FM core 0x%04x, ECO 0x%08x, DSP ROM v%d\n", id,
		 fm->eco_ver, fm->rom_ver);

	ret = mt6627_download_patch(fm);
	if (ret)
		return ret;

	ret = mt6627_write(fm, FM_DSP_PATCH_DATA, 0x0000);
	if (!ret)
		ret = mt6627_write(fm, FM_DSP_PATCH_CTRL, 0x0040);
	if (!ret)
		ret = mt6627_write(fm, FM_DSP_PATCH_CTRL, 0x0000);
	if (ret)
		return ret;

	ret = mt6627_digital_init(fm);
	if (ret)
		return ret;
	ret = mt6627_fine_tune(fm);
	if (ret)
		return ret;

	/* I2S output of the FM core and the 2-wire receiver of CONSYS */
	ret = mt6627_write(fm, FM_I2S_TX_CFG, FM_I2S_TX_ON);
	if (ret)
		return ret;
	return mt6627_host_write(fm, CONSYS_FM_I2S_RX, CONSYS_FM_I2S_RX_ON);
}

int mt6627_power_down(struct mtk_fm *fm)
{
	u16 intr;
	u32 host;
	int ret;

	/* a pending STC interrupt keeps the DSP from switching clocks back */
	ret = mt6627_read(fm, FM_MAIN_INTR, &intr);
	if (!ret && (intr & FM_INTR_STC_DONE))
		mt6627_write(fm, FM_MAIN_INTR, intr);

	ret = mt6627_host_read(fm, CONSYS_FM_I2S_RX, &host);
	if (!ret)
		mt6627_host_write(fm, CONSYS_FM_I2S_RX,
				  host & ~CONSYS_FM_I2S_RX_PATH);

	ret = mt6627_power_off_seq(fm);
	if (ret)
		return ret;

	mt6627_write(fm, FM_MAIN_EXTINTRMASK, 0x0000);

	ret = mt6627_host_read(fm, CONSYS_TOP_CK_CTRL, &host);
	if (ret)
		return ret;
	return mt6627_host_write(fm, CONSYS_TOP_CK_CTRL,
				 host & ~CONSYS_TOP_CK_EN_ADIE);
}

int mt6627_rampdown(struct mtk_fm *fm)
{
	struct fm_pkt pkt;
	int ret;

	ret = mt6627_top_write(fm, 0x0050, 0x00000007);
	if (!ret)
		ret = mt6627_set_bits(fm, 0x0f, 0x0000, 0xf800);
	if (!ret)
		ret = mt6627_top_write(fm, 0x0050, 0x0000000f);
	if (!ret)
		ret = mt6627_write(fm, FM_MAIN_INTRMASK, 0x0000);
	if (!ret)
		ret = mt6627_write(fm, FM_MAIN_EXTINTRMASK, 0x0000);
	if (ret)
		return ret;

	fm_pkt_init(fm, &pkt, FM_OP_RAMPDOWN);
	/* clear the DSP state, ramp down, wait for STC_DONE and clear it */
	bop_modify(&pkt, FM_MAIN_CTRL, 0xfff0, 0x0000);
	bop_modify(&pkt, FM_MAIN_CTRL, 0xffff, FM_CTRL_RAMP_DOWN);
	bop_rd_until(&pkt, FM_MAIN_INTR, FM_INTR_STC_DONE, FM_INTR_STC_DONE);
	bop_modify(&pkt, FM_MAIN_CTRL, (u16)~FM_CTRL_RAMP_DOWN, 0x0000);
	bop_modify(&pkt, FM_MAIN_INTR, 0xffff, FM_INTR_STC_DONE);
	ret = fm_pkt_send(fm, &pkt, FM_FLAG(FM_OP_RAMPDOWN), FM_TMO_CMD);
	if (ret)
		return ret;

	ret = mt6627_write(fm, FM_MAIN_EXTINTRMASK, 0x0021);
	if (!ret)
		ret = mt6627_write(fm, FM_MAIN_INTRMASK, 0x0021);
	return ret;
}

static unsigned int mt6627_chan_index(u16 freq)
{
	return freq < 6500 ? 0 : (freq - 6500) / 5;
}

static u16 mt6627_chan_para(u16 freq)
{
	unsigned int i = mt6627_chan_index(freq);

	if (freq < 6500)
		return 0;
	return mt6627_chan_para_map[min_t(unsigned int, i,
				      ARRAY_SIZE(mt6627_chan_para_map) - 1)];
}

static bool mt6627_chan_tdd(u16 freq)
{
	unsigned int i = mt6627_chan_index(freq);

	if (freq < 6500 || i / 4 >= ARRAY_SIZE(mt6627_tdd))
		return false;
	return mt6627_tdd[i / 4] & (1 << (4 * (i % 4)));
}

static u16 mt6627_freq_reg(u16 freq)
{
	return (freq - 6400) * 2 / 10;
}

/* Tunes to @freq (10 kHz units, multiple of 5). Ramp down first. */
int mt6627_tune(struct mtk_fm *fm, u16 freq)
{
	struct fm_pkt pkt;
	u16 para = mt6627_chan_para(freq);
	int ret;

	ret = mt6627_top_write(fm, 0x0050, 0x00000007);
	if (!ret)
		ret = mt6627_set_bits(fm, 0x0f, 0x0455, 0xf800);
	if (!ret)
		ret = mt6627_set_bits(fm, 0x30, mt6627_chan_tdd(freq) ?
				      0x0008 : 0x0000, 0xfff3);
	if (!ret)
		ret = mt6627_top_write(fm, 0x0050, 0x0000000f);
	if (!ret)
		ret = mt6627_set_bits(fm, FM_CHANNEL_SET,
				      mt6627_freq_reg(freq), 0xfc00);
	if (!ret)
		ret = mt6627_set_bits(fm, FM_CHANNEL_SET, para << 12, 0x0fff);
	if (ret)
		return ret;

	fm_pkt_init(fm, &pkt, FM_OP_TUNE);
	bop_modify(&pkt, FM_MAIN_CTRL, 0xfff8, FM_CTRL_TUNE);
	return fm_pkt_send(fm, &pkt, FM_FLAG(FM_OP_TUNE) | FM_FLAG_TUNE_DONE,
			   FM_TMO_CMD);
}

int mt6627_set_mute(struct mtk_fm *fm, bool mute)
{
	return mt6627_set_bits(fm, FM_AUDIO_MUTE, mute ? 0x0003 : 0x0000,
			       0xfffc);
}

int mt6627_set_volume(struct mtk_fm *fm, u8 vol)
{
	return mt6627_write(fm, FM_VOLUME,
			    mt6627_volume[min_t(u8, vol, FM_VOLUME_MAX)]);
}

int mt6627_set_mono(struct mtk_fm *fm, bool mono)
{
	int ret, ret2;

	ret = mt6627_top_write(fm, 0x0050, 0x00000007);
	if (ret)
		return ret;
	ret = mt6627_set_bits(fm, 0x75, mono ? 0x0008 : 0x0000, (u16)~0x0008);
	ret2 = mt6627_top_write(fm, 0x0050, 0x0000000f);
	return ret ? ret : ret2;
}

int mt6627_set_deemphasis(struct mtk_fm *fm, bool us75)
{
	return mt6627_set_bits(fm, FM_MAIN_CG2_CTRL,
			       us75 ? FM_CG2_DEEMPHASIS_75US : 0,
			       (u16)~FM_CG2_DEEMPHASIS_75US);
}

static int mt6627_rssi_raw(u16 reg)
{
	int raw = reg & FM_RSSI_MASK;

	return raw > 511 ? raw - 1024 : raw;
}

/* RSSI in dBm: raw (10-bit two's complement) * 6 / 16 */
int mt6627_get_signal(struct mtk_fm *fm, int *rssi_dbm, bool *stereo)
{
	u16 val;
	int ret;

	ret = mt6627_read(fm, FM_RSSI_IND, &val);
	if (ret)
		return ret;
	*rssi_dbm = mt6627_rssi_raw(val) * 6 / 16;
	*stereo = val & FM_RSSI_STEREO;
	return 0;
}

/* Audio off while searching: ramp down and stop both I2S ends. */
int mt6627_search_begin(struct mtk_fm *fm)
{
	int ret;

	ret = mt6627_rampdown(fm);
	if (!ret)
		ret = mt6627_host_write(fm, CONSYS_FM_I2S_RX, 0x00000000);
	if (!ret)
		ret = mt6627_write(fm, FM_I2S_TX_CFG, 0x0000);
	return ret;
}

int mt6627_search_end(struct mtk_fm *fm)
{
	int ret;

	ret = mt6627_rampdown(fm);
	if (!ret)
		ret = mt6627_write(fm, FM_I2S_TX_CFG, FM_I2S_TX_ON);
	if (!ret)
		ret = mt6627_host_write(fm, CONSYS_FM_I2S_RX,
					CONSYS_FM_I2S_RX_ON);
	return ret;
}

static bool mt6627_is_desense_chan(u16 freq)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mt6627_desense_chan); i++)
		if (mt6627_desense_chan[i] == freq)
			return true;
	return false;
}

static int fm_s10(u16 v)
{
	v &= 0x3ff;
	return v >= 512 ? v - 1024 : v;
}

static int fm_s9(u16 v)
{
	v &= 0x1ff;
	return v >= 256 ? v - 512 : v;
}

/*
 * Measures @freq with the soft-mute tune command and reports whether it
 * carries a station, with the checks and thresholds of the stock driver
 * (mt6627_soft_mute_tune() and mt6627_desense_check()).
 */
int mt6627_probe_channel(struct mtk_fm *fm, u16 freq, bool *valid)
{
	const struct mtk_fm_cqi *cqi;
	struct fm_pkt pkt;
	int rssi, pamd, mr, atdc;
	u32 prx, atdev, smg;
	u16 atdc_raw;
	int ret;

	ret = mt6627_set_bits(fm, FM_CHANNEL_SET,
			      mt6627_chan_para(freq) == 2 ? 0x2000 : 0x0000,
			      0x0fff);
	if (ret)
		return ret;

	fm_pkt_init(fm, &pkt, FM_OP_SOFT_MUTE_TUNE);
	fm_pkt_u8(&pkt, 1);	/* one channel */
	fm_pkt_le16(&pkt, freq);
	ret = fm_pkt_send(fm, &pkt, FM_FLAG(FM_OP_SOFT_MUTE_TUNE),
			  FM_TMO_SM_TUNE);
	if (ret)
		return ret;

	if (fm->cqi_len < 2 + sizeof(*cqi)) {
		dev_warn(fm->dev, "short channel report (%u bytes)\n",
			 fm->cqi_len);
		*valid = false;
		return 0;
	}
	cqi = (const struct mtk_fm_cqi *)&fm->cqi[2];

	rssi = fm_s10(le16_to_cpu(cqi->rssi));
	pamd = fm_s9(le16_to_cpu(cqi->pamd));
	mr = fm_s9(le16_to_cpu(cqi->mr));
	atdc_raw = le16_to_cpu(cqi->atdc);
	atdc = atdc_raw >= 32768 ? 65536 - atdc_raw : atdc_raw;
	prx = le16_to_cpu(cqi->prx) & 0xff;
	atdev = le16_to_cpu(cqi->atdev);
	smg = le16_to_cpu(cqi->smg);

	*valid = rssi >= FM_TH_RSSI && pamd <= FM_TH_PAMD &&
		 atdc <= FM_TH_ATDC && mr >= FM_TH_MR && prx >= FM_TH_PRX &&
		 atdev >= atdc && smg >= FM_TH_SMG;
	if (*valid && mt6627_is_desense_chan(freq) &&
	    rssi < FM_TH_DESENSE_RSSI)
		*valid = false;

	dev_dbg(fm->dev,
		"%u: rssi %d pamd %d mr %d atdc %d prx %u atdev %u smg %u -> %d\n",
		freq, rssi, pamd, mr, atdc, prx, atdev, smg, *valid);
	return 0;
}

int mt6627_rds_enable(struct mtk_fm *fm, bool on)
{
	u16 val;
	int ret;

	if (on) {
		/* buffer threshold: one event per 6 groups */
		ret = mt6627_write(fm, FM_RDS_CFG0, 6);
		if (ret)
			return ret;
	}
	ret = mt6627_read(fm, FM_MAIN_CTRL, &val);
	if (ret)
		return ret;
	val = on ? (val | FM_CTRL_RDS) : (val & ~FM_CTRL_RDS);
	return mt6627_write(fm, FM_MAIN_CTRL, val);
}
