// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/compat.h>
#include <linux/freezer.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/videodev2.h>

#include "mtk-hcp.h"
#include "mtk-hcp_isp71.h"

#include <linux/dma-heap.h>
#include <uapi/linux/dma-heap.h>
#include <linux/dma-heap.h>
#include <linux/dma-direction.h>
#include <linux/scatterlist.h>
#include <linux/dma-buf.h>

/*
 * HCP (Hetero Control Processor ) is a tiny processor controlling
 * the methodology of register programming. If the module support
 * to run on CM4 then it will send data to CM4 to program register.
 * Or it will send the data to user library and let RED to program
 * register.
 */

#define HCP_TIMEOUT_MS          4000U

#define SYNC_SEND               1
#define ASYNC_SEND              0
static struct mtk_hcp *hcp_mtkdev;

static int mtk_hcp_of_rproc(struct mtk_hcp *hcp,
			    struct platform_device *pdev)
{
	struct device *dev = hcp->dev;

	hcp->scp = scp_get(pdev);
	if (!hcp->scp) {
		dev_err(dev, "%s failed to get scp reference\n", __func__);
		return -ENODEV;
	}

	hcp->rproc_handle = scp_get_rproc(hcp->scp);
	dev_info(dev, "%s hcp rproc_phandle: 0x%pK\n", __func__, hcp->rproc_handle);
	hcp->scp_dev = scp_get_device(hcp->scp);

	return 0;
}

static void hcp_ipi_handler(void *data, unsigned int len, void *priv)
{
	struct mtk_hcp *hcp_dev = (struct mtk_hcp *)priv;
	struct share_buf *scp_msg = (struct share_buf *)data;
	struct hcp_desc *desc = &hcp_dev->hcp_desc_table[scp_msg->id];

	if (scp_msg->id < HCP_MAX_ID && desc->handler) {
		desc->handler(scp_msg->share_data, scp_msg->len,
			      hcp_dev->hcp_desc_table[scp_msg->id].priv);
	}
}

static int hcp_ipi_init(struct mtk_hcp *hcp)
{
	struct device *dev = hcp->dev;
	int ret = 0;

	ret = rproc_boot(hcp->rproc_handle);
	if (ret) {
		dev_err(dev, "%s failed to rproc_boot\n", __func__);
		return ret;
	}

	ret = scp_ipi_register(hcp->scp, SCP_IPI_IMGSYS_CMD,
			       hcp_ipi_handler, hcp);
	if (ret) {
		dev_err(dev, "%s failed to register IPI cmd\n", __func__);
		return ret;
	}

	dev_info(dev, "%s success to register IPI cmd of SCP_IPI_IMGSYS_CMD\n", __func__);

	return ret;
}

int mtk_hcp_register(struct platform_device *pdev,
		     enum hcp_id id, hcp_handler_t handler,
		     const char *name, void *priv)
{
	struct mtk_hcp *hcp_dev = platform_get_drvdata(pdev);

	if (!hcp_dev) {
		pr_info("%s hcp device in not ready\n", __func__);
		return -EPROBE_DEFER;
	}

	if (id >= HCP_INIT_ID && id < HCP_MAX_ID && handler) {
		hcp_dev->hcp_desc_table[id].name = name;
		hcp_dev->hcp_desc_table[id].handler = handler;
		hcp_dev->hcp_desc_table[id].priv = priv;
		return 0;
	}

	return -EINVAL;
}
EXPORT_SYMBOL(mtk_hcp_register);

int mtk_hcp_unregister(struct platform_device *pdev, enum hcp_id id)
{
	struct mtk_hcp *hcp_dev = platform_get_drvdata(pdev);

	if (!hcp_dev) {
		dev_info(&pdev->dev, "%s hcp device in not ready\n", __func__);
		return -EPROBE_DEFER;
	}

	if (id == HCP_DIP_INIT_ID)
		rproc_shutdown(hcp_dev->rproc_handle);

	if (id >= HCP_INIT_ID && id < HCP_MAX_ID) {
		memset((void *)&hcp_dev->hcp_desc_table[id], 0, sizeof(struct hcp_desc));
		return 0;
	}

	dev_info(&pdev->dev, "%s register hcp id %d with invalid arguments\n",
		 __func__, id);

	return -EINVAL;
}
EXPORT_SYMBOL(mtk_hcp_unregister);

static int hcp_send_internal(struct platform_device *pdev,
			     enum hcp_id id, void *buf,
			     unsigned int len, int req_fd,
			     unsigned int wait)
{
	struct mtk_hcp *hcp_dev = platform_get_drvdata(pdev);
	struct share_buf send_obj = { 0 };
	int ret = 0;

	dev_dbg(&pdev->dev, "%s id:%d len %d\n",
		__func__, id, len);

	if (id < HCP_INIT_ID || id >= HCP_MAX_ID ||
	    len > sizeof(send_obj.share_data) || !buf) {
		dev_info(&pdev->dev,
			 "%s failed to send hcp message (Invalid arg.), len/sz(%d/%zu)\n",
			 __func__, len, sizeof(send_obj.share_data));
		return -EINVAL;
	}

	if (id == HCP_IMGSYS_INIT_ID) {
		ret = hcp_ipi_init(hcp_dev);
		if (ret) {
			dev_err(&pdev->dev, "%s failed to register IPI cmd\n", __func__);
			return ret;
		}
	}

	send_obj.len = len;
	send_obj.id = id;
	memcpy((void *)send_obj.share_data, buf, len);

	send_obj.info.send.hcp = id;
	send_obj.info.send.req = req_fd;
	send_obj.info.send.ack = (wait ? 1 : 0);
	send_obj.info.send.seq = 0;

	ret = scp_ipi_send(hcp_dev->scp, SCP_IPI_IMGSYS_CMD, (void *)&send_obj,
			   sizeof(send_obj), wait ? msecs_to_jiffies(HCP_TIMEOUT_MS) : 0);
	if (ret)
		dev_err(&pdev->dev, "%s: send SCP(%d) failed %d\n", __func__, id, ret);

	return 0;
}

int mtk_hcp_send(struct platform_device *pdev,
		 enum hcp_id id, void *buf,
		 unsigned int len, int req_fd)
{
	return hcp_send_internal(pdev, id, buf, len, req_fd, SYNC_SEND);
}
EXPORT_SYMBOL(mtk_hcp_send);

int mtk_hcp_send_async(struct platform_device *pdev,
		       enum hcp_id id, void *buf,
		       unsigned int len, int req_fd)
{
	return hcp_send_internal(pdev, id, buf, len, req_fd, ASYNC_SEND);
}
EXPORT_SYMBOL(mtk_hcp_send_async);

struct platform_device *mtk_hcp_get_plat_device(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *hcp_node;
	struct platform_device *hcp_pdev;

	hcp_node = of_parse_phandle(dev->of_node, "mediatek,hcp", 0);
	if (!hcp_node) {
		dev_info(&pdev->dev, "%s can't get hcp node.\n", __func__);
		return NULL;
	}

	hcp_pdev = of_find_device_by_node(hcp_node);
	if (WARN_ON(!hcp_pdev)) {
		dev_info(&pdev->dev, "%s hcp pdev failed.\n", __func__);
		of_node_put(hcp_node);
		return NULL;
	}

	return hcp_pdev;
}
EXPORT_SYMBOL(mtk_hcp_get_plat_device);

void *mtk_hcp_get_reserve_mem_virt(struct platform_device *pdev, unsigned int id)
{
	struct mtk_hcp *hcp_dev = platform_get_drvdata(pdev);
	void *buffer;

	if (id >= NUMS_MEM_ID) {
		pr_info("[HCP] no reserve memory for %d", id);
		return 0;
	} else {
		if (!hcp_dev->data->get_reserve_mem_virt) {
			dev_info(&pdev->dev, "%s: not supported\n", __func__);
			return NULL;
		}

		buffer = hcp_dev->data->get_reserve_mem_virt(id);
		if (!buffer)
			dev_info(&pdev->dev, "%s: get_reserve_mem_virt is null for id:%u\n", __func__, id);

		return buffer;
	}
}
EXPORT_SYMBOL(mtk_hcp_get_reserve_mem_virt);

void *mtk_hcp_get_gce_mem_virt(struct platform_device *pdev)
{
	struct mtk_hcp *hcp_dev = platform_get_drvdata(pdev);
	void *buffer;

	if (!hcp_dev->data->get_gce_virt) {
		dev_info(&pdev->dev, "%s: not supported\n", __func__);
		return NULL;
	}

	buffer = hcp_dev->data->get_gce_virt();
	if (!buffer)
		dev_info(&pdev->dev, "%s: gce buffer is null\n", __func__);

	return buffer;
}
EXPORT_SYMBOL(mtk_hcp_get_gce_mem_virt);

phys_addr_t mtk_hcp_get_gce_mem_size(struct platform_device *pdev)
{
	struct mtk_hcp *hcp_dev = platform_get_drvdata(pdev);
	phys_addr_t mem_sz;

	if (!hcp_dev->data->get_gce_mem_size) {
		dev_info(&pdev->dev, "%s: not supported\n", __func__);
		return 0;
	}

	mem_sz = hcp_dev->data->get_gce_mem_size();

	return mem_sz;
}
EXPORT_SYMBOL(mtk_hcp_get_gce_mem_size);

int mtk_hcp_allocate_working_buffer(struct platform_device *pdev, unsigned int mode)
{
	struct mtk_hcp *hcp_dev = platform_get_drvdata(pdev);

	if (!hcp_dev->data->allocate)
		return -EFAULT;

	return hcp_dev->data->allocate(hcp_dev, mode);
}
EXPORT_SYMBOL(mtk_hcp_allocate_working_buffer);

int mtk_hcp_release_working_buffer(struct platform_device *pdev)
{
	struct mtk_hcp *hcp_dev = platform_get_drvdata(pdev);

	if (!hcp_dev->data->release)
		return -EFAULT;

	return hcp_dev->data->release(hcp_dev);
}
EXPORT_SYMBOL(mtk_hcp_release_working_buffer);

int mtk_hcp_get_init_info(struct platform_device *pdev,
			  struct img_init_info *info)
{
	struct mtk_hcp *hcp_dev = platform_get_drvdata(pdev);

	if (!hcp_dev->data->get_init_info || !info) {
		dev_info(&pdev->dev, "%s:not supported\n", __func__);
		return -1;
	}

	return hcp_dev->data->get_init_info(info);
}
EXPORT_SYMBOL(mtk_hcp_get_init_info);

static int mtk_hcp_probe(struct platform_device *pdev)
{
	struct mtk_hcp *hcp_dev;
	int ret = 0;

	hcp_dev = devm_kzalloc(&pdev->dev, sizeof(*hcp_dev), GFP_KERNEL);
	if (!hcp_dev)
		return -ENOMEM;

	hcp_mtkdev = hcp_dev;
	hcp_dev->dev = &pdev->dev;

	hcp_dev->data = of_device_get_match_data(&pdev->dev);

	platform_set_drvdata(pdev, hcp_dev);
	dev_set_drvdata(&pdev->dev, hcp_dev);

	if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(34)))
		dev_info(&pdev->dev, "%s:No DMA available\n", __func__);

	if (!pdev->dev.dma_parms) {
		pdev->dev.dma_parms =
			devm_kzalloc(hcp_dev->dev, sizeof(*hcp_dev->dev->dma_parms), GFP_KERNEL);
	}
	if (hcp_dev->dev->dma_parms) {
		ret = dma_set_max_seg_size(hcp_dev->dev, (unsigned int)DMA_BIT_MASK(34));
		if (ret)
			dev_info(hcp_dev->dev, "Failed to set DMA segment size\n");
	}

	ret = mtk_hcp_of_rproc(hcp_dev, pdev);
	if (ret < 0) {
		dev_info(&pdev->dev, "mtk_hcp_of_rproc fail  err= %d", ret);
		goto err_alloc;
	}

	dev_info(&pdev->dev, "- X. hcp driver probe success.\n");

	return 0;

err_alloc:
	devm_kfree(&pdev->dev, hcp_dev);

	dev_info(&pdev->dev, "- X. hcp driver probe fail.\n");

	return ret;
}

static const struct of_device_id mtk_hcp_match[] = {
	{ .compatible = "mediatek,hcp", .data = (void *)&isp71_hcp_data },
	{},
};
MODULE_DEVICE_TABLE(of, mtk_hcp_match);

static int mtk_hcp_remove(struct platform_device *pdev)
{
	struct mtk_hcp *hcp_dev = platform_get_drvdata(pdev);

	devm_kfree(&pdev->dev, hcp_dev);
	dev_dbg(&pdev->dev, "- X. hcp driver remove.\n");
	return 0;
}

static struct platform_driver mtk_hcp_driver = {
	.probe  = mtk_hcp_probe,
	.remove = mtk_hcp_remove,
	.driver = {
		.name = "mtk_hcp",
		.owner  = THIS_MODULE,
		.of_match_table = mtk_hcp_match,
	},
};

module_platform_driver(mtk_hcp_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mediatek hetero control process driver");
