// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 MediaTek Inc.
 *
 * Author: Frederic Chen <frederic.chen@mediatek.com>
 *         Holmes Chiou <holmes.chiou@mediatek.com>
 *
 */
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/of_address.h>
#include <linux/pm_runtime.h>
#include <linux/remoteproc.h>
#include <linux/dma-mapping.h>
#include <linux/clk.h>
#include <linux/of_platform.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/clk.h>
#include "mtk-ipesys-me.h"
#include "mtk_imgsys-debug.h"
#include "mtk_imgsys-engine.h"

static struct clk_bulk_data imgsys_isp7_me_clks[] = {
	{ .id = "ME_CG_IPE" },
	{ .id = "ME_CG_IPE_TOP" },
	{ .id = "ME_CG" },
	{ .id = "ME_CG_LARB12" },
};

static struct ipesys_me_device *me_dev;

void ipesys_me_set_initial_value(struct mtk_imgsys_dev *imgsys_dev)
{
	int ret;

	pm_runtime_get_sync(me_dev->dev);
	ret = clk_bulk_prepare_enable(me_dev->me_clk.clk_num, me_dev->me_clk.clks);
	if (ret)
		pr_info("failed to enable clock:%d\n", ret);
}
EXPORT_SYMBOL(ipesys_me_set_initial_value);

void ipesys_me_uninit(struct mtk_imgsys_dev *imgsys_dev)
{
	pm_runtime_put_sync(me_dev->dev);
	clk_bulk_disable_unprepare(me_dev->me_clk.clk_num, me_dev->me_clk.clks);
}
EXPORT_SYMBOL(ipesys_me_uninit);

void ipesys_me_debug_dump(struct mtk_imgsys_dev *imgsys_dev, unsigned int engine)
{
	void __iomem *me_reg_base;
	unsigned int i;

	me_reg_base = me_dev->regs;
	if (!me_reg_base) {
		dev_info(imgsys_dev->dev, "%s Unable to ioremap dip registers\n",
			 __func__);
		dev_info(imgsys_dev->dev, "%s of_iomap fail, devnode(%s).\n",
			 __func__, imgsys_dev->dev->of_node->name);
	}
	dev_info(imgsys_dev->dev, "%s: dump me regs\n", __func__);
	for (i = ME_CTL_OFFSET; i <= ME_CTL_OFFSET + ME_CTL_RANGE; i += 0x10) {
		dev_info(imgsys_dev->dev, "%s: 0x%08X %08X, %08X, %08X, %08X", __func__,
			 0x15320000 + i,
			 ioread32(me_reg_base + i),
			 ioread32(me_reg_base + i + 0x4),
			 ioread32(me_reg_base + i + 0x8),
			 ioread32(me_reg_base + i + 0xC));
	}
}
EXPORT_SYMBOL(ipesys_me_debug_dump);

void ipesys_me_ndd_dump(struct mtk_imgsys_dev *imgsys_dev,
				struct imgsys_ndd_frm_dump_info *frm_dump_info)
{
	char *me_name;
	char file_name[NDD_FP_SIZE] = "\0";
	void *reg_va;
	ssize_t ret;

	if (frm_dump_info->eng_e != IMGSYS_NDD_ENG_ME)
		return;

	reg_va = ioremap(frm_dump_info->cq_ofst[frm_dump_info->eng_e], ME_REG_RANGE);
	if (!reg_va)
		return;

	me_name = frm_dump_info->user_buffer ? "REG_ME_me.reg" : "REG_ME_me.regKernel";

	ret = snprintf(file_name, sizeof(file_name), "%s%s", frm_dump_info->fp, me_name);
	if (ret < 0 || ret >= sizeof(file_name)) {
		iounmap(reg_va);
		return;
	}

	if (frm_dump_info->user_buffer) {
		ret = copy_to_user(frm_dump_info->user_buffer, file_name, sizeof(file_name));
		if (ret) {
			iounmap(reg_va);
			return;
		}
		frm_dump_info->user_buffer += sizeof(file_name);

		ret = copy_to_user(frm_dump_info->user_buffer, reg_va, ME_REG_RANGE);
		if (ret != 0) {
			iounmap(reg_va);
			return;
		}
		frm_dump_info->user_buffer += ME_REG_RANGE;
	}

	iounmap(reg_va);
}
EXPORT_SYMBOL(ipesys_me_ndd_dump);

static int mtk_ipesys_me_probe(struct platform_device *pdev)
{
	int ret = 0;
	int ret_result = 0;
	struct device_link *link;
	int larbs_num;
	struct device_node *larb_node;
	struct platform_device *larb_pdev;

	me_dev = devm_kzalloc(&pdev->dev, sizeof(struct ipesys_me_device) * 1, GFP_KERNEL);
	if (!me_dev)
		return -ENOMEM;

	me_dev->dev = &pdev->dev;
	me_dev->me_clk.clk_num = ARRAY_SIZE(imgsys_isp7_me_clks);
	me_dev->me_clk.clks = imgsys_isp7_me_clks;
	me_dev->regs = of_iomap(pdev->dev.of_node, 0);
	ret = devm_clk_bulk_get(&pdev->dev, me_dev->me_clk.clk_num, me_dev->me_clk.clks);
	if (ret)
		pr_info("failed to get raw clock:%d\n", ret);

	larbs_num = of_count_phandle_with_args(pdev->dev.of_node,
					       "mediatek,larb", NULL);
	dev_info(me_dev->dev, "%d larbs to be added", larbs_num);

	larb_node = of_parse_phandle(pdev->dev.of_node, "mediatek,larb", 0);
	if (!larb_node) {
		dev_info(me_dev->dev,
			 "%s: larb node not found\n", __func__);
	}

	larb_pdev = of_find_device_by_node(larb_node);
	if (!larb_pdev) {
		of_node_put(larb_node);
		dev_info(me_dev->dev,
			 "%s: larb device not found\n", __func__);
	}
	of_node_put(larb_node);

	link = device_link_add(&pdev->dev, &larb_pdev->dev,
			       DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS);

	if (!link)
		dev_info(me_dev->dev, "unable to link SMI LARB\n");

	pm_runtime_enable(&pdev->dev);

	return ret_result;
}

static int mtk_ipesys_me_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);
	devm_kfree(&pdev->dev, me_dev);

	return 0;
}

static int __maybe_unused mtk_ipesys_me_runtime_suspend(struct device *dev)
{
	return 0;
}

static int __maybe_unused mtk_ipesys_me_runtime_resume(struct device *dev)
{
	return 0;
}

static int __maybe_unused mtk_ipesys_me_pm_suspend(struct device *dev)
{
	return 0;
}

static int __maybe_unused mtk_ipesys_me_pm_resume(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops mtk_ipesys_me_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_ipesys_me_pm_suspend,
				mtk_ipesys_me_pm_resume)
	SET_RUNTIME_PM_OPS(mtk_ipesys_me_runtime_suspend,
			   mtk_ipesys_me_runtime_resume,
			   NULL)
};

static const struct of_device_id mtk_ipesys_me_of_match[] = {
	{ .compatible = "mediatek,ipesys-me", },
	{}
};
MODULE_DEVICE_TABLE(of, mtk_ipesys_me_of_match);

static struct platform_driver mtk_ipesys_me_driver = {
	.probe   = mtk_ipesys_me_probe,
	.remove  = mtk_ipesys_me_remove,
	.driver  = {
		.name = "camera-me",
		.owner	= THIS_MODULE,
		.pm = &mtk_ipesys_me_pm_ops,
		.of_match_table = mtk_ipesys_me_of_match,
	}
};

module_platform_driver(mtk_ipesys_me_driver);

MODULE_AUTHOR("Marvin Lin <marvin.lin@mediatek.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mediatek ME driver");

