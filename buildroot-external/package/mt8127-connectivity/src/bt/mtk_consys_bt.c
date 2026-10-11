// SPDX-License-Identifier: GPL-2.0
/*
 * Bluetooth HCI driver for the MediaTek MT8127 CONSYS.
 *
 * HCI packets travel on the BT channel (BT_TASK_INDX) of the STP link that the
 * WMT core runs over BTIF. Each STP payload is an H:4 packet, packet type
 * indicator included, in both directions: the byte stream the stock
 * /dev/stpbt character device exchanged with user space. The WMT core powers
 * the BT function (mtk_wcn_wmt_func_on), received payloads are queued in the
 * STP BT ring and signalled through the STP event callback.
 *
 * The controller setup is the stock Android vendor library sequence
 * (libbluetooth_mtk.so, bt_init_script_consys), see docs/bt.md.
 */

#include <linux/hex.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "stp_exp.h"
#include "wmt_exp.h"

#define CONSYS_BT_MAX_PKT	4095	/* 12-bit STP length field */
#define CONSYS_BT_TX_TIMEOUT	msecs_to_jiffies(2000)

#define HCI_OP_MTK_SET_BDADDR		0xfc1a
#define HCI_OP_MTK_SET_RADIO		0xfc79
#define HCI_OP_MTK_SET_SLEEP_TIMEOUT	0xfc7a
#define HCI_OP_MTK_SET_TX_PWR_OFFSET	0xfc93

enum {
	CONSYS_BT_RUNNING,	/* BT function on, STP callbacks registered */
	CONSYS_BT_RESETTING,	/* WMT whole-chip reset in progress */
	CONSYS_BT_TX_RESUME,	/* STP signalled free TX window space */
};

struct consys_bt_pkt {
	u8 type;
	u8 hlen;	/* header length after the type byte */
	u8 loff;	/* offset of the payload length field in the header */
	u8 lsize;	/* size of the payload length field (1 or 2) */
	u16 maxlen;
};

static const struct consys_bt_pkt consys_bt_pkts[] = {
	{ HCI_ACLDATA_PKT, HCI_ACL_HDR_SIZE, 2, 2, HCI_MAX_FRAME_SIZE },
	{ HCI_SCODATA_PKT, HCI_SCO_HDR_SIZE, 2, 1, HCI_SCO_HDR_SIZE + HCI_MAX_SCO_SIZE },
	{ HCI_EVENT_PKT, HCI_EVENT_HDR_SIZE, 1, 1, HCI_MAX_EVENT_SIZE },
};

struct consys_bt {
	struct hci_dev *hdev;
	struct workqueue_struct *wq;
	struct work_struct rx_work;
	struct work_struct tx_work;
	struct sk_buff_head txq;
	wait_queue_head_t tx_wait;
	unsigned long flags;

	struct sk_buff *rx_skb;
	const struct consys_bt_pkt *rx_pkt;
	u8 rx_buf[2048];

	bdaddr_t fallback_addr;
};

/* The STP and WMT callbacks carry no context: there is one BT channel. */
static struct consys_bt *consys_bt;

static char *bdaddr;
module_param(bdaddr, charp, 0444);
MODULE_PARM_DESC(bdaddr, "Public BD address (xx:xx:xx:xx:xx:xx), overrides the controller eFUSE");

/* Address in the stock NVRAM default and unprogrammed eFUSE: 00:00:46:81:27:01 */
static const bdaddr_t consys_bt_default_addr = {{ 0x01, 0x27, 0x81, 0x46, 0x00, 0x00 }};

/* Stock NVRAM defaults (libcustom_nvram.so, ap_nvram_btradio_struct) */
static const u8 consys_bt_radio[] = { 0x07, 0x80, 0x00, 0x06, 0x05, 0x07 };
static const u8 consys_bt_tx_pwr_offset[] = { 0xff, 0xff, 0xff };
static const u8 consys_bt_sleep[] = { 0x03, 0x40, 0x1f, 0x40, 0x1f, 0x00, 0x04 };

static const struct consys_bt_pkt *consys_bt_find_pkt(u8 type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(consys_bt_pkts); i++)
		if (consys_bt_pkts[i].type == type)
			return &consys_bt_pkts[i];
	return NULL;
}

static void consys_bt_rx_drop(struct consys_bt *bt)
{
	kfree_skb(bt->rx_skb);
	bt->rx_skb = NULL;
	bt->hdev->stat.err_rx++;
}

/* Reassemble H:4 packets from the STP BT byte stream. */
/*
 * The firmware rejects Read Local Extended Features with a status outside the HCI specification although it
 * advertises the command, which aborts the controller initialisation. Such a reply is replaced with a successful one
 * reporting no extended features (page 1 zeroed, maximum page 1).
 */
static struct sk_buff *consys_bt_fix_event(struct sk_buff *skb)
{
	struct hci_ev_cmd_complete *cc;
	struct hci_event_hdr *hdr;
	struct sk_buff *fixed;
	u8 *p;

	if (hci_skb_pkt_type(skb) != HCI_EVENT_PKT || skb->len < HCI_EVENT_HDR_SIZE + sizeof(*cc) + 1)
		return skb;
	hdr = (struct hci_event_hdr *)skb->data;
	cc = (struct hci_ev_cmd_complete *)(skb->data + HCI_EVENT_HDR_SIZE);
	if (hdr->evt != HCI_EV_CMD_COMPLETE || le16_to_cpu(cc->opcode) != HCI_OP_READ_LOCAL_EXT_FEATURES ||
	    !skb->data[HCI_EVENT_HDR_SIZE + sizeof(*cc)])
		return skb;

	fixed = bt_skb_alloc(HCI_EVENT_HDR_SIZE + sizeof(*cc) + 11, GFP_KERNEL);
	if (!fixed)
		return skb;
	hci_skb_pkt_type(fixed) = HCI_EVENT_PKT;
	p = skb_put_zero(fixed, HCI_EVENT_HDR_SIZE + sizeof(*cc) + 11);
	p[0] = HCI_EV_CMD_COMPLETE;
	p[1] = sizeof(*cc) + 11;
	memcpy(p + HCI_EVENT_HDR_SIZE, cc, sizeof(*cc));
	p[HCI_EVENT_HDR_SIZE + sizeof(*cc)] = 0;		/* status */
	p[HCI_EVENT_HDR_SIZE + sizeof(*cc) + 1] = 1;	/* page */
	p[HCI_EVENT_HDR_SIZE + sizeof(*cc) + 2] = 1;	/* maximum page */
	kfree_skb(skb);
	return fixed;
}

static void consys_bt_recv(struct consys_bt *bt, const u8 *data, unsigned int count)
{
	struct hci_dev *hdev = bt->hdev;

	while (count) {
		const struct consys_bt_pkt *pkt;
		struct sk_buff *skb = bt->rx_skb;
		unsigned int len, dlen;

		if (!skb) {
			pkt = consys_bt_find_pkt(*data);
			if (!pkt) {
				bt_dev_err(hdev, "unknown packet type 0x%02x, dropping %u bytes",
					   *data, count);
				hdev->stat.err_rx++;
				return;
			}
			skb = bt_skb_alloc(pkt->maxlen, GFP_KERNEL);
			if (!skb) {
				hdev->stat.err_rx++;
				return;
			}
			hci_skb_pkt_type(skb) = pkt->type;
			hci_skb_expect(skb) = pkt->hlen;
			bt->rx_skb = skb;
			bt->rx_pkt = pkt;
			data++;
			count--;
			continue;
		}

		pkt = bt->rx_pkt;
		len = min_t(unsigned int, hci_skb_expect(skb) - skb->len, count);
		skb_put_data(skb, data, len);
		data += len;
		count -= len;
		if (skb->len < hci_skb_expect(skb))
			continue;

		if (skb->len == pkt->hlen) {
			dlen = pkt->lsize == 1 ? skb->data[pkt->loff] :
					 get_unaligned_le16(skb->data + pkt->loff);
			if (pkt->hlen + dlen > pkt->maxlen) {
				bt_dev_err(hdev, "packet type 0x%02x too long (%u)",
					   pkt->type, dlen);
				consys_bt_rx_drop(bt);
				return;
			}
			if (dlen) {
				hci_skb_expect(skb) += dlen;
				continue;
			}
		}

		bt->rx_skb = NULL;
		hdev->stat.byte_rx += skb->len;
		hci_recv_frame(hdev, consys_bt_fix_event(skb));
	}
}

static void consys_bt_rx_work(struct work_struct *work)
{
	struct consys_bt *bt = container_of(work, struct consys_bt, rx_work);
	int len;

	while ((len = mtk_wcn_stp_receive_data(bt->rx_buf, sizeof(bt->rx_buf),
					       BT_TASK_INDX)) > 0)
		consys_bt_recv(bt, bt->rx_buf, len);
}

/* STP RX event callback, may run in atomic context. */
static void consys_bt_rx_event(void)
{
	struct consys_bt *bt = consys_bt;

	if (bt && test_bit(CONSYS_BT_RUNNING, &bt->flags))
		queue_work(bt->wq, &bt->rx_work);
}

/* STP TX window resume callback, shared by all STP channels. */
static void consys_bt_tx_event(void)
{
	struct consys_bt *bt = consys_bt;

	if (bt) {
		set_bit(CONSYS_BT_TX_RESUME, &bt->flags);
		wake_up(&bt->tx_wait);
	}
}

static bool consys_bt_tx_allowed(struct consys_bt *bt)
{
	return test_bit(CONSYS_BT_RUNNING, &bt->flags) &&
	       !test_bit(CONSYS_BT_RESETTING, &bt->flags);
}

static int consys_bt_tx_one(struct consys_bt *bt, struct sk_buff *skb)
{
	unsigned long deadline = jiffies + CONSYS_BT_TX_TIMEOUT;
	int ret;

	for (;;) {
		if (!consys_bt_tx_allowed(bt))
			return -ENETDOWN;

		clear_bit(CONSYS_BT_TX_RESUME, &bt->flags);
		ret = mtk_wcn_stp_send_data(skb->data, skb->len, BT_TASK_INDX);
		if (ret == skb->len)
			return 0;
		if (ret < 0)
			return -EIO;

		/* No STP window space: wait for the core to resume TX. */
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		wait_event_timeout(bt->tx_wait,
				   test_bit(CONSYS_BT_TX_RESUME, &bt->flags) ||
				   !consys_bt_tx_allowed(bt),
				   msecs_to_jiffies(20));
	}
}

static void consys_bt_tx_work(struct work_struct *work)
{
	struct consys_bt *bt = container_of(work, struct consys_bt, tx_work);
	struct hci_dev *hdev = bt->hdev;
	struct sk_buff *skb;
	int ret;

	while ((skb = skb_dequeue(&bt->txq))) {
		ret = consys_bt_tx_one(bt, skb);
		if (ret) {
			bt_dev_err(hdev, "TX of %u bytes failed (%d)", skb->len, ret);
			hdev->stat.err_tx++;
			kfree_skb(skb);
			continue;
		}

		hdev->stat.byte_tx += skb->len;
		switch (hci_skb_pkt_type(skb)) {
		case HCI_COMMAND_PKT:
			hdev->stat.cmd_tx++;
			break;
		case HCI_ACLDATA_PKT:
			hdev->stat.acl_tx++;
			break;
		case HCI_SCODATA_PKT:
			hdev->stat.sco_tx++;
			break;
		}
		kfree_skb(skb);
	}
}

static void consys_bt_rst_cb(ENUM_WMTDRV_TYPE_T src, ENUM_WMTDRV_TYPE_T dst,
			     ENUM_WMTMSG_TYPE_T type, void *buf, UINT32 sz)
{
	struct consys_bt *bt = consys_bt;
	ENUM_WMTRSTMSG_TYPE_T msg;

	if (!bt || src != WMTDRV_TYPE_WMT || dst != WMTDRV_TYPE_BT ||
	    type != WMTMSG_TYPE_RESET || sz != sizeof(msg))
		return;

	memcpy(&msg, buf, sizeof(msg));
	switch (msg) {
	case WMTRSTMSG_RESET_START:
		bt_dev_warn(bt->hdev, "CONSYS reset started");
		set_bit(CONSYS_BT_RESETTING, &bt->flags);
		break;
	case WMTRSTMSG_RESET_END:
		bt_dev_warn(bt->hdev, "CONSYS reset done, restarting");
		clear_bit(CONSYS_BT_RESETTING, &bt->flags);
		/* Closes and reopens the device from the HCI core. */
		hci_reset_dev(bt->hdev);
		break;
	default:
		bt_dev_err(bt->hdev, "CONSYS reset failed (%d)", msg);
		break;
	}
}

static int consys_bt_open(struct hci_dev *hdev)
{
	struct consys_bt *bt = hci_get_drvdata(hdev);

	if (mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE) {
		bt_dev_err(hdev, "WMT BT function on failed");
		return -ENODEV;
	}

	if (!mtk_wcn_stp_is_ready()) {
		bt_dev_err(hdev, "STP is not ready");
		mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT);
		return -ENODEV;
	}

	clear_bit(CONSYS_BT_RESETTING, &bt->flags);
	mtk_wcn_wmt_msgcb_reg(WMTDRV_TYPE_BT, consys_bt_rst_cb);
	mtk_wcn_stp_set_bluez(0);
	mtk_wcn_stp_register_tx_event_cb(BT_TASK_INDX, consys_bt_tx_event);
	set_bit(CONSYS_BT_RUNNING, &bt->flags);
	/* Also flushes the BT RX ring; until now the STP core dropped BT frames. */
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, consys_bt_rx_event);

	return 0;
}

static int consys_bt_close(struct hci_dev *hdev)
{
	struct consys_bt *bt = hci_get_drvdata(hdev);

	clear_bit(CONSYS_BT_RUNNING, &bt->flags);
	wake_up(&bt->tx_wait);
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);
	mtk_wcn_stp_register_tx_event_cb(BT_TASK_INDX, NULL);
	mtk_wcn_wmt_msgcb_unreg(WMTDRV_TYPE_BT);

	cancel_work_sync(&bt->rx_work);
	cancel_work_sync(&bt->tx_work);
	skb_queue_purge(&bt->txq);
	kfree_skb(bt->rx_skb);
	bt->rx_skb = NULL;

	if (mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE) {
		bt_dev_err(hdev, "WMT BT function off failed");
		return -EIO;
	}

	return 0;
}

static int consys_bt_flush(struct hci_dev *hdev)
{
	struct consys_bt *bt = hci_get_drvdata(hdev);

	skb_queue_purge(&bt->txq);
	return 0;
}

static int consys_bt_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct consys_bt *bt = hci_get_drvdata(hdev);

	if (skb->len + 1 > CONSYS_BT_MAX_PKT)
		return -EMSGSIZE;
	if (skb_cow_head(skb, 1))
		return -ENOMEM;

	/* The STP BT channel carries H:4 packets. */
	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);
	skb_queue_tail(&bt->txq, skb);
	queue_work(bt->wq, &bt->tx_work);

	return 0;
}

static int consys_bt_vendor_cmd(struct hci_dev *hdev, u16 opcode, u32 plen,
				const void *param)
{
	int ret;

	ret = __hci_cmd_sync_status(hdev, opcode, plen, param, HCI_INIT_TIMEOUT);
	if (ret > 0) {
		bt_dev_err(hdev, "opcode 0x%04x failed, status 0x%02x", opcode, ret);
		return -bt_to_errno(ret);
	}
	return ret;
}

static int consys_bt_set_bdaddr(struct hci_dev *hdev, const bdaddr_t *addr)
{
	return consys_bt_vendor_cmd(hdev, HCI_OP_MTK_SET_BDADDR, sizeof(*addr), addr);
}

static bool consys_bt_addr_valid(const bdaddr_t *addr)
{
	return bacmp(addr, BDADDR_ANY) && bacmp(addr, &consys_bt_default_addr);
}

/*
 * Address priority: one set through mgmt (btmgmt public-addr), the bdaddr
 * module parameter, the controller eFUSE unless it holds the chip default,
 * and last a random address chosen at module load.
 */
static void consys_bt_pick_bdaddr(struct consys_bt *bt, bdaddr_t *addr)
{
	struct hci_dev *hdev = bt->hdev;
	struct hci_rp_read_bd_addr *rp;
	struct sk_buff *skb;
	u8 mac[6];
	int i;

	if (bacmp(&hdev->public_addr, BDADDR_ANY)) {
		bacpy(addr, &hdev->public_addr);
		return;
	}

	if (bdaddr && *bdaddr) {
		if (mac_pton(bdaddr, mac)) {
			for (i = 0; i < 6; i++)
				addr->b[i] = mac[5 - i];
			return;
		}
		bt_dev_warn(hdev, "invalid bdaddr parameter '%s'", bdaddr);
	}

	skb = __hci_cmd_sync(hdev, HCI_OP_READ_BD_ADDR, 0, NULL, HCI_INIT_TIMEOUT);
	if (!IS_ERR(skb)) {
		rp = (struct hci_rp_read_bd_addr *)skb->data;
		if (skb->len >= sizeof(*rp) && !rp->status &&
		    consys_bt_addr_valid(&rp->bdaddr)) {
			bacpy(addr, &rp->bdaddr);
			kfree_skb(skb);
			return;
		}
		kfree_skb(skb);
	}

	bt_dev_warn(hdev, "no BD address configured, using random %pMR",
		    &bt->fallback_addr);
	bacpy(addr, &bt->fallback_addr);
}

static int consys_bt_setup(struct hci_dev *hdev)
{
	struct consys_bt *bt = hci_get_drvdata(hdev);
	bdaddr_t addr;
	int ret;

	consys_bt_pick_bdaddr(bt, &addr);
	ret = consys_bt_set_bdaddr(hdev, &addr);
	if (ret)
		return ret;

	ret = consys_bt_vendor_cmd(hdev, HCI_OP_MTK_SET_RADIO,
				   sizeof(consys_bt_radio), consys_bt_radio);
	if (ret)
		return ret;

	ret = consys_bt_vendor_cmd(hdev, HCI_OP_MTK_SET_TX_PWR_OFFSET,
				   sizeof(consys_bt_tx_pwr_offset),
				   consys_bt_tx_pwr_offset);
	if (ret)
		return ret;

	ret = consys_bt_vendor_cmd(hdev, HCI_OP_MTK_SET_SLEEP_TIMEOUT,
				   sizeof(consys_bt_sleep), consys_bt_sleep);
	if (ret)
		return ret;

	/* The HCI core init sends HCI_Reset next, the last stock step. */
	bt_dev_info(hdev, "BD address %pMR", &addr);
	return 0;
}

static int __init consys_bt_init(void)
{
	struct consys_bt *bt;
	struct hci_dev *hdev;
	int ret;

	bt = kzalloc(sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;

	bt->wq = alloc_ordered_workqueue("mtk_consys_bt", WQ_HIGHPRI);
	if (!bt->wq) {
		ret = -ENOMEM;
		goto err_free;
	}
	INIT_WORK(&bt->rx_work, consys_bt_rx_work);
	INIT_WORK(&bt->tx_work, consys_bt_tx_work);
	skb_queue_head_init(&bt->txq);
	init_waitqueue_head(&bt->tx_wait);

	/* Locally administered unicast, as the stock random address. */
	get_random_bytes(bt->fallback_addr.b, sizeof(bt->fallback_addr.b));
	bt->fallback_addr.b[5] = (bt->fallback_addr.b[5] & 0xfc) | 0x02;

	hdev = hci_alloc_dev();
	if (!hdev) {
		ret = -ENOMEM;
		goto err_wq;
	}
	bt->hdev = hdev;
	hci_set_drvdata(hdev, bt);

	/* BTIF is a UART-type link to the CONSYS MCU. */
	hdev->bus = HCI_UART;
	hdev->manufacturer = 70;	/* MediaTek */
	hdev->open = consys_bt_open;
	hdev->close = consys_bt_close;
	hdev->flush = consys_bt_flush;
	hdev->setup = consys_bt_setup;
	hdev->send = consys_bt_send;
	hdev->set_bdaddr = consys_bt_set_bdaddr;
	/* The function off/on cycle on close/open loses the vendor settings. */
	hci_set_quirk(hdev, HCI_QUIRK_NON_PERSISTENT_SETUP);

	consys_bt = bt;
	ret = hci_register_dev(hdev);
	if (ret < 0) {
		consys_bt = NULL;
		goto err_hdev;
	}

	return 0;

err_hdev:
	hci_free_dev(hdev);
err_wq:
	destroy_workqueue(bt->wq);
err_free:
	kfree(bt);
	return ret;
}

static void __exit consys_bt_exit(void)
{
	struct consys_bt *bt = consys_bt;

	hci_unregister_dev(bt->hdev);
	consys_bt = NULL;
	hci_free_dev(bt->hdev);
	destroy_workqueue(bt->wq);
	kfree(bt);
}

module_init(consys_bt_init);
module_exit(consys_bt_exit);

MODULE_DESCRIPTION("MediaTek MT8127 CONSYS Bluetooth HCI driver");
MODULE_LICENSE("GPL");
