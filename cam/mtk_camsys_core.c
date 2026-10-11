// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT8127 camera subsystem driver: probe, media graph and sensor binding.
 *
 * Graph: sensor subdevs (rear SP2508, front SP0A20) -> mtk-seninf (CSI-2 receiver) -> capture video node. The
 * sensors are bound through the fwnode graph of the "ports" node and take their master clock from this device.
 *
 * Copyright (c) 2026 Marc Lainez <marc.lainez@gmail.com>
 */
#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <media/v4l2-fwnode.h>
#include <media/videobuf2-dma-contig.h>

#include "mtk_camsys.h"

struct mtk_camsys_asc {
	struct v4l2_async_connection asc;
	unsigned int port;
};

static const char * const reg_names[] = {
	"imgsys", "isp", "seninf", "mipi-rx", "mipi-ana", "spm", "topckgen", "gpio",
};

static int mtk_camsys_map(struct mtk_camsys *cam)
{
	struct platform_device *pdev = to_platform_device(cam->dev);
	void __iomem **bases[] = {
		&cam->imgsys, &cam->isp, &cam->seninf, &cam->mipi_rx,
		&cam->mipi_ana, &cam->spm, &cam->topckgen, &cam->gpio,
	};
	unsigned int i;

	/*
	 * The windows are mapped without claiming the regions: SPM, TOPCKGEN and GPIO belong to other drivers and
	 * only the bits this driver needs are changed (read-modify-write).
	 */
	for (i = 0; i < ARRAY_SIZE(reg_names); i++) {
		struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, reg_names[i]);

		if (!res)
			return dev_err_probe(cam->dev, -EINVAL, "missing register window \"%s\"\n", reg_names[i]);
		*bases[i] = devm_ioremap(cam->dev, res->start, resource_size(res));
		if (!*bases[i])
			return -ENOMEM;
	}
	return 0;
}

static int mtk_camsys_bound(struct v4l2_async_notifier *nf, struct v4l2_subdev *sd, struct v4l2_async_connection *asc)
{
	struct mtk_camsys *cam = container_of(nf, struct mtk_camsys, notifier);
	unsigned int port = container_of(asc, struct mtk_camsys_asc, asc)->port;
	u32 flags = 0;
	int pad, ret;

	pad = media_entity_get_fwnode_pad(&sd->entity, asc->match.fwnode, MEDIA_PAD_FL_SOURCE);
	if (pad < 0)
		return pad;

	/* The rear sensor feeds the ISP by default; the front link is enabled at completion only without it. */
	if (port == MTK_CAMSYS_PORT_REAR && cam->active_port < 0)
		flags = MEDIA_LNK_FL_ENABLED;

	ret = media_create_pad_link(&sd->entity, pad, &cam->csi2.entity, port, flags);
	if (ret)
		return ret;

	cam->ports[port].sensor = sd;
	if (flags & MEDIA_LNK_FL_ENABLED)
		mtk_camsys_csi2_set_active(cam, port);
	return 0;
}

static void mtk_camsys_unbind(struct v4l2_async_notifier *nf, struct v4l2_subdev *sd,
			      struct v4l2_async_connection *asc)
{
	struct mtk_camsys *cam = container_of(nf, struct mtk_camsys, notifier);
	unsigned int port = container_of(asc, struct mtk_camsys_asc, asc)->port;

	cam->ports[port].sensor = NULL;
	if (cam->active_port == port)
		mtk_camsys_csi2_set_active(cam, -1);
}

static int mtk_camsys_complete(struct v4l2_async_notifier *nf)
{
	struct mtk_camsys *cam = container_of(nf, struct mtk_camsys, notifier);
	int ret;

	ret = v4l2_device_register_subdev_nodes(&cam->v4l2_dev);
	if (ret)
		return ret;

	if (cam->active_port < 0 && cam->ports[MTK_CAMSYS_PORT_FRONT].sensor) {
		struct v4l2_subdev *sensor = cam->ports[MTK_CAMSYS_PORT_FRONT].sensor;
		struct media_link *link = media_entity_find_link(&sensor->entity.pads[0],
								 &cam->csi2_pads[MTK_CAMSYS_PORT_FRONT]);

		if (link) {
			ret = media_entity_setup_link(link, MEDIA_LNK_FL_ENABLED);
			if (ret)
				return ret;
		}
	}

	if (!vb2_is_busy(&cam->queue))
		mtk_camsys_video_set_default_format(cam);
	return 0;
}

static const struct v4l2_async_notifier_operations mtk_camsys_nf_ops = {
	.bound = mtk_camsys_bound,
	.unbind = mtk_camsys_unbind,
	.complete = mtk_camsys_complete,
};

/* One endpoint per sensor port: port 0 is the rear sensor on CSI0, port 1 the front sensor on CSI1. */
static int mtk_camsys_parse_ports(struct mtk_camsys *cam)
{
	struct fwnode_handle *fwnode = dev_fwnode(cam->dev);
	unsigned int i, found = 0;
	int ret;

	v4l2_async_nf_init(&cam->notifier, &cam->v4l2_dev);
	cam->notifier.ops = &mtk_camsys_nf_ops;

	for (i = 0; i < MTK_CAMSYS_NUM_PORTS; i++) {
		struct v4l2_fwnode_endpoint vep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
		struct mtk_camsys_asc *asc;
		struct fwnode_handle *ep;

		ep = fwnode_graph_get_endpoint_by_id(fwnode, i, 0, 0);
		if (!ep)
			continue;

		ret = v4l2_fwnode_endpoint_parse(ep, &vep);
		if (ret) {
			dev_err(cam->dev, "port %u: invalid endpoint: %d\n", i, ret);
			goto err_put;
		}
		if (!vep.bus.mipi_csi2.num_data_lanes || vep.bus.mipi_csi2.num_data_lanes > 4) {
			dev_err(cam->dev, "port %u: invalid data-lanes\n", i);
			ret = -EINVAL;
			goto err_put;
		}

		asc = v4l2_async_nf_add_fwnode_remote(&cam->notifier, ep, struct mtk_camsys_asc);
		if (IS_ERR(asc)) {
			ret = PTR_ERR(asc);
			goto err_put;
		}
		asc->port = i;

		cam->ports[i].cfg = &mtk_camsys_port_cfgs[i];
		cam->ports[i].present = true;
		cam->ports[i].lanes = vep.bus.mipi_csi2.num_data_lanes;
		fwnode_handle_put(ep);
		found++;
		continue;
err_put:
		fwnode_handle_put(ep);
		v4l2_async_nf_cleanup(&cam->notifier);
		return ret;
	}

	if (!found) {
		v4l2_async_nf_cleanup(&cam->notifier);
		return dev_err_probe(cam->dev, -ENODEV, "no sensor endpoint in the ports node\n");
	}
	return 0;
}

static int mtk_camsys_stats_show(struct seq_file *m, void *unused)
{
	struct mtk_camsys *cam = m->private;
	unsigned int bit;

	seq_printf(m, "streaming %d sequence %u drops %u hw_errors %u\n", cam->streaming, cam->sequence, cam->drops,
		   cam->hw_errors);
	seq_printf(m, "last INT_STATUS %#010x DMA_INT %#010x dma_irqs %u\n", cam->last_st, cam->last_dma,
		   cam->dma_irqs);
	for (bit = 0; bit < 32; bit++)
		if (cam->irq_bits[bit])
			seq_printf(m, "bit %2u: %u\n", bit, cam->irq_bits[bit]);
	if (cam->power_count) {
		seq_printf(m, "TG_VF_CON %#010x TG_INTER_ST %#010x IMGO_BASE %#010x\n",
			   readl(cam->isp + ISP_TG_VF_CON), readl(cam->isp + ISP_TG_INTER_ST),
			   readl(cam->isp + ISP_IMGO_BASE_ADDR));
		seq_printf(m, "TG_SEN_GRAB_PXL %#010x TG_SEN_GRAB_LIN %#010x\n",
			   readl(cam->isp + ISP_TG_SEN_GRAB_PXL), readl(cam->isp + ISP_TG_SEN_GRAB_LIN));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mtk_camsys_stats);

static int mtk_camsys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_camsys *cam;
	int ret;

	cam = devm_kzalloc(dev, sizeof(*cam), GFP_KERNEL);
	if (!cam)
		return -ENOMEM;
	cam->dev = dev;
	cam->active_port = -1;
	spin_lock_init(&cam->tg_lock);
	mutex_init(&cam->power_lock);
	platform_set_drvdata(pdev, cam);

	ret = mtk_camsys_map(cam);
	if (ret)
		return ret;

	cam->irq = platform_get_irq(pdev, 0);
	if (cam->irq < 0)
		return cam->irq;
	ret = devm_request_irq(dev, cam->irq, mtk_camsys_isp_irq, IRQF_NO_AUTOEN, dev_name(dev), cam);
	if (ret)
		return dev_err_probe(dev, ret, "cannot request the ISP interrupt\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ret = vb2_dma_contig_set_max_seg_size(dev, UINT_MAX);
	if (ret)
		return ret;

	ret = mtk_camsys_clk_register(cam);
	if (ret)
		return ret;

	cam->mdev.dev = dev;
	strscpy(cam->mdev.model, "MediaTek MT8127 camsys", sizeof(cam->mdev.model));
	snprintf(cam->mdev.bus_info, sizeof(cam->mdev.bus_info), "platform:%s", dev_name(dev));
	media_device_init(&cam->mdev);
	cam->v4l2_dev.mdev = &cam->mdev;

	ret = v4l2_device_register(dev, &cam->v4l2_dev);
	if (ret) {
		dev_err(dev, "cannot register the V4L2 device: %d\n", ret);
		goto err_media_cleanup;
	}

	ret = mtk_camsys_csi2_register(cam);
	if (ret)
		goto err_v4l2;

	ret = mtk_camsys_video_register(cam);
	if (ret)
		goto err_csi2;

	ret = mtk_camsys_parse_ports(cam);
	if (ret)
		goto err_video;

	ret = media_device_register(&cam->mdev);
	if (ret)
		goto err_nf_cleanup;

	ret = v4l2_async_nf_register(&cam->notifier);
	if (ret) {
		dev_err(dev, "cannot register the sensor notifier: %d\n", ret);
		goto err_media;
	}

	cam->debugfs = debugfs_create_dir("mtk_camsys", NULL);
	debugfs_create_file("stats", 0444, cam->debugfs, cam, &mtk_camsys_stats_fops);
	return 0;

err_media:
	media_device_unregister(&cam->mdev);
err_nf_cleanup:
	v4l2_async_nf_cleanup(&cam->notifier);
err_video:
	mtk_camsys_video_unregister(cam);
err_csi2:
	mtk_camsys_csi2_unregister(cam);
err_v4l2:
	v4l2_device_unregister(&cam->v4l2_dev);
err_media_cleanup:
	media_device_cleanup(&cam->mdev);
	return ret;
}

static void mtk_camsys_remove(struct platform_device *pdev)
{
	struct mtk_camsys *cam = platform_get_drvdata(pdev);

	debugfs_remove_recursive(cam->debugfs);
	v4l2_async_nf_unregister(&cam->notifier);
	v4l2_async_nf_cleanup(&cam->notifier);
	media_device_unregister(&cam->mdev);
	mtk_camsys_video_unregister(cam);
	mtk_camsys_csi2_unregister(cam);
	v4l2_device_unregister(&cam->v4l2_dev);
	media_device_cleanup(&cam->mdev);
}

static const struct of_device_id mtk_camsys_of_match[] = {
	{ .compatible = "mediatek,mt8127-camsys" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_camsys_of_match);

static struct platform_driver mtk_camsys_driver = {
	.probe = mtk_camsys_probe,
	.remove = mtk_camsys_remove,
	.driver = {
		.name = MTK_CAMSYS_NAME,
		.of_match_table = mtk_camsys_of_match,
	},
};
module_platform_driver(mtk_camsys_driver);

MODULE_DESCRIPTION("MediaTek MT8127 camera subsystem driver");
MODULE_AUTHOR("Marc Lainez <marc.lainez@gmail.com>");
MODULE_LICENSE("GPL");
