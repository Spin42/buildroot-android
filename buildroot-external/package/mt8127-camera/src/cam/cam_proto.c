// SPDX-License-Identifier: GPL-2.0
/*
 * Staged capture prototype for the MT8127 camera pipeline.
 *
 * Binds to "mediatek,mt8127-cam-proto" and exposes /sys/kernel/debug/mt8127_cam/ to power the camera, program a
 * sensor, bring up the CSI-2 receiver, configure ISP pass 1 and capture one frame into a DMA buffer, stage by stage.
 * docs/cam-proto.md describes the node and the procedure.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "cam_proto.h"

struct cam_knobs cam_knobs = {
	.irq_mode = 1,
	.port = -1,
	.lanes = 0,
	.csi2_ctrl = -1,
	.csi2_settle = 14,
	.csi2_ecc_order = -1,
	.csi2_vsync = -1,
	.seninf_inten = 0x7f,
	.tg_pad = -1, .tg_src = -1, .tg_fmt = -1,
	.sync_a = -1, .sync_b = -1,
	.grab_x = -1, .grab_y = -1,
	.width = 0, .height = 0,
	.pixid = -1,
	.set_clr_mode = 1,
	.int_en = 0x01a01e31,
	.dma_int_bits = 0x04100000,
	.imgo_con = -1, .imgo_con2 = -1,
	.imgo_pb = 0,
	.imgo_stride_px = 0,
	.imgo_base_mode = 1,
	.fbc_mode = 0,
	.vf_single = 1,
	.vf_delay = -1,
	.done_mask = 0x400,
	.capture_timeout_ms = 2000,
	.frame_fill = 0xa5,
	.mclk_pinmux = 1,
	.io_drive = 0x6000,
	.isp_domain_keep = 0,
	.test_pattern = 0,
	.stream_wait_ms = 200,
	.run_while_error = 1,
};

#define KNOB(n, desc) \
	module_param_named(n, cam_knobs.n, int, 0644); \
	MODULE_PARM_DESC(n, desc)

KNOB(irq_mode, "1: frame done by IRQ, 0: poll INT_STATUS (default 1)");
KNOB(port, "MIPI port: -1 main=CSI0 sub=CSI1, 0 or 1 to force");
KNOB(lanes, "CSI-2 data lanes, 0 = sensor default");
KNOB(csi2_ctrl, "SENINF1_CSI2_CTRL value, -1 = computed");
KNOB(csi2_settle, "CSI2_DELAY settle count (stock 14)");
KNOB(csi2_ecc_order, "CSI2_CTRL bit5, -1 = sensor default");
KNOB(csi2_vsync, "CSI2_CTRL bit13, -1 = sensor default");
KNOB(seninf_inten, "SENINF1_INTEN value (stock 0x7f)");
KNOB(tg_pad, "SENINF1_CTRL pad select, -1 = sensor default");
KNOB(tg_src, "setTg1InputCfg src (8 = MIPI YUV path; 3 for RAW in the stock table), -1 = default");
KNOB(tg_fmt, "TG1_FMT in FMT_SEL (1 RAW10, 3 YUV422), -1 = default");
KNOB(sync_a, "setTg1SensorModeCfg first argument, -1 = default");
KNOB(sync_b, "setTg1SensorModeCfg second argument (SENINF1_CTRL bit9), -1 = default");
KNOB(grab_x, "TG grab start x, -1 = sensor default");
KNOB(grab_y, "TG grab start y, -1 = sensor default");
KNOB(width, "frame width override, 0 = sensor default");
KNOB(height, "frame height override, 0 = sensor default");
KNOB(pixid, "Bayer start colour for PIX_ID, -1 = sensor default");
KNOB(set_clr_mode, "1: SET/CLR helper registers, 0: read-modify-write the base registers");
KNOB(int_en, "ISP INT_EN value written for the capture");
KNOB(dma_int_bits, "bits OR-ed into ISP+0x028 in stage 3 (0 = none)");
KNOB(imgo_con, "IMGO_CON, -1 = 0x08100850");
KNOB(imgo_con2, "IMGO_CON2, -1 = 0x00100800");
KNOB(imgo_pb, "IMGO packing code (bytes per 4 pixels), 0 = 5 for RAW10, 8 for YUV422");
KNOB(imgo_stride_px, "IMGO stride in pixels, 0 = tight");
KNOB(imgo_base_mode, "IMGO_BASE_ADDR: bit0 before VF start, bit1 rewrite at SOF1, bit2 rewrite at PASS1_DON");
KNOB(fbc_mode, "0: FBC left off, 1: vendor FBC init with one buffer");
KNOB(vf_single, "1: single-shot TG, 0: continuous until stopped");
KNOB(vf_delay, "TG_VF_CON delay field, -1 = sensor default");
KNOB(done_mask, "INT_STATUS bits that complete a capture (0x400 = PASS1_TG1_DON)");
KNOB(capture_timeout_ms, "frame wait timeout");
KNOB(frame_fill, "byte pattern the buffer holds before a capture");
KNOB(mclk_pinmux, "1: set GPIO119 to the CMMCLK function in stage 1");
KNOB(io_drive, "GPIO_DRV_117_120 field value (0x6000 = 8 mA), -1 = leave");
KNOB(isp_domain_keep, "1: keep the ISP power domain on at stage 0");
KNOB(test_pattern, "1: SP2508 colour bar output");
KNOB(stream_wait_ms, "wait after the sensor starts, before the stage 2 report");
KNOB(run_while_error, "1: continue stage 2 even if the PHY calibration reports not ready");

static const struct {
	const char *name;
	resource_size_t min;
	bool optional;
} cam_wins[CAM_NUM_WIN] = {
	[CAM_WIN_IMGSYS]   = { "imgsys",   0x00c },
	[CAM_WIN_ISP]      = { "isp",      0x1604 },
	[CAM_WIN_SENINF]   = { "seninf",   0x624 },
	[CAM_WIN_MIPI_RX]  = { "mipi-rx",  0x04c },
	[CAM_WIN_MIPI_ANA] = { "mipi-ana", 0x054 },
	[CAM_WIN_SPM]      = { "spm",      0x614 },
	[CAM_WIN_TOPCKGEN] = { "topckgen", 0x054 },
	[CAM_WIN_GPIO]     = { "gpio",     0xb64 },
	[CAM_WIN_EMI]      = { "emi",      0x124, true },
};

static void cam_status_text(struct cam_proto *c, char *buf, size_t sz, size_t *np)
{
	SNPR(buf, sz, *np, "stage %d, sensor %s, flags: domain_ours %d clk %d mclk %d vcama %d vcamio %d seninf %d isp_cfg %d irq %d\n",
	     c->stage, c->sensor ? c->sensor->name : "none", c->domain_ours, c->clk_on, c->mclk_on, c->vcama_on,
	     c->vcamio_on, c->seninf_on, c->isp_cfg, c->irq_req);
	if (!c->clk_on) {
		SNPR(buf, sz, *np, "clocks off: registers not read\n");
		return;
	}
	SNPR(buf, sz, *np, "[power] IMG_CG %#x (gates open when 0x1e1 bits clear) SPM PWR_CON %#x STATUS %#x/%#x CLK_CFG_1 %#x\n",
	     cam_rd(c, CAM_WIN_IMGSYS, IMG_CG_CON), cam_rd(c, CAM_WIN_SPM, SPM_ISP_PWR_CON),
	     cam_rd(c, CAM_WIN_SPM, SPM_PWR_STATUS), cam_rd(c, CAM_WIN_SPM, SPM_PWR_STATUS_S),
	     cam_rd(c, CAM_WIN_TOPCKGEN, TOP_CLK_CFG_1));
	cam_seninf_dump(c, buf, sz, np);
	cam_isp_dump(c, buf, sz, np);
	if (c->frame_cpu)
		SNPR(buf, sz, *np, "[frame] %ux%u stride %u bytes, size %zu, valid %d, last capture ret %d\n  %s\n",
		     c->frame_w, c->frame_h, c->frame_stride, c->frame_size, c->frame_valid, c->capture_ret,
		     c->frame_stats);
}

static void cam_snapshot(struct cam_proto *c)
{
	size_t n = 0;

	c->snap[0] = 0;
	SNPR(c->snap, CAM_STATUS_BUF, n, "# post-mortem snapshot taken when stage %d failed\n", c->stage + 1);
	cam_status_text(c, c->snap, CAM_STATUS_BUF, &n);
}

static void cam_teardown(struct cam_proto *c)
{
	cam_isp_quiesce(c);
	if (c->irq_req) {
		free_irq(c->irq, c);
		c->irq_req = false;
	}
	cam_seninf_off(c);
	cam_power_off(c);
	c->stage = 0;
	dev_info(c->dev, "stage 0: everything powered down\n");
}

static int stage1(struct cam_proto *c)
{
	int ret = cam_power_on(c);

	return ret ?: cam_sensor_identify(c);
}

static int stage2(struct cam_proto *c)
{
	int ret = cam_seninf_phy_on(c);

	if (ret)
		return ret;
	dev_info(c->dev, "stage 2: programming %s\n", c->sensor->name);
	ret = cam_sensor_program(c);
	if (ret)
		return ret;
	ret = cam_seninf_route(c);
	if (ret)
		return ret;
	msleep(cam_knobs.stream_wait_ms);
	cam_seninf_report(c);
	return 0;
}

static int stage3(struct cam_proto *c)
{
	return cam_isp_configure(c);
}

static int stage4(struct cam_proto *c)
{
	int ret = cam_isp_prepare_frame(c);

	return ret ?: cam_isp_capture(c);
}

static int (*const cam_stages[])(struct cam_proto *) = { NULL, stage1, stage2, stage3, stage4 };

static int cam_run_stage(struct cam_proto *c, int target)
{
	int ret;

	if (target == 0) {
		cam_teardown(c);
		return 0;
	}
	if (!c->sensor) {
		dev_err(c->dev, "select a sensor first: echo sp2508|sp0a20 > sensor\n");
		return -EINVAL;
	}
	if (target <= c->stage)
		cam_teardown(c);

	while (c->stage < target) {
		int next = c->stage + 1;

		ret = cam_stages[next](c);
		if (ret) {
			dev_err(c->dev, "stage %d failed: %d, powering down\n", next, ret);
			cam_snapshot(c);
			cam_teardown(c);
			return ret;
		}
		c->stage = next;
		dev_info(c->dev, "stage %d complete\n", next);
	}
	return 0;
}

/* debugfs */

static ssize_t stage_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct cam_proto *c = file_inode(f)->i_private;
	int val, ret;

	ret = kstrtoint_from_user(ubuf, len, 0, &val);
	if (ret)
		return ret;
	if (val < 0 || val > 4)
		return -EINVAL;
	mutex_lock(&c->lock);
	ret = cam_run_stage(c, val);
	mutex_unlock(&c->lock);
	return ret ?: len;
}

static ssize_t stage_read(struct file *f, char __user *ubuf, size_t len, loff_t *ppos)
{
	struct cam_proto *c = file_inode(f)->i_private;
	char buf[8];
	int n;

	mutex_lock(&c->lock);
	n = scnprintf(buf, sizeof(buf), "%d\n", c->stage);
	mutex_unlock(&c->lock);
	return simple_read_from_buffer(ubuf, len, ppos, buf, n);
}

static const struct file_operations stage_fops = {
	.owner = THIS_MODULE, .open = simple_open, .read = stage_read, .write = stage_write, .llseek = default_llseek,
};

static ssize_t sensor_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct cam_proto *c = file_inode(f)->i_private;
	const struct cam_sensor *s;
	char buf[16];

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	s = cam_sensor_by_name(strim(buf));
	if (!s)
		return -EINVAL;
	mutex_lock(&c->lock);
	if (c->stage) {
		mutex_unlock(&c->lock);
		return -EBUSY;
	}
	c->sensor = s;
	mutex_unlock(&c->lock);
	return len;
}

static ssize_t sensor_read(struct file *f, char __user *ubuf, size_t len, loff_t *ppos)
{
	struct cam_proto *c = file_inode(f)->i_private;
	char buf[32];
	int n = scnprintf(buf, sizeof(buf), "%s\n", c->sensor ? c->sensor->name : "none");

	return simple_read_from_buffer(ubuf, len, ppos, buf, n);
}

static const struct file_operations sensor_fops = {
	.owner = THIS_MODULE, .open = simple_open, .read = sensor_read, .write = sensor_write, .llseek = default_llseek,
};

static int status_show(struct seq_file *m, void *unused)
{
	struct cam_proto *c = m->private;
	char *buf = kzalloc(CAM_STATUS_BUF, GFP_KERNEL);
	size_t n = 0;

	if (!buf)
		return -ENOMEM;
	mutex_lock(&c->lock);
	if (c->stage || c->clk_on) {
		cam_status_text(c, buf, CAM_STATUS_BUF, &n);
	} else if (c->snap[0]) {
		SNPR(buf, CAM_STATUS_BUF, n, "%s", c->snap);
	} else {
		SNPR(buf, CAM_STATUS_BUF, n, "powered down (stage 0)\n");
	}
	mutex_unlock(&c->lock);
	seq_puts(m, buf);
	kfree(buf);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static ssize_t frame_read(struct file *f, char __user *ubuf, size_t len, loff_t *ppos)
{
	struct cam_proto *c = file_inode(f)->i_private;
	ssize_t ret;

	mutex_lock(&c->lock);
	if (!c->frame_cpu || !c->frame_size)
		ret = 0;
	else
		ret = simple_read_from_buffer(ubuf, len, ppos, c->frame_cpu, c->frame_size);
	mutex_unlock(&c->lock);
	return ret;
}

static const struct file_operations frame_fops = {
	.owner = THIS_MODULE, .open = simple_open, .read = frame_read, .llseek = default_llseek,
};

static int frame_info_show(struct seq_file *m, void *unused)
{
	struct cam_proto *c = m->private;

	mutex_lock(&c->lock);
	seq_printf(m, "valid=%d\nformat=%s\nwidth=%u\nheight=%u\nstride_bytes=%u\nsize=%zu\ndma=%pad\n%s\n",
		   c->frame_valid, c->frame_fmt ?: "none", c->frame_w, c->frame_h, c->frame_stride, c->frame_size,
		   &c->frame_dma, c->frame_stats);
	mutex_unlock(&c->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(frame_info);

static ssize_t capture_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct cam_proto *c = file_inode(f)->i_private;
	int val, ret;

	ret = kstrtoint_from_user(ubuf, len, 0, &val);
	if (ret)
		return ret;
	if (val != 1)
		return -EINVAL;
	mutex_lock(&c->lock);
	if (c->stage != 4) {
		dev_err(c->dev, "capture needs stage 4 (current %d)\n", c->stage);
		ret = -EINVAL;
	} else {
		ret = cam_isp_capture(c);
	}
	mutex_unlock(&c->lock);
	return ret ?: len;
}

static const struct file_operations capture_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = capture_write, .llseek = noop_llseek,
};

static ssize_t path_read(struct file *f, char __user *ubuf, size_t len, loff_t *ppos)
{
	struct cam_proto *c = file_inode(f)->i_private;
	char *buf = kzalloc(CAM_STATUS_BUF, GFP_KERNEL);
	size_t n = 0;
	ssize_t ret;

	if (!buf)
		return -ENOMEM;
	mutex_lock(&c->lock);
	cam_isp_path_show(c, buf, CAM_STATUS_BUF, &n);
	mutex_unlock(&c->lock);
	ret = simple_read_from_buffer(ubuf, len, ppos, buf, n);
	kfree(buf);
	return ret;
}

static ssize_t path_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct cam_proto *c = file_inode(f)->i_private;
	char buf[48], name[24];
	unsigned int val;
	size_t l;
	bool is_set = false;
	int ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	if (sscanf(buf, "%23s %i", name, &val) != 2)
		return -EINVAL;
	l = strlen(name);
	if (l < 5)
		return -EINVAL;
	if (!strcmp(name + l - 4, "_SET"))
		is_set = true;
	else if (strcmp(name + l - 4, "_CLR"))
		return -EINVAL;
	name[l - 4] = 0;
	mutex_lock(&c->lock);
	if (!c->sensor) {
		mutex_unlock(&c->lock);
		return -EINVAL;
	}
	ret = cam_isp_path_write(c, name, is_set, val);
	mutex_unlock(&c->lock);
	return ret ?: len;
}

static const struct file_operations path_fops = {
	.owner = THIS_MODULE, .open = simple_open, .read = path_read, .write = path_write, .llseek = default_llseek,
};

static ssize_t reg_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct cam_proto *c = file_inode(f)->i_private;
	char buf[64], win[16];
	unsigned int off, val = 0;
	int n, i, ret = -EINVAL;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	n = sscanf(buf, "%15s %i %i", win, &off, &val);
	if (n < 2)
		return -EINVAL;
	mutex_lock(&c->lock);
	if (!c->clk_on) {
		mutex_unlock(&c->lock);
		return -ENODEV;
	}
	for (i = 0; i < CAM_NUM_WIN; i++) {
		if (strcmp(win, cam_wins[i].name))
			continue;
		if (!c->base[i] || (off & 3) || off + 4 > c->size[i])
			break;
		if (n == 3) {
			cam_wr(c, i, off, val);
			snprintf(c->reg_result, sizeof(c->reg_result), "%s+%#x <- %#x\n", win, off, val);
		} else {
			snprintf(c->reg_result, sizeof(c->reg_result), "%s+%#x = %#x\n", win, off, cam_rd(c, i, off));
		}
		dev_info(c->dev, "%s", c->reg_result);
		ret = 0;
		break;
	}
	mutex_unlock(&c->lock);
	return ret ?: len;
}

static ssize_t reg_read(struct file *f, char __user *ubuf, size_t len, loff_t *ppos)
{
	struct cam_proto *c = file_inode(f)->i_private;

	return simple_read_from_buffer(ubuf, len, ppos, c->reg_result, strlen(c->reg_result));
}

static const struct file_operations reg_fops = {
	.owner = THIS_MODULE, .open = simple_open, .read = reg_read, .write = reg_write, .llseek = default_llseek,
};

static int cam_map_windows(struct cam_proto *c, struct device_node *np)
{
	int i;

	for (i = 0; i < CAM_NUM_WIN; i++) {
		struct resource res;
		int idx = of_property_match_string(np, "reg-names", cam_wins[i].name);

		if (idx < 0) {
			if (cam_wins[i].optional)
				continue;
			dev_err(c->dev, "missing reg-names entry \"%s\"\n", cam_wins[i].name);
			return -EINVAL;
		}
		if (of_address_to_resource(np, idx, &res))
			return -EINVAL;
		if (resource_size(&res) < cam_wins[i].min) {
			dev_err(c->dev, "window \"%s\" is %pa bytes, need at least %pa\n", cam_wins[i].name,
				&(resource_size_t){ resource_size(&res) }, &cam_wins[i].min);
			return -EINVAL;
		}
		c->base[i] = devm_ioremap(c->dev, res.start, resource_size(&res));
		if (!c->base[i])
			return -ENOMEM;
		c->size[i] = resource_size(&res);
	}
	return 0;
}

static void cam_put_adapter(void *data)
{
	i2c_put_adapter(data);
}

static int cam_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node, *i2c_np;
	struct cam_proto *c;
	int ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = dev;
	mutex_init(&c->lock);
	init_completion(&c->frame_done);
	atomic_set(&c->int_acc, 0);
	atomic_set(&c->dma_acc, 0);
	atomic_set(&c->irq_count, 0);
	c->snap = devm_kzalloc(dev, CAM_STATUS_BUF, GFP_KERNEL);
	if (!c->snap)
		return -ENOMEM;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ret = cam_map_windows(c, np);
	if (ret)
		return ret;
	c->irq = platform_get_irq(pdev, 0);
	if (c->irq < 0)
		return c->irq;

	c->vcama = devm_regulator_get(dev, "vcama");
	if (IS_ERR(c->vcama))
		return dev_err_probe(dev, PTR_ERR(c->vcama), "vcama\n");
	c->vcamio = devm_regulator_get(dev, "vcamio");
	if (IS_ERR(c->vcamio))
		return dev_err_probe(dev, PTR_ERR(c->vcamio), "vcamio\n");

	c->main_rst = devm_gpiod_get_optional(dev, "main-reset", GPIOD_OUT_LOW);
	if (IS_ERR(c->main_rst))
		return PTR_ERR(c->main_rst);
	c->main_pdn = devm_gpiod_get_optional(dev, "main-powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(c->main_pdn))
		return PTR_ERR(c->main_pdn);
	c->sub_rst = devm_gpiod_get_optional(dev, "sub-reset", GPIOD_OUT_LOW);
	if (IS_ERR(c->sub_rst))
		return PTR_ERR(c->sub_rst);
	c->sub_pdn = devm_gpiod_get_optional(dev, "sub-powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(c->sub_pdn))
		return PTR_ERR(c->sub_pdn);

	i2c_np = of_parse_phandle(np, "i2c-bus", 0);
	if (!i2c_np)
		return dev_err_probe(dev, -EINVAL, "missing i2c-bus phandle\n");
	c->adap = of_find_i2c_adapter_by_node(i2c_np);
	of_node_put(i2c_np);
	if (!c->adap)
		return -EPROBE_DEFER;
	ret = devm_add_action_or_reset(dev, cam_put_adapter, c->adap);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, c);
	c->dbg = debugfs_create_dir("mt8127_cam", NULL);
	debugfs_create_file("stage", 0644, c->dbg, c, &stage_fops);
	debugfs_create_file("sensor", 0644, c->dbg, c, &sensor_fops);
	debugfs_create_file("status", 0444, c->dbg, c, &status_fops);
	debugfs_create_file("frame", 0444, c->dbg, c, &frame_fops);
	debugfs_create_file("frame_info", 0444, c->dbg, c, &frame_info_fops);
	debugfs_create_file("capture", 0200, c->dbg, c, &capture_fops);
	debugfs_create_file("path_regs", 0644, c->dbg, c, &path_fops);
	debugfs_create_file("reg", 0644, c->dbg, c, &reg_fops);

	dev_info(dev, "ready: echo sp2508|sp0a20 > sensor; echo 1..4 > stage (see docs/cam-proto.md)\n");
	return 0;
}

static void cam_remove(struct platform_device *pdev)
{
	struct cam_proto *c = platform_get_drvdata(pdev);

	debugfs_remove_recursive(c->dbg);
	mutex_lock(&c->lock);
	cam_teardown(c);
	cam_isp_free_frame(c);
	mutex_unlock(&c->lock);
}

static const struct of_device_id cam_of_match[] = {
	{ .compatible = "mediatek,mt8127-cam-proto" },
	{ }
};
MODULE_DEVICE_TABLE(of, cam_of_match);

static struct platform_driver cam_driver = {
	.probe = cam_probe,
	.remove = cam_remove,
	.driver = {
		.name = "mtk-cam-proto",
		.of_match_table = cam_of_match,
	},
};
module_platform_driver(cam_driver);

MODULE_DESCRIPTION("MT8127 camera pipeline staged capture prototype");
MODULE_LICENSE("GPL");
