/* SPDX-License-Identifier: GPL-2.0 */
/*
 * FM receiver of the MT8127 CONSYS (MT6627-class FM core behind the WMT/STP
 * stack): shared definitions of the link, chip and V4L2 layers.
 */
#ifndef MTK_FM_H
#define MTK_FM_H

#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/videodev2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>

/* Packet types of the FM channel of STP */
#define FM_PKT_CMD		0x01
#define FM_PKT_EVT		0x04
#define FM_PKT_HDR_LEN		4

/* Command opcodes (vendor fm_link.h) */
#define FM_OP_FSPI_READ		0x03
#define FM_OP_FSPI_WRITE	0x04
#define FM_OP_ENABLE		0x07
#define FM_OP_TUNE		0x09
#define FM_OP_SEEK		0x0a
#define FM_OP_SCAN		0x0b
#define FM_OP_RDS_DATA		0x0d
#define FM_OP_RAMPDOWN		0x0e
#define FM_OP_MODIFY		0x11
#define FM_OP_PATCH		0x12
#define FM_OP_COEFF		0x13
#define FM_OP_SOFT_MUTE_TUNE	0x17
#define FM_OP_HOST_READ		0x18
#define FM_OP_HOST_WRITE	0x19
#define FM_OP_CSPI_WRITE	0x20
#define FM_OP_CSPI_READ		0x21

/*
 * Completion flags. An opcode below 32 sets BIT(opcode) when its event
 * arrives; the remaining bits are the vendor's special cases.
 */
#define FM_FLAG(op)		BIT(op)
#define FM_FLAG_CSPI_READ	BIT(22)
#define FM_FLAG_CSPI_WRITE	BIT(26)
#define FM_FLAG_CQI_DONE	BIT(27)
#define FM_FLAG_TUNE_DONE	BIT(28)
#define FM_FLAG_SEEK_DONE	BIT(29)
#define FM_FLAG_SCAN_DONE	BIT(30)

#define FM_TX_BUF_SIZE		1024
#define FM_RX_PKT_MAX		512
#define FM_CQI_SIZE		96

/* Frequencies are in 10 kHz units, the unit of the firmware interface. */
#define FM_FREQ_MIN		8750
#define FM_FREQ_MAX		10800
#define FM_FREQ_DEFAULT		8750

/* V4L2 radio frequencies are in 62.5 Hz units: 10 kHz = 160 units. */
#define FM_FREQ_TO_V4L2(f)	((u32)(f) * 160)

#define FM_VOLUME_MAX		15

#define FM_RDS_FIFO_SIZE	512

/* One group of the RDS data event */
struct mtk_fm_rds_group {
	__le16 blk[4];
	__le16 cbc;	/* corrected bit count, one nibble per block (A..D) */
	__le16 crc;	/* good-block flags: A = 0x8, B = 0x4, C = 0x2, D = 0x1 */
} __packed;

/* Channel quality report of the soft-mute tune command */
struct mtk_fm_cqi {
	__le16 ch;
	__le16 rssi;
	__le16 pamd;
	__le16 pr;
	__le16 fpamd;
	__le16 mr;
	__le16 atdc;
	__le16 prx;
	__le16 atdev;
	__le16 smg;
	__le16 drssi;
} __packed;

struct mtk_fm {
	struct device *dev;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *ctrl_mute;

	/* Serializes the V4L2 ioctls, power changes and all commands */
	struct mutex lock;
	unsigned int users;
	bool powered;
	bool reset;	/* the WMT core reset the chip under us */

	/* Command transport */
	u8 tx[FM_TX_BUF_SIZE];
	spinlock_t evt_lock;	/* evt_flags and the read results below */
	u32 evt_flags;
	wait_queue_head_t evt_wait;
	u16 fspi_rd;
	u32 cspi_rd;
	u8 cqi[FM_CQI_SIZE];
	unsigned int cqi_len;

	/* Event parser, runs on rx_wq */
	struct workqueue_struct *rx_wq;
	struct work_struct rx_work;
	u8 rx_hdr[FM_PKT_HDR_LEN];
	unsigned int rx_hdr_len;
	u8 rx_pkt[FM_RX_PKT_MAX];
	unsigned int rx_len;
	unsigned int rx_need;

	/* Chip state */
	u16 hw_id;
	u32 eco_ver;
	int rom_ver;
	u16 patch_ver;
	u16 freq;
	bool mono;
	bool deemph_75us;
	u8 volume;
	bool muted;

	/* RDS blocks for read() */
	DECLARE_KFIFO(rds_fifo, struct v4l2_rds_data, FM_RDS_FIFO_SIZE);
	spinlock_t rds_lock;	/* rds_fifo */
	wait_queue_head_t rds_wait;

	struct dentry *debugfs;
	char dbg_result[384];
};

/* fm_link.c: STP transport and event parser */
int mtk_fm_link_init(struct mtk_fm *fm);
void mtk_fm_link_exit(struct mtk_fm *fm);
void mtk_fm_link_start(struct mtk_fm *fm);
void mtk_fm_link_stop(struct mtk_fm *fm);
int mtk_fm_cmd(struct mtk_fm *fm, size_t len, u32 mask,
	       unsigned int timeout_ms);
void mtk_fm_link_abort(struct mtk_fm *fm);

/* fm_mt6627.c: command sequences of the FM core */
int mt6627_read(struct mtk_fm *fm, u8 addr, u16 *val);
int mt6627_write(struct mtk_fm *fm, u8 addr, u16 val);
int mt6627_set_bits(struct mtk_fm *fm, u8 addr, u16 bits, u16 mask);
int mt6627_top_read(struct mtk_fm *fm, u16 addr, u32 *val);
int mt6627_top_write(struct mtk_fm *fm, u16 addr, u32 val);
int mt6627_host_read(struct mtk_fm *fm, u32 addr, u32 *val);
int mt6627_host_write(struct mtk_fm *fm, u32 addr, u32 val);
int mt6627_power_up(struct mtk_fm *fm);
int mt6627_power_down(struct mtk_fm *fm);
int mt6627_rampdown(struct mtk_fm *fm);
int mt6627_tune(struct mtk_fm *fm, u16 freq);
int mt6627_set_mute(struct mtk_fm *fm, bool mute);
int mt6627_set_volume(struct mtk_fm *fm, u8 vol);
int mt6627_set_mono(struct mtk_fm *fm, bool mono);
int mt6627_set_deemphasis(struct mtk_fm *fm, bool us75);
int mt6627_get_signal(struct mtk_fm *fm, int *rssi_dbm, bool *stereo);
int mt6627_i2s_pinmux(struct mtk_fm *fm);
int mt6627_i2s_apply(struct mtk_fm *fm);
int mt6627_i2s_dump(struct mtk_fm *fm, char *buf, size_t size);
int mt6627_search_begin(struct mtk_fm *fm);
int mt6627_search_end(struct mtk_fm *fm);
int mt6627_probe_channel(struct mtk_fm *fm, u16 freq, bool *valid);
int mt6627_rds_enable(struct mtk_fm *fm, bool on);

/* fm_v4l2.c */
void mtk_fm_rds_event(struct mtk_fm *fm, const u8 *data, unsigned int len);

#endif /* MTK_FM_H */
