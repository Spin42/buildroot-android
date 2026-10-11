// SPDX-License-Identifier: GPL-2.0
/*
 * Capture video node: vb2-dma-contig queue, format handling and the buffer rotation done at the frame-done
 * interrupt. The ISP writes the frame into the current buffer; when the frame ends the next queued buffer is
 * programmed and the finished one is returned. With nothing queued the ISP writes into a scratch buffer, so every
 * queued buffer completes, and the frames written there are counted as drops.
 */
#include <linux/dma-mapping.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "mtk_camsys.h"

#define MTK_CAMSYS_MIN_BUFFERS	3

u32 mtk_camsys_bytesperline(const struct mtk_camsys_port_cfg *cfg)
{
	/* MediaTek RAW10 packs 4 pixels in 5 bytes and the pitch is a multiple of 8 pixels; YUV 4:2:2 takes 2 bytes. */
	return cfg->yuv ? cfg->width * 2 : roundup(cfg->width, 8) * 5 / 4;
}

static const struct mtk_camsys_port_cfg *cfg_by_pixelformat(u32 pixelformat)
{
	unsigned int i;

	for (i = 0; i < MTK_CAMSYS_NUM_PORTS; i++)
		if (mtk_camsys_port_cfgs[i].pixelformat == pixelformat)
			return &mtk_camsys_port_cfgs[i];
	return NULL;
}

static const struct mtk_camsys_port_cfg *cfg_by_mbus_code(u32 code)
{
	unsigned int i;

	for (i = 0; i < MTK_CAMSYS_NUM_PORTS; i++)
		if (mtk_camsys_port_cfgs[i].mbus_code == code)
			return &mtk_camsys_port_cfgs[i];
	return NULL;
}

static void fill_pix_format(struct v4l2_pix_format *pix, const struct mtk_camsys_port_cfg *cfg)
{
	pix->width = cfg->width;
	pix->height = cfg->height;
	pix->pixelformat = cfg->pixelformat;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = mtk_camsys_bytesperline(cfg);
	pix->sizeimage = pix->bytesperline * cfg->height;
	if (cfg->yuv) {
		pix->colorspace = V4L2_COLORSPACE_SRGB;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
		pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
		pix->xfer_func = V4L2_XFER_FUNC_SRGB;
	} else {
		pix->colorspace = V4L2_COLORSPACE_RAW;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
		pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
		pix->xfer_func = V4L2_XFER_FUNC_NONE;
	}
}

void mtk_camsys_video_set_default_format(struct mtk_camsys *cam)
{
	unsigned int port = cam->active_port < 0 ? MTK_CAMSYS_PORT_REAR : cam->active_port;

	fill_pix_format(&cam->fmt, &mtk_camsys_port_cfgs[port]);
}

/* Buffer queue */

static void return_buffers(struct mtk_camsys *cam, enum vb2_buffer_state state)
{
	struct mtk_camsys_buf *buf, *tmp;
	unsigned long flags;
	LIST_HEAD(list);

	spin_lock_irqsave(&cam->qlock, flags);
	if (cam->cur) {
		list_add(&cam->cur->list, &list);
		cam->cur = NULL;
	}
	list_splice_tail_init(&cam->queued, &list);
	spin_unlock_irqrestore(&cam->qlock, flags);

	list_for_each_entry_safe(buf, tmp, &list, list)
		vb2_buffer_done(&buf->vb.vb2_buf, state);
}

static int queue_setup(struct vb2_queue *q, unsigned int *nbuffers, unsigned int *nplanes, unsigned int sizes[],
		       struct device *alloc_devs[])
{
	struct mtk_camsys *cam = vb2_get_drv_priv(q);

	if (*nplanes)
		return sizes[0] < cam->fmt.sizeimage ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = cam->fmt.sizeimage;
	return 0;
}

static int buf_prepare(struct vb2_buffer *vb)
{
	struct mtk_camsys *cam = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < cam->fmt.sizeimage)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, cam->fmt.sizeimage);
	return 0;
}

static void buf_queue(struct vb2_buffer *vb)
{
	struct mtk_camsys *cam = vb2_get_drv_priv(vb->vb2_queue);
	struct mtk_camsys_buf *buf = container_of(to_vb2_v4l2_buffer(vb), struct mtk_camsys_buf, vb);
	unsigned long flags;

	spin_lock_irqsave(&cam->qlock, flags);
	list_add_tail(&buf->list, &cam->queued);
	spin_unlock_irqrestore(&cam->qlock, flags);
}

static int start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct mtk_camsys *cam = vb2_get_drv_priv(q);
	const struct mtk_camsys_port_cfg *cfg;
	struct mtk_camsys_buf *buf;
	unsigned long flags;
	int ret;

	if (cam->active_port < 0) {
		ret = -ENOLINK;
		goto err_buffers;
	}
	cfg = &mtk_camsys_port_cfgs[cam->active_port];

	cam->scratch_size = cam->fmt.sizeimage;
	cam->scratch = dma_alloc_coherent(cam->dev, cam->scratch_size, &cam->scratch_dma, GFP_KERNEL);
	if (!cam->scratch) {
		ret = -ENOMEM;
		goto err_buffers;
	}

	ret = mtk_camsys_power_get(cam);
	if (ret)
		goto err_scratch;

	ret = video_device_pipeline_start(&cam->vdev, &cam->pipe);
	if (ret)
		goto err_pm;

	ret = v4l2_subdev_enable_streams(&cam->csi2, MTK_CSI2_PAD_SRC, BIT_ULL(0));
	if (ret)
		goto err_pipe;

	spin_lock_irqsave(&cam->qlock, flags);
	buf = list_first_entry(&cam->queued, struct mtk_camsys_buf, list);
	list_del(&buf->list);
	cam->cur = buf;
	cam->sequence = 0;
	cam->drops = 0;
	cam->hw_errors = 0;
	cam->err_bits = 0;
	cam->sof_ns = 0;
	cam->streaming = true;
	spin_unlock_irqrestore(&cam->qlock, flags);

	mtk_camsys_isp_start(cam, cfg, cam->fmt.bytesperline, vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0));
	return 0;

err_pipe:
	video_device_pipeline_stop(&cam->vdev);
err_pm:
	mtk_camsys_power_put(cam);
err_scratch:
	dma_free_coherent(cam->dev, cam->scratch_size, cam->scratch, cam->scratch_dma);
	cam->scratch = NULL;
err_buffers:
	return_buffers(cam, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void stop_streaming(struct vb2_queue *q)
{
	struct mtk_camsys *cam = vb2_get_drv_priv(q);
	unsigned long flags;

	spin_lock_irqsave(&cam->qlock, flags);
	cam->streaming = false;
	spin_unlock_irqrestore(&cam->qlock, flags);

	mtk_camsys_isp_stop(cam);
	v4l2_subdev_disable_streams(&cam->csi2, MTK_CSI2_PAD_SRC, BIT_ULL(0));
	video_device_pipeline_stop(&cam->vdev);
	mtk_camsys_power_put(cam);

	dev_dbg(cam->dev, "stopped after %u frames, %u dropped, %u hardware errors\n", cam->sequence, cam->drops,
		cam->hw_errors);
	return_buffers(cam, VB2_BUF_STATE_ERROR);
	dma_free_coherent(cam->dev, cam->scratch_size, cam->scratch, cam->scratch_dma);
	cam->scratch = NULL;
}

static const struct vb2_ops queue_ops = {
	.queue_setup = queue_setup,
	.buf_prepare = buf_prepare,
	.buf_queue = buf_queue,
	.start_streaming = start_streaming,
	.stop_streaming = stop_streaming,
};

/*
 * Called from the interrupt handler when the TG finished a frame. The ISP restarts into the buffer programmed
 * here during the blanking interval.
 */
void mtk_camsys_video_frame_done(struct mtk_camsys *cam, u32 status)
{
	struct mtk_camsys_buf *done, *next = NULL;
	enum vb2_buffer_state state;
	u32 seq;

	spin_lock(&cam->qlock);
	if (!cam->streaming)
		goto out;

	seq = cam->sequence++;
	done = cam->cur;
	if (!list_empty(&cam->queued)) {
		next = list_first_entry(&cam->queued, struct mtk_camsys_buf, list);
		list_del(&next->list);
	}
	cam->cur = next;
	mtk_camsys_isp_set_base(cam, next ? vb2_dma_contig_plane_dma_addr(&next->vb.vb2_buf, 0) :
					    cam->scratch_dma);

	if (!done) {
		/* The frame went to the scratch buffer. */
		cam->drops++;
		cam->err_bits = 0;
		goto out;
	}

	state = VB2_BUF_STATE_DONE;
	if (cam->err_bits) {
		cam->hw_errors++;
		state = VB2_BUF_STATE_ERROR;
		cam->err_bits = 0;
	}
	done->vb.vb2_buf.timestamp = cam->sof_ns ?: ktime_get_ns();
	done->vb.sequence = seq;
	done->vb.field = V4L2_FIELD_NONE;
	vb2_buffer_done(&done->vb.vb2_buf, state);
out:
	spin_unlock(&cam->qlock);
}

/* ioctls */

static int vidioc_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct mtk_camsys *cam = video_drvdata(file);

	strscpy(cap->driver, MTK_CAMSYS_NAME, sizeof(cap->driver));
	strscpy(cap->card, "MediaTek MT8127 camera", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", dev_name(cam->dev));
	return 0;
}

/* With a media bus code, only the pixel format produced from that code is listed. */
static int vidioc_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	const struct mtk_camsys_port_cfg *cfg;

	if (f->mbus_code) {
		cfg = cfg_by_mbus_code(f->mbus_code);
		if (!cfg || f->index)
			return -EINVAL;
	} else {
		if (f->index >= MTK_CAMSYS_NUM_PORTS)
			return -EINVAL;
		cfg = &mtk_camsys_port_cfgs[f->index];
	}
	f->pixelformat = cfg->pixelformat;
	return 0;
}

static int vidioc_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct mtk_camsys *cam = video_drvdata(file);

	f->fmt.pix = cam->fmt;
	return 0;
}

/* The size follows from the pixel format: each format is produced by one fixed sensor mode. */
static int vidioc_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct mtk_camsys *cam = video_drvdata(file);
	const struct mtk_camsys_port_cfg *cfg = cfg_by_pixelformat(f->fmt.pix.pixelformat);

	if (!cfg)
		cfg = cfg_by_pixelformat(cam->fmt.pixelformat);
	fill_pix_format(&f->fmt.pix, cfg);
	return 0;
}

static int vidioc_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct mtk_camsys *cam = video_drvdata(file);

	if (vb2_is_busy(&cam->queue)) {
		dev_warn(cam->dev, "format change refused while buffers are allocated\n");
		return -EBUSY;
	}
	vidioc_try_fmt(file, priv, f);
	cam->fmt = f->fmt.pix;
	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *fsize)
{
	const struct mtk_camsys_port_cfg *cfg = cfg_by_pixelformat(fsize->pixel_format);

	if (!cfg || fsize->index)
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = cfg->width;
	fsize->discrete.height = cfg->height;
	return 0;
}

static const struct v4l2_ioctl_ops ioctl_ops = {
	.vidioc_querycap = vidioc_querycap,
	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt,
	.vidioc_g_fmt_vid_cap = vidioc_g_fmt,
	.vidioc_s_fmt_vid_cap = vidioc_s_fmt,
	.vidioc_try_fmt_vid_cap = vidioc_try_fmt,
	.vidioc_enum_framesizes = vidioc_enum_framesizes,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations video_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/* The format the user selected has to be what the active sensor delivers. */
static int video_link_validate(struct media_link *link)
{
	struct video_device *vdev = media_entity_to_video_device(link->sink->entity);
	struct mtk_camsys *cam = video_get_drvdata(vdev);
	struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(link->source->entity);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = link->source->index,
	};
	const struct mtk_camsys_port_cfg *cfg;
	int ret;

	ret = v4l2_subdev_call_state_active(sd, pad, get_fmt, &fmt);
	if (ret)
		return ret;

	cfg = cfg_by_mbus_code(fmt.format.code);
	if (!cfg || cfg->pixelformat != cam->fmt.pixelformat || fmt.format.width != cam->fmt.width ||
	    fmt.format.height != cam->fmt.height) {
		dev_warn(cam->dev, "capture format %p4cc %ux%u does not match the receiver output %#x %ux%u\n",
			 &cam->fmt.pixelformat, cam->fmt.width, cam->fmt.height, fmt.format.code, fmt.format.width,
			 fmt.format.height);
		return -EPIPE;
	}
	return 0;
}

static const struct media_entity_operations video_entity_ops = {
	.link_validate = video_link_validate,
};

int mtk_camsys_video_register(struct mtk_camsys *cam)
{
	struct video_device *vdev = &cam->vdev;
	struct vb2_queue *q = &cam->queue;
	int ret;

	mutex_init(&cam->video_lock);
	spin_lock_init(&cam->qlock);
	INIT_LIST_HEAD(&cam->queued);
	mtk_camsys_video_set_default_format(cam);

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = cam;
	q->buf_struct_size = sizeof(struct mtk_camsys_buf);
	q->ops = &queue_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &cam->video_lock;
	q->dev = cam->dev;
	q->min_queued_buffers = 1;
	q->min_reqbufs_allocation = MTK_CAMSYS_MIN_BUFFERS;
	ret = vb2_queue_init(q);
	if (ret)
		return ret;

	strscpy(vdev->name, "mtk-camsys capture", sizeof(vdev->name));
	vdev->v4l2_dev = &cam->v4l2_dev;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->fops = &video_fops;
	vdev->ioctl_ops = &ioctl_ops;
	vdev->lock = &cam->video_lock;
	vdev->queue = q;
	vdev->release = video_device_release_empty;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
	vdev->entity.ops = &video_entity_ops;
	video_set_drvdata(vdev, cam);

	cam->vdev_pad.flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	ret = media_entity_pads_init(&vdev->entity, 1, &cam->vdev_pad);
	if (ret)
		return ret;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_entity;

	ret = media_create_pad_link(&cam->csi2.entity, MTK_CSI2_PAD_SRC, &vdev->entity, 0,
				    MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		goto err_video;
	return 0;

err_video:
	video_unregister_device(vdev);
err_entity:
	media_entity_cleanup(&vdev->entity);
	return ret;
}

void mtk_camsys_video_unregister(struct mtk_camsys *cam)
{
	video_unregister_device(&cam->vdev);
	media_entity_cleanup(&cam->vdev.entity);
}
