// SPDX-License-Identifier: GPL-2.0
/*
 * V4L2 radio device for the FM receiver of the MT8127 CONSYS.
 *
 * The FM core powers up at the first open of the radio device. At the last
 * close it powers down only if the mute control is set; otherwise it keeps
 * playing, so one-shot tools such as v4l2-ctl can drive it. The audio
 * leaves the FM core as 32 kHz I2S towards the audio front-end of the SoC.
 */

#include <linux/debugfs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>

#include "osal_typedef.h"
#include "wmt_exp.h"

#include "mtk_fm.h"

#define DRIVER_NAME		"mtk-consys-fm"

/* RSSI range reported as 0..65535 tuner signal */
#define FM_RSSI_DBM_LOW		(-120)
#define FM_RSSI_DBM_HIGH	(-40)

static struct platform_device *mtk_fm_pdev;
/* The WMT reset callback carries no context. */
static struct mtk_fm *mtk_fm_wmt_dev;

static void mtk_fm_rds_reset(struct mtk_fm *fm)
{
	unsigned long flags;

	spin_lock_irqsave(&fm->rds_lock, flags);
	kfifo_reset(&fm->rds_fifo);
	spin_unlock_irqrestore(&fm->rds_lock, flags);
}

/*
 * RDS data event: sin and cos words, then groups of blocks A-D, corrected
 * bit counts and good-block flags (vendor fm_rds.h, struct rds_rx_t).
 */
void mtk_fm_rds_event(struct mtk_fm *fm, const u8 *data, unsigned int len)
{
	const struct mtk_fm_rds_group *grp;
	unsigned int n, i, b;
	unsigned long flags;
	bool added = false;

	if (len < 4)
		return;
	n = (len - 4) / sizeof(*grp);
	grp = (const struct mtk_fm_rds_group *)(data + 4);

	spin_lock_irqsave(&fm->rds_lock, flags);
	for (i = 0; i < n; i++) {
		u16 crc = le16_to_cpu(grp[i].crc);
		u16 cbc = le16_to_cpu(grp[i].cbc);
		/* version B groups carry block C' */
		bool ver_b = le16_to_cpu(grp[i].blk[1]) & BIT(11);

		for (b = 0; b < 4; b++) {
			struct v4l2_rds_data rds;
			u16 v = le16_to_cpu(grp[i].blk[b]);

			rds.lsb = v & 0xff;
			rds.msb = v >> 8;
			rds.block = b;
			if (b == 2 && ver_b)
				rds.block = V4L2_RDS_BLOCK_C_ALT;
			if (!(crc & (0x8 >> b)))
				rds.block |= V4L2_RDS_BLOCK_ERROR;
			else if ((cbc >> (12 - 4 * b)) & 0xf)
				rds.block |= V4L2_RDS_BLOCK_CORRECTED;
			/* drop the oldest group when nobody reads */
			if (kfifo_is_full(&fm->rds_fifo))
				kfifo_skip(&fm->rds_fifo);
			kfifo_put(&fm->rds_fifo, rds);
			added = true;
		}
	}
	spin_unlock_irqrestore(&fm->rds_lock, flags);

	if (added)
		wake_up_interruptible(&fm->rds_wait);
}

static int mtk_fm_power_on(struct mtk_fm *fm)
{
	int ret;

	fm->reset = false;
	mtk_fm_link_start(fm);

	if (!mtk_wcn_wmt_func_on(WMTDRV_TYPE_FM)) {
		dev_err(fm->dev, "WMT failed to switch the FM function on\n");
		ret = -EIO;
		goto err_link;
	}

	ret = mt6627_power_up(fm);
	if (ret) {
		dev_err(fm->dev, "power-up sequence failed: %d\n", ret);
		goto err_func;
	}

	/* CONSYS pins in FM I2S mode (stock AUDDRV_SET_FM_I2S_GPIO) */
	ret = mtk_wcn_cmb_stub_aif_ctrl(CMB_STUB_AIF_2, CMB_STUB_AIF_CTRL_EN);
	if (ret)
		dev_warn(fm->dev, "CONSYS audio interface setup failed: %d\n",
			 ret);

	fm->powered = true;

	ret = mt6627_rampdown(fm);
	if (!ret)
		ret = mt6627_tune(fm, fm->freq);
	if (!ret)
		ret = mt6627_set_mono(fm, fm->mono);
	if (!ret)
		ret = mt6627_set_volume(fm, fm->volume);
	if (!ret)
		ret = mt6627_set_mute(fm, fm->muted);
	if (!ret)
		ret = mt6627_rds_enable(fm, true);
	if (ret) {
		dev_err(fm->dev, "initial setup failed: %d\n", ret);
		fm->powered = false;
		mt6627_power_down(fm);
		goto err_aif;
	}

	mtk_fm_rds_reset(fm);
	dev_info(fm->dev, "FM on, %u.%02u MHz\n", fm->freq / 100,
		 fm->freq % 100);
	return 0;

err_aif:
	mtk_wcn_cmb_stub_aif_ctrl(CMB_STUB_AIF_0, CMB_STUB_AIF_CTRL_EN);
err_func:
	mtk_wcn_wmt_func_off(WMTDRV_TYPE_FM);
err_link:
	mtk_fm_link_stop(fm);
	return ret;
}

static void mtk_fm_power_off(struct mtk_fm *fm)
{
	if (!fm->powered)
		return;
	fm->powered = false;

	if (!fm->reset) {
		mt6627_rds_enable(fm, false);
		mt6627_power_down(fm);
		mtk_wcn_cmb_stub_aif_ctrl(CMB_STUB_AIF_0, CMB_STUB_AIF_CTRL_EN);
	}
	mtk_wcn_wmt_func_off(WMTDRV_TYPE_FM);
	mtk_fm_link_stop(fm);
	wake_up_interruptible(&fm->rds_wait);
	dev_info(fm->dev, "FM off\n");
}

static void mtk_fm_wmt_msg(ENUM_WMTDRV_TYPE_T src, ENUM_WMTDRV_TYPE_T dst,
			   ENUM_WMTMSG_TYPE_T type, void *buf,
			   unsigned int sz)
{
	struct mtk_fm *fm = READ_ONCE(mtk_fm_wmt_dev);
	ENUM_WMTRSTMSG_TYPE_T msg;

	if (!fm || src != WMTDRV_TYPE_WMT || dst != WMTDRV_TYPE_FM ||
	    type != WMTMSG_TYPE_RESET || !buf || sz < sizeof(msg))
		return;

	memcpy(&msg, buf, sizeof(msg));
	if (msg == WMTRSTMSG_RESET_START) {
		dev_warn(fm->dev, "chip reset: FM state lost, reopen the radio\n");
		WRITE_ONCE(fm->reset, true);
		mtk_fm_link_abort(fm);
	}
}

/* --- V4L2 --- */

static int mtk_fm_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	strscpy(cap->driver, DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, "MT8127 CONSYS FM", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:" DRIVER_NAME, sizeof(cap->bus_info));
	return 0;
}

static int mtk_fm_g_tuner(struct file *file, void *priv,
			  struct v4l2_tuner *t)
{
	struct mtk_fm *fm = video_drvdata(file);
	int rssi = FM_RSSI_DBM_LOW;
	bool stereo = false;
	int ret;

	if (t->index)
		return -EINVAL;

	strscpy(t->name, "FM", sizeof(t->name));
	t->type = V4L2_TUNER_RADIO;
	t->capability = V4L2_TUNER_CAP_LOW | V4L2_TUNER_CAP_STEREO |
			V4L2_TUNER_CAP_RDS | V4L2_TUNER_CAP_RDS_BLOCK_IO |
			V4L2_TUNER_CAP_HWSEEK_BOUNDED |
			V4L2_TUNER_CAP_HWSEEK_WRAP;
	t->rangelow = FM_FREQ_TO_V4L2(FM_FREQ_MIN);
	t->rangehigh = FM_FREQ_TO_V4L2(FM_FREQ_MAX);
	t->audmode = fm->mono ? V4L2_TUNER_MODE_MONO : V4L2_TUNER_MODE_STEREO;
	t->afc = 0;

	if (fm->powered) {
		ret = mt6627_get_signal(fm, &rssi, &stereo);
		if (ret)
			return ret;
	}
	dev_dbg(fm->dev, "rssi %d dBm, %s\n", rssi, stereo ? "stereo" : "mono");

	t->rxsubchans = (stereo ? V4L2_TUNER_SUB_STEREO : V4L2_TUNER_SUB_MONO) |
			V4L2_TUNER_SUB_RDS;
	rssi = clamp(rssi, FM_RSSI_DBM_LOW, FM_RSSI_DBM_HIGH);
	t->signal = (rssi - FM_RSSI_DBM_LOW) * 65535 /
		    (FM_RSSI_DBM_HIGH - FM_RSSI_DBM_LOW);
	return 0;
}

static int mtk_fm_s_tuner(struct file *file, void *priv,
			  const struct v4l2_tuner *t)
{
	struct mtk_fm *fm = video_drvdata(file);
	bool mono;
	int ret;

	if (t->index)
		return -EINVAL;

	mono = t->audmode == V4L2_TUNER_MODE_MONO;
	if (fm->powered) {
		ret = mt6627_set_mono(fm, mono);
		if (ret)
			return ret;
	}
	fm->mono = mono;
	return 0;
}

static int mtk_fm_g_frequency(struct file *file, void *priv,
			      struct v4l2_frequency *f)
{
	struct mtk_fm *fm = video_drvdata(file);

	if (f->tuner)
		return -EINVAL;
	f->type = V4L2_TUNER_RADIO;
	f->frequency = FM_FREQ_TO_V4L2(fm->freq);
	return 0;
}

/* 62.5 Hz units to 10 kHz units on the 50 kHz grid of the chip */
static u16 mtk_fm_freq_from_v4l2(u32 f)
{
	u32 khz10 = DIV_ROUND_CLOSEST(f, 160);

	return clamp_t(u32, DIV_ROUND_CLOSEST(khz10, 5) * 5, FM_FREQ_MIN,
		       FM_FREQ_MAX);
}

static int mtk_fm_set_freq(struct mtk_fm *fm, u16 freq)
{
	int ret;

	if (fm->powered) {
		ret = mt6627_rampdown(fm);
		if (!ret)
			ret = mt6627_tune(fm, freq);
		if (ret)
			return ret;
		mtk_fm_rds_reset(fm);
	}
	fm->freq = freq;
	return 0;
}

static int mtk_fm_s_frequency(struct file *file, void *priv,
			      const struct v4l2_frequency *f)
{
	struct mtk_fm *fm = video_drvdata(file);

	if (f->tuner || f->type != V4L2_TUNER_RADIO)
		return -EINVAL;
	return mtk_fm_set_freq(fm, mtk_fm_freq_from_v4l2(f->frequency));
}

/*
 * The firmware seek of this core is not used by the stock driver; seek
 * steps through the band and measures each channel instead.
 */
static int mtk_fm_s_hw_freq_seek(struct file *file, void *priv,
				 const struct v4l2_hw_freq_seek *seek)
{
	struct mtk_fm *fm = video_drvdata(file);
	u16 low = FM_FREQ_MIN, high = FM_FREQ_MAX, step, freq, found = 0;
	unsigned int n, count;
	bool valid;
	int ret, ret2;

	if (seek->tuner || seek->type != V4L2_TUNER_RADIO)
		return -EINVAL;
	if (file->f_flags & O_NONBLOCK)
		return -EWOULDBLOCK;
	if (!fm->powered)
		return -EIO;

	if (seek->rangelow || seek->rangehigh) {
		low = mtk_fm_freq_from_v4l2(seek->rangelow);
		high = mtk_fm_freq_from_v4l2(seek->rangehigh);
		if (low >= high)
			return -EINVAL;
	}

	switch (seek->spacing) {
	case 50000:
		step = 5;
		break;
	case 200000:
		step = 20;
		break;
	default:
		step = 10;
		break;
	}

	ret = mt6627_search_begin(fm);
	if (ret)
		return ret;

	count = (high - low) / step + 1;
	freq = fm->freq;
	for (n = 0; n < count; n++) {
		if (seek->seek_upward)
			freq += step;
		else
			freq -= step;

		if (freq > high || freq < low) {
			if (!seek->wrap_around)
				break;
			freq = seek->seek_upward ? low : high;
		}

		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}

		ret = mt6627_probe_channel(fm, freq, &valid);
		if (ret)
			break;
		if (valid) {
			found = freq;
			break;
		}
	}

	ret2 = mt6627_search_end(fm);
	if (!ret)
		ret = ret2;
	if (!ret)
		ret = mtk_fm_set_freq(fm, found ? found : fm->freq);
	if (!ret && !found)
		ret = -ENODATA;
	return ret;
}

static int mtk_fm_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mtk_fm *fm = container_of(ctrl->handler, struct mtk_fm, ctrls);
	int ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_AUDIO_MUTE:
		if (fm->powered)
			ret = mt6627_set_mute(fm, ctrl->val);
		if (!ret)
			fm->muted = ctrl->val;
		break;
	case V4L2_CID_AUDIO_VOLUME:
		if (fm->powered)
			ret = mt6627_set_volume(fm, ctrl->val);
		if (!ret)
			fm->volume = ctrl->val;
		break;
	case V4L2_CID_TUNE_DEEMPHASIS:
		if (fm->powered)
			ret = mt6627_set_deemphasis(fm, ctrl->val ==
						    V4L2_DEEMPHASIS_75_uS);
		if (!ret)
			fm->deemph_75us = ctrl->val == V4L2_DEEMPHASIS_75_uS;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops mtk_fm_ctrl_ops = {
	.s_ctrl = mtk_fm_s_ctrl,
};

static int mtk_fm_open(struct file *file)
{
	struct mtk_fm *fm = video_drvdata(file);
	int ret;

	mutex_lock(&fm->lock);
	ret = v4l2_fh_open(file);
	if (ret)
		goto out;

	if (!fm->powered) {
		ret = mtk_fm_power_on(fm);
		if (ret) {
			v4l2_fh_release(file);
			goto out;
		}
	}
	fm->users++;
out:
	mutex_unlock(&fm->lock);
	return ret;
}

static int mtk_fm_release(struct file *file)
{
	struct mtk_fm *fm = video_drvdata(file);

	mutex_lock(&fm->lock);
	if (!--fm->users && (fm->muted || fm->reset))
		mtk_fm_power_off(fm);
	v4l2_fh_release(file);
	mutex_unlock(&fm->lock);
	return 0;
}

static ssize_t mtk_fm_read(struct file *file, char __user *buf, size_t count,
			   loff_t *ppos)
{
	struct mtk_fm *fm = video_drvdata(file);
	struct v4l2_rds_data rds;
	unsigned long flags;
	size_t done = 0;
	int ret;

	count -= count % sizeof(rds);
	if (!count)
		return 0;

	while (done < count) {
		spin_lock_irqsave(&fm->rds_lock, flags);
		ret = kfifo_get(&fm->rds_fifo, &rds);
		spin_unlock_irqrestore(&fm->rds_lock, flags);

		if (!ret) {
			if (done)
				break;
			if (file->f_flags & O_NONBLOCK)
				return -EWOULDBLOCK;
			ret = wait_event_interruptible(fm->rds_wait,
						       !kfifo_is_empty(&fm->rds_fifo) ||
						       !READ_ONCE(fm->powered));
			if (ret)
				return ret;
			if (!READ_ONCE(fm->powered))
				return -EIO;
			continue;
		}

		if (copy_to_user(buf + done, &rds, sizeof(rds)))
			return done ? done : -EFAULT;
		done += sizeof(rds);
	}

	return done;
}

static __poll_t mtk_fm_poll(struct file *file, struct poll_table_struct *pt)
{
	struct mtk_fm *fm = video_drvdata(file);
	__poll_t req = poll_requested_events(pt);
	__poll_t res = v4l2_ctrl_poll(file, pt);

	if (req & (EPOLLIN | EPOLLRDNORM)) {
		poll_wait(file, &fm->rds_wait, pt);
		if (!kfifo_is_empty(&fm->rds_fifo))
			res |= EPOLLIN | EPOLLRDNORM;
	}
	return res;
}

static int mtk_fm_log_status(struct file *file, void *priv)
{
	struct mtk_fm *fm = video_drvdata(file);

	v4l2_info(&fm->v4l2_dev,
		  "powered %d, core 0x%04x, ROM v%d, patch 0x%04x, %u.%02u MHz\n",
		  fm->powered, fm->hw_id, fm->rom_ver, fm->patch_ver,
		  fm->freq / 100, fm->freq % 100);
	return v4l2_ctrl_log_status(file, priv);
}

static const struct v4l2_file_operations mtk_fm_fops = {
	.owner		= THIS_MODULE,
	.open		= mtk_fm_open,
	.release	= mtk_fm_release,
	.read		= mtk_fm_read,
	.poll		= mtk_fm_poll,
	.unlocked_ioctl	= video_ioctl2,
};

static const struct v4l2_ioctl_ops mtk_fm_ioctl_ops = {
	.vidioc_querycap	= mtk_fm_querycap,
	.vidioc_g_tuner		= mtk_fm_g_tuner,
	.vidioc_s_tuner		= mtk_fm_s_tuner,
	.vidioc_g_frequency	= mtk_fm_g_frequency,
	.vidioc_s_frequency	= mtk_fm_s_frequency,
	.vidioc_s_hw_freq_seek	= mtk_fm_s_hw_freq_seek,
	.vidioc_log_status	= mtk_fm_log_status,
	.vidioc_subscribe_event	= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* --- debugfs: raw register access for bring-up --- */

static ssize_t mtk_fm_dbg_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct mtk_fm *fm = file->private_data;
	char buf[64], op[4];
	u32 addr, val = 0, rd32;
	u16 rd16;
	int n, ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = 0;

	n = sscanf(buf, "%3s %x %x", op, &addr, &val);
	if (n < 2)
		return -EINVAL;

	mutex_lock(&fm->lock);
	if (!fm->powered) {
		ret = -EIO;
		goto out;
	}

	if (!strcmp(op, "r")) {
		ret = mt6627_read(fm, addr, &rd16);
		if (!ret)
			snprintf(fm->dbg_result, sizeof(fm->dbg_result),
				 "fm 0x%02x = 0x%04x\n", addr, rd16);
	} else if (!strcmp(op, "w") && n == 3) {
		ret = mt6627_write(fm, addr, val);
	} else if (!strcmp(op, "tr")) {
		ret = mt6627_top_read(fm, addr, &rd32);
		if (!ret)
			snprintf(fm->dbg_result, sizeof(fm->dbg_result),
				 "top 0x%04x = 0x%08x\n", addr, rd32);
	} else if (!strcmp(op, "tw") && n == 3) {
		ret = mt6627_top_write(fm, addr, val);
	} else if (!strcmp(op, "hr")) {
		ret = mt6627_host_read(fm, addr, &rd32);
		if (!ret)
			snprintf(fm->dbg_result, sizeof(fm->dbg_result),
				 "host 0x%08x = 0x%08x\n", addr, rd32);
	} else if (!strcmp(op, "hw") && n == 3) {
		ret = mt6627_host_write(fm, addr, val);
	} else {
		ret = -EINVAL;
	}
out:
	mutex_unlock(&fm->lock);
	return ret ? ret : count;
}

static ssize_t mtk_fm_dbg_read(struct file *file, char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct mtk_fm *fm = file->private_data;

	return simple_read_from_buffer(ubuf, count, ppos, fm->dbg_result,
				       strlen(fm->dbg_result));
}

static const struct file_operations mtk_fm_dbg_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= mtk_fm_dbg_read,
	.write	= mtk_fm_dbg_write,
	.llseek	= default_llseek,
};

/* --- platform driver --- */

static int mtk_fm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_fm *fm;
	int ret;

	fm = devm_kzalloc(dev, sizeof(*fm), GFP_KERNEL);
	if (!fm)
		return -ENOMEM;

	fm->dev = dev;
	fm->freq = FM_FREQ_DEFAULT;
	fm->volume = FM_VOLUME_MAX;
	mutex_init(&fm->lock);
	spin_lock_init(&fm->rds_lock);
	init_waitqueue_head(&fm->rds_wait);
	INIT_KFIFO(fm->rds_fifo);
	platform_set_drvdata(pdev, fm);

	ret = mtk_fm_link_init(fm);
	if (ret)
		return ret;

	ret = v4l2_device_register(dev, &fm->v4l2_dev);
	if (ret)
		goto err_link;

	v4l2_ctrl_handler_init(&fm->ctrls, 3);
	fm->ctrl_mute = v4l2_ctrl_new_std(&fm->ctrls, &mtk_fm_ctrl_ops,
					  V4L2_CID_AUDIO_MUTE, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&fm->ctrls, &mtk_fm_ctrl_ops, V4L2_CID_AUDIO_VOLUME,
			  0, FM_VOLUME_MAX, 1, FM_VOLUME_MAX);
	v4l2_ctrl_new_std_menu(&fm->ctrls, &mtk_fm_ctrl_ops,
			       V4L2_CID_TUNE_DEEMPHASIS,
			       V4L2_DEEMPHASIS_75_uS,
			       BIT(V4L2_DEEMPHASIS_DISABLED),
			       V4L2_DEEMPHASIS_50_uS);
	if (fm->ctrls.error) {
		ret = fm->ctrls.error;
		goto err_ctrls;
	}
	fm->v4l2_dev.ctrl_handler = &fm->ctrls;

	strscpy(fm->vdev.name, "MT8127 CONSYS FM", sizeof(fm->vdev.name));
	fm->vdev.v4l2_dev = &fm->v4l2_dev;
	fm->vdev.fops = &mtk_fm_fops;
	fm->vdev.ioctl_ops = &mtk_fm_ioctl_ops;
	fm->vdev.release = video_device_release_empty;
	fm->vdev.lock = &fm->lock;
	fm->vdev.device_caps = V4L2_CAP_TUNER | V4L2_CAP_RADIO |
			       V4L2_CAP_HW_FREQ_SEEK | V4L2_CAP_RDS_CAPTURE |
			       V4L2_CAP_READWRITE;
	video_set_drvdata(&fm->vdev, fm);

	WRITE_ONCE(mtk_fm_wmt_dev, fm);
	mtk_wcn_wmt_msgcb_reg(WMTDRV_TYPE_FM, mtk_fm_wmt_msg);

	ret = video_register_device(&fm->vdev, VFL_TYPE_RADIO, -1);
	if (ret)
		goto err_msgcb;

	fm->debugfs = debugfs_create_dir("mtk_consys_fm", NULL);
	debugfs_create_file("reg", 0600, fm->debugfs, fm, &mtk_fm_dbg_fops);

	dev_info(dev, "registered %s\n", video_device_node_name(&fm->vdev));
	return 0;

err_msgcb:
	mtk_wcn_wmt_msgcb_unreg(WMTDRV_TYPE_FM);
	WRITE_ONCE(mtk_fm_wmt_dev, NULL);
err_ctrls:
	v4l2_ctrl_handler_free(&fm->ctrls);
	v4l2_device_unregister(&fm->v4l2_dev);
err_link:
	mtk_fm_link_exit(fm);
	return ret;
}

static void mtk_fm_remove(struct platform_device *pdev)
{
	struct mtk_fm *fm = platform_get_drvdata(pdev);

	debugfs_remove_recursive(fm->debugfs);
	video_unregister_device(&fm->vdev);

	mutex_lock(&fm->lock);
	mtk_fm_power_off(fm);
	mutex_unlock(&fm->lock);

	mtk_wcn_wmt_msgcb_unreg(WMTDRV_TYPE_FM);
	WRITE_ONCE(mtk_fm_wmt_dev, NULL);
	v4l2_ctrl_handler_free(&fm->ctrls);
	v4l2_device_unregister(&fm->v4l2_dev);
	mtk_fm_link_exit(fm);
}

static struct platform_driver mtk_fm_driver = {
	.probe = mtk_fm_probe,
	.remove = mtk_fm_remove,
	.driver = {
		.name = DRIVER_NAME,
	},
};

/*
 * The FM core has no device-tree node of its own: it is a function of the
 * CONSYS that the WMT core already drives, so the module instantiates its
 * device itself.
 */
static int __init mtk_fm_init(void)
{
	int ret;

	ret = platform_driver_register(&mtk_fm_driver);
	if (ret)
		return ret;

	mtk_fm_pdev = platform_device_register_simple(DRIVER_NAME, -1, NULL, 0);
	if (IS_ERR(mtk_fm_pdev)) {
		platform_driver_unregister(&mtk_fm_driver);
		return PTR_ERR(mtk_fm_pdev);
	}
	return 0;
}

static void __exit mtk_fm_exit(void)
{
	platform_device_unregister(mtk_fm_pdev);
	platform_driver_unregister(&mtk_fm_driver);
}

module_init(mtk_fm_init);
module_exit(mtk_fm_exit);

MODULE_DESCRIPTION("FM radio receiver of the MediaTek MT8127 CONSYS");
MODULE_AUTHOR("Marc Lainez <marc.lainez@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("mediatek/mt6627_fm_v1_patch.bin");
MODULE_FIRMWARE("mediatek/mt6627_fm_v1_coeff.bin");
