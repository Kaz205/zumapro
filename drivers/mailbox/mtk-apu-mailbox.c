// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <asm/io.h>
#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/mailbox_controller.h>
#include <linux/mailbox/mtk-apu-mailbox.h>
#include <linux/platform_device.h>

#define INBOX		(0x0)
#define OUTBOX		(0x20)
#define INBOX_IRQ	(0xc0)
#define OUTBOX_IRQ	(0xc4)
#define INBOX_IRQ_MASK	(0xd0)

#define SPARE_OFF_START	(0x40)
#define SPARE_OFF_END	(0xB0)

#define MSG_MBOX_SLOTS	(8)

struct mtk_apu_mailbox {
	struct device *dev;
	void __iomem *regs;
	struct mbox_controller controller;
	u32 msgs[MSG_MBOX_SLOTS];
};

struct mtk_apu_mailbox *g_mbox;

static irqreturn_t mtk_apu_mailbox_irq_top_half(int irq, void *dev_id)
{
	struct mtk_apu_mailbox *mbox = dev_id;
	struct mbox_chan *link = &mbox->controller.chans[0];
	int i;

	for (i = 0; i < MSG_MBOX_SLOTS; i++)
		mbox->msgs[i] = readl(mbox->regs + OUTBOX + i * sizeof(u32));

	mbox_chan_received_data(link, &mbox->msgs);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t mtk_apu_mailbox_irq_btm_half(int irq, void *dev_id)
{
	struct mtk_apu_mailbox *mbox = dev_id;
	struct mbox_chan *link = &mbox->controller.chans[0];

	mbox_chan_received_data_bh(link, NULL);
	writel(readl(mbox->regs + OUTBOX_IRQ), mbox->regs + OUTBOX_IRQ);

	return IRQ_HANDLED;
}

static int mtk_apu_mailbox_send_data(struct mbox_chan *chan, void *data)
{
	struct mtk_apu_mailbox *mbox = container_of(chan->mbox,
						    struct mtk_apu_mailbox,
						    controller);
	int send_cnt = ((int*)data)[0];
	int i;

	if (send_cnt <= 0 || send_cnt > MSG_MBOX_SLOTS) {
		dev_err(mbox->dev, "%s: invalid send_cnt %d\n", __func__, send_cnt);
		return -EINVAL;
	}

	/*
	 *	Mask lowest "send_cnt-1" interrupts bits, so the interrupt on the other side
	 *	triggers only after the last data slot is written (sent).
	 */
	writel(GENMASK(send_cnt-2, 0), mbox->regs + INBOX_IRQ_MASK);
	for (i = 0; i < send_cnt; i++)
		writel(((u32 *)data)[i+1], mbox->regs + INBOX + i * sizeof(u32));

	return 0;
}

static bool mtk_apu_mailbox_last_tx_done(struct mbox_chan *chan)
{
	struct mtk_apu_mailbox *mbox = container_of(chan->mbox,
						    struct mtk_apu_mailbox,
						    controller);

	return readl(mbox->regs + INBOX_IRQ) == 0;
}

static const struct mbox_chan_ops mtk_apu_mailbox_ops = {
	.send_data = mtk_apu_mailbox_send_data,
	.last_tx_done = mtk_apu_mailbox_last_tx_done,
};

/**
 * mtk_apu_mbox_write - write value to specifice mtk_apu_mbox spare register.
 * @val: value to be written.
 * @offset: offset of the spare register.
 */
void mtk_apu_mbox_write(u32 val, u32 offset)
{
	if (!g_mbox) {
		pr_err("mtk apu mbox was not initialized, stop writing register\n");
		return;
	}

	if (offset < SPARE_OFF_START || offset >= SPARE_OFF_END) {
		dev_err(g_mbox->dev, "Invalid offset %d for mtk apu mbox spare register\n", offset);
		return;
	}

	writel(val, g_mbox->regs + offset);
}
EXPORT_SYMBOL_NS(mtk_apu_mbox_write, MTK_APU_MAILBOX);

/**
 * mtk_apu_mbox_read - read value to specifice mtk_apu_mbox spare register.
 * @offset: offset of the spare register.
 */
u32 mtk_apu_mbox_read(u32 offset)
{
	if (!g_mbox) {
		pr_err("mtk apu mbox was not initialized, stop reading register\n");
		return 0;
	}

	if (offset < SPARE_OFF_START || offset >= SPARE_OFF_END) {
		dev_err(g_mbox->dev, "Invalid offset %d for mtk apu mbox spare register\n", offset);
		return 0;
	}

	return readl(g_mbox->regs + offset);
}
EXPORT_SYMBOL_NS(mtk_apu_mbox_read, MTK_APU_MAILBOX);

static int mtk_apu_mailbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_apu_mailbox *mbox;
	int irq = -1, ret = 0;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	mbox->dev = dev;
	platform_set_drvdata(pdev, mbox);

	mbox->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mbox->regs))
		return PTR_ERR(mbox->regs);

	mbox->controller.txdone_irq = false;
	mbox->controller.txdone_poll = true;
	mbox->controller.txpoll_period = 1;
	mbox->controller.ops = &mtk_apu_mailbox_ops;
	mbox->controller.dev = dev;
	/*
	 *	Here we only register 1 mbox channel
	 *	The remaining channels are used by other modules
	 */
	mbox->controller.num_chans = 1;
	mbox->controller.chans = devm_kcalloc(dev, mbox->controller.num_chans,
					      sizeof(*mbox->controller.chans),
					      GFP_KERNEL);
	if (!mbox->controller.chans)
		return -ENOMEM;

	ret = devm_mbox_controller_register(dev, &mbox->controller);
	if (ret)
		return ret;

	dev_info(dev, "registered mtk apu mailbox\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "Failed to get mtk apu mailbox IRQ\n");
		return irq;
	}

	ret = devm_request_threaded_irq(dev, irq, mtk_apu_mailbox_irq_top_half,
					mtk_apu_mailbox_irq_btm_half, IRQF_ONESHOT,
					dev_name(dev), mbox);
	if (ret) {
		dev_err(dev, "Failed to register mtk apu mailbox IRQ: %d\n", ret);
		return -ENODEV;
	}

	g_mbox = mbox;

	return 0;
}

static int mtk_apu_mailbox_remove(struct platform_device *pdev)
{
	g_mbox = NULL;
	return 0;
}

static const struct of_device_id mtk_apu_mailbox_of_match[] = {
	{ .compatible = "mediatek,mt8188-apu-mailbox" },
	{}
};
MODULE_DEVICE_TABLE(of, mtk_apu_mailbox_of_match);

static struct platform_driver mtk_apu_mailbox_driver = {
	.probe = mtk_apu_mailbox_probe,
	.remove = mtk_apu_mailbox_remove,
	.driver = {
		.name = "mtk-apu-mailbox",
		.of_match_table = mtk_apu_mailbox_of_match,
	},
};

module_platform_driver(mtk_apu_mailbox_driver);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek APU Mailbox Driver");
