/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT8127 camera subsystem: shared state of the V4L2 capture driver.
 *
 * Register values follow docs/seninf-mipi.md and docs/isp-capture.md; docs/v4l2-driver.md describes the driver.
 */
#ifndef MTK_CAMSYS_H
#define MTK_CAMSYS_H

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-v4l2.h>

#include "cam_regs.h"

/* MediaTek ISP packed Bayer, for kernels whose headers predate the format. */
#ifndef V4L2_PIX_FMT_MTISP_SBGGR10
#define V4L2_PIX_FMT_MTISP_SBGGR10	v4l2_fourcc('M', 'B', 'B', 'A')
#endif

#define MTK_CAMSYS_NAME		"mtk-camsys"

enum mtk_camsys_port_id {
	MTK_CAMSYS_PORT_REAR,	/* CSI0, raw sensor */
	MTK_CAMSYS_PORT_FRONT,	/* CSI1, YUV sensor */
	MTK_CAMSYS_NUM_PORTS
};

/* The CSI-2 receiver has one sink pad per port and a single source pad towards the ISP. */
#define MTK_CSI2_PAD_SRC	MTK_CAMSYS_NUM_PORTS
#define MTK_CSI2_NUM_PADS	(MTK_CAMSYS_NUM_PORTS + 1)

/* Fixed description of what the hardware receives on a port. */
struct mtk_camsys_port_cfg {
	u32 mbus_code;
	u32 pixelformat;
	u16 width, height;		/* captured frame: the TG grab window */
	u16 sensor_width, sensor_height;	/* frame the sensor sends */
	bool yuv;
	u8 csi_port;		/* MIPI RX block the sensor is wired to */
	u8 tg_pad, tg_src, tg_fmt;
	u8 pixid;		/* Bayer start colour for the ISP PIX_ID */
	u8 vf_delay;
	u8 ecc_order;
	u8 vsync_pol;
	u8 sync_a, sync_b;
	u8 grab_x, grab_y;	/* TG grab start */
};

struct mtk_camsys_port {
	const struct mtk_camsys_port_cfg *cfg;
	bool present;		/* described in the device tree */
	u8 lanes;
	struct v4l2_subdev *sensor;
};

struct mtk_camsys_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct mtk_camsys {
	struct device *dev;
	void __iomem *imgsys, *isp, *seninf, *mipi_rx, *mipi_ana, *spm, *topckgen, *gpio;
	int irq;

	/* Camera power and the sensor master clock; tg_lock serialises the TG phase counter register. */
	spinlock_t tg_lock;
	struct mutex power_lock;
	unsigned int power_count;
	bool domain_ours;
	bool mclk_on;
	struct clk_hw mclk_hw;

	struct media_device mdev;
	struct v4l2_device v4l2_dev;
	struct v4l2_async_notifier notifier;
	struct mtk_camsys_port ports[MTK_CAMSYS_NUM_PORTS];

	/* CSI-2 receiver subdev */
	struct v4l2_subdev csi2;
	struct media_pad csi2_pads[MTK_CSI2_NUM_PADS];
	int active_port;	/* sink pad with an enabled link, -1 when none */

	/* capture video node */
	struct video_device vdev;
	struct media_pad vdev_pad;
	struct media_pipeline pipe;
	struct vb2_queue queue;
	struct mutex video_lock;
	struct v4l2_pix_format fmt;

	/* buffer rotation, protected by qlock (taken in the interrupt handler) */
	spinlock_t qlock;
	struct list_head queued;
	struct mtk_camsys_buf *cur;
	bool streaming;
	u32 sequence;
	u32 drops;
	u32 hw_errors;
	u64 sof_ns;
	u32 err_bits;

	/* interrupt statistics for debugfs */
	u32 irq_bits[32];
	u32 dma_irqs;
	u32 last_st, last_dma;
	struct dentry *debugfs;
};

extern const struct mtk_camsys_port_cfg mtk_camsys_port_cfgs[MTK_CAMSYS_NUM_PORTS];

static inline void mtk_camsys_rmw(void __iomem *base, u32 off, u32 clr, u32 set)
{
	writel((readl(base + off) & ~clr) | set, base + off);
}

/* mtk_camsys_clk.c: power, clocks, sensor master clock */
int mtk_camsys_clk_register(struct mtk_camsys *cam);
int mtk_camsys_power_get(struct mtk_camsys *cam);
void mtk_camsys_power_put(struct mtk_camsys *cam);
void mtk_camsys_tg_clock_restore(struct mtk_camsys *cam);

/* mtk_camsys_seninf.c: CSI-2 receiver subdev and MIPI RX programming */
int mtk_camsys_csi2_register(struct mtk_camsys *cam);
void mtk_camsys_csi2_unregister(struct mtk_camsys *cam);
void mtk_camsys_csi2_set_active(struct mtk_camsys *cam, int port);

/* mtk_camsys_isp.c: ISP pass 1 and the IMGO DMA */
void mtk_camsys_isp_start(struct mtk_camsys *cam, const struct mtk_camsys_port_cfg *cfg, u32 bytesperline,
			  dma_addr_t dma);
void mtk_camsys_isp_stop(struct mtk_camsys *cam);
void mtk_camsys_isp_set_base(struct mtk_camsys *cam, dma_addr_t dma);
irqreturn_t mtk_camsys_isp_irq(int irq, void *data);

/* mtk_camsys_video.c: capture node and buffer rotation */
int mtk_camsys_video_register(struct mtk_camsys *cam);
void mtk_camsys_video_unregister(struct mtk_camsys *cam);
void mtk_camsys_video_set_default_format(struct mtk_camsys *cam);
void mtk_camsys_video_frame_done(struct mtk_camsys *cam, u32 status);
u32 mtk_camsys_bytesperline(const struct mtk_camsys_port_cfg *cfg);

#endif
