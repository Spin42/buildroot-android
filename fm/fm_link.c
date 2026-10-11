// SPDX-License-Identifier: GPL-2.0
/*
 * FM channel of STP: commands go out with mtk_wcn_stp_send_data() on
 * FM_TASK_INDX, events come back through the STP event callback and
 * mtk_wcn_stp_receive_data().
 *
 * Every command is a packet [0x01, opcode, len_lo, len_hi, payload] and
 * is answered by one or more events [0x04, opcode, len_lo, len_hi,
 * payload]. Each event sets a completion flag; a command waits until all
 * the flags of its mask are set. The flag and payload rules follow the
 * vendor fm_link.c (fm_event_parser).
 */

#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "osal_typedef.h"
#include "stp_exp.h"

#include "mtk_fm.h"

/* The STP event callback carries no context: one FM core per system. */
static struct mtk_fm *mtk_fm_link_dev;

static void mtk_fm_stp_event(void)
{
	struct mtk_fm *fm = READ_ONCE(mtk_fm_link_dev);

	if (fm)
		queue_work(fm->rx_wq, &fm->rx_work);
}

static void mtk_fm_evt_complete(struct mtk_fm *fm, u32 flags)
{
	unsigned long irqflags;

	spin_lock_irqsave(&fm->evt_lock, irqflags);
	fm->evt_flags |= flags;
	spin_unlock_irqrestore(&fm->evt_lock, irqflags);
	wake_up(&fm->evt_wait);
}

static void mtk_fm_dispatch(struct mtk_fm *fm, u8 op, const u8 *p,
			    unsigned int len)
{
	unsigned long irqflags;
	u32 flags = 0;

	print_hex_dump_debug("mtk-fm evt: ", DUMP_PREFIX_NONE, 16, 1, p,
			     min(len, 32U), false);
	dev_dbg(fm->dev, "event op 0x%02x len %u\n", op, len);

	if (op == FM_OP_CSPI_WRITE) {
		mtk_fm_evt_complete(fm, FM_FLAG_CSPI_WRITE);
		return;
	}

	if (!len) {
		if (op < 32)
			mtk_fm_evt_complete(fm, FM_FLAG(op));
		return;
	}

	spin_lock_irqsave(&fm->evt_lock, irqflags);
	switch (op) {
	case FM_OP_TUNE:
		if (len == 1 && p[0] == 1)
			flags = FM_FLAG_TUNE_DONE;
		break;
	case FM_OP_SOFT_MUTE_TUNE:
		if (len >= 2) {
			fm->cqi_len = min_t(unsigned int, len, FM_CQI_SIZE);
			memcpy(fm->cqi, p, fm->cqi_len);
			flags = FM_FLAG(op);
		}
		break;
	case FM_OP_SEEK:
		flags = FM_FLAG_SEEK_DONE;
		break;
	case FM_OP_SCAN:
		flags = FM_FLAG_SCAN_DONE;
		break;
	case FM_OP_FSPI_READ:
		if (len >= 2)
			fm->fspi_rd = get_unaligned_le16(p);
		flags = FM_FLAG(op);
		break;
	case FM_OP_CSPI_READ:
		if (len >= 4)
			fm->cspi_rd = get_unaligned_le32(p);
		flags = FM_FLAG_CSPI_READ;
		break;
	case FM_OP_HOST_READ:
		if (len >= 4)
			fm->cspi_rd = get_unaligned_le32(p);
		flags = FM_FLAG(op);
		break;
	case FM_OP_RDS_DATA:
		flags = FM_FLAG(op);
		break;
	default:
		if (op < 32)
			flags = FM_FLAG(op);
		break;
	}
	fm->evt_flags |= flags;
	spin_unlock_irqrestore(&fm->evt_lock, irqflags);

	if (op == FM_OP_RDS_DATA)
		mtk_fm_rds_event(fm, p, len);

	if (flags)
		wake_up(&fm->evt_wait);
}

static void mtk_fm_parse(struct mtk_fm *fm, const u8 *buf, unsigned int n)
{
	unsigned int i = 0;

	while (i < n) {
		if (fm->rx_hdr_len < FM_PKT_HDR_LEN) {
			u8 c = buf[i++];

			if (fm->rx_hdr_len == 0 && c != FM_PKT_EVT) {
				dev_warn_ratelimited(fm->dev,
						     "bad event type 0x%02x\n",
						     c);
				continue;
			}
			fm->rx_hdr[fm->rx_hdr_len++] = c;
			if (fm->rx_hdr_len < FM_PKT_HDR_LEN)
				continue;

			fm->rx_need = get_unaligned_le16(&fm->rx_hdr[2]);
			fm->rx_len = 0;
		} else {
			unsigned int take = min(n - i, fm->rx_need - fm->rx_len);

			/* payload beyond FM_RX_PKT_MAX is consumed, not kept */
			if (fm->rx_len < FM_RX_PKT_MAX)
				memcpy(&fm->rx_pkt[fm->rx_len], &buf[i],
				       min(take, FM_RX_PKT_MAX - fm->rx_len));
			fm->rx_len += take;
			i += take;
		}

		if (fm->rx_len == fm->rx_need) {
			if (fm->rx_need > FM_RX_PKT_MAX)
				dev_warn(fm->dev,
					 "event 0x%02x truncated (%u bytes)\n",
					 fm->rx_hdr[1], fm->rx_need);
			mtk_fm_dispatch(fm, fm->rx_hdr[1], fm->rx_pkt,
					min_t(unsigned int, fm->rx_need,
					      FM_RX_PKT_MAX));
			fm->rx_hdr_len = 0;
			fm->rx_len = 0;
			fm->rx_need = 0;
		}
	}
}

static void mtk_fm_rx_work(struct work_struct *work)
{
	struct mtk_fm *fm = container_of(work, struct mtk_fm, rx_work);
	u8 buf[256];
	int n;

	while ((n = mtk_wcn_stp_receive_data(buf, sizeof(buf),
					     FM_TASK_INDX)) > 0)
		mtk_fm_parse(fm, buf, n);
}

/*
 * Sends the command built in fm->tx and waits for all flags of @mask.
 * Called with fm->lock held.
 */
int mtk_fm_cmd(struct mtk_fm *fm, size_t len, u32 mask,
	       unsigned int timeout_ms)
{
	unsigned long irqflags;
	long left;
	int ret;

	if (fm->reset)
		return -EIO;

	spin_lock_irqsave(&fm->evt_lock, irqflags);
	fm->evt_flags &= ~mask;
	spin_unlock_irqrestore(&fm->evt_lock, irqflags);

	print_hex_dump_debug("mtk-fm cmd: ", DUMP_PREFIX_NONE, 16, 1, fm->tx,
			     min_t(size_t, len, 32), false);

	ret = mtk_wcn_stp_send_data(fm->tx, len, FM_TASK_INDX);
	if (ret <= 0) {
		dev_err(fm->dev, "STP send of op 0x%02x failed: %d\n",
			fm->tx[1], ret);
		return -EIO;
	}

	left = wait_event_timeout(fm->evt_wait,
				  (READ_ONCE(fm->evt_flags) & mask) == mask ||
				  READ_ONCE(fm->reset),
				  msecs_to_jiffies(timeout_ms));
	if (fm->reset)
		return -EIO;
	if (!left) {
		dev_err(fm->dev, "op 0x%02x timed out (flags 0x%08x, want 0x%08x)\n",
			fm->tx[1], READ_ONCE(fm->evt_flags), mask);
		return -ETIMEDOUT;
	}

	spin_lock_irqsave(&fm->evt_lock, irqflags);
	fm->evt_flags &= ~mask;
	spin_unlock_irqrestore(&fm->evt_lock, irqflags);

	return 0;
}

/* Wakes up a command waiting for an event that will not come. */
void mtk_fm_link_abort(struct mtk_fm *fm)
{
	wake_up(&fm->evt_wait);
}

/* Starts receiving events; the STP core drops queued FM data here. */
void mtk_fm_link_start(struct mtk_fm *fm)
{
	fm->rx_hdr_len = 0;
	fm->rx_len = 0;
	fm->rx_need = 0;
	fm->evt_flags = 0;
	mtk_wcn_stp_register_event_cb(FM_TASK_INDX, mtk_fm_stp_event);
}

void mtk_fm_link_stop(struct mtk_fm *fm)
{
	mtk_wcn_stp_register_event_cb(FM_TASK_INDX, NULL);
	flush_workqueue(fm->rx_wq);
}

int mtk_fm_link_init(struct mtk_fm *fm)
{
	spin_lock_init(&fm->evt_lock);
	init_waitqueue_head(&fm->evt_wait);
	INIT_WORK(&fm->rx_work, mtk_fm_rx_work);

	fm->rx_wq = alloc_ordered_workqueue("mtk_fm_rx", WQ_HIGHPRI);
	if (!fm->rx_wq)
		return -ENOMEM;

	WRITE_ONCE(mtk_fm_link_dev, fm);
	return 0;
}

void mtk_fm_link_exit(struct mtk_fm *fm)
{
	WRITE_ONCE(mtk_fm_link_dev, NULL);
	destroy_workqueue(fm->rx_wq);
}
