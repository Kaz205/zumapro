// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/pm_runtime.h>

#include <linux/remoteproc/mtk_apu.h>
#include <linux/soc/mediatek/mtk_apu_hw_logger.h>
#include "mtk_apu_ipi_config.h"
#include "mtk_apu_rproc.h"

#define APU_MBOX_SEND_CNT 4

struct mtk_apu_mbox_hdr {
	unsigned int id;
	unsigned int len;
	unsigned int serial_no;
	unsigned int csum;
};
struct mtk_apu_mbox_send_hdr {
	unsigned int send_cnt;
	struct mtk_apu_mbox_hdr hdr;
};
static struct lock_class_key ipi_lock_key[MTK_APU_IPI_MAX];

static unsigned int tx_serial_no;
static unsigned int rx_serial_no;

static struct mtk_apu *g_apu;

static u8 temp_buf[sizeof(struct mtk_share_obj)];

static uint32_t calculate_csum(void *data, uint32_t len)
{
	uint32_t csum = 0, res = 0, i;
	uint8_t *ptr;

	for (i = 0; i < (len / sizeof(csum)); i++)
		csum += *(((uint32_t *)data) + i);

	ptr = (uint8_t *)data + len / sizeof(csum) * sizeof(csum);
	for (i = 0; i < (len % sizeof(csum)); i++)
		res |= *(ptr + i) << i * 8;

	csum += res;

	return csum;
}

static inline bool bypass_check(u32 id)
{
	/* whitelist IPI used in power off flow */
	return id == MTK_APU_IPI_DEEP_IDLE;
}

static void ipi_usage_cnt_update(struct mtk_apu *apu, u32 id, int diff)
{
	struct mtk_apu_ipi_desc *ipi = &apu->ipi_desc[id];

	if (ipi_attrs[id].ack != IPI_WITH_ACK)
		return;

	spin_lock(&apu->usage_cnt_lock);
	ipi->usage_cnt += diff;
	spin_unlock(&apu->usage_cnt_lock);
}

int mtk_apu_ipi_send(struct mtk_apu *apu, u32 id, void *data, u32 len,
		 u32 wait_ms)
{
	struct timespec64 ts, te;
	struct device *dev;
	struct mtk_apu_mbox_send_hdr send_hdr;
	unsigned long timeout;
	int ret = 0;

	ktime_get_ts64(&ts);

	if (!apu || id <= MTK_APU_IPI_INIT || id >= MTK_APU_IPI_MAX ||
	    id == MTK_APU_IPI_NS_SERVICE || len > MTK_APU_SHARE_BUFFER_SIZE || !data)
		return -EINVAL;

	dev = apu->dev;

	if (!pm_runtime_enabled(dev)) {
		dev_info(dev, "%s: rpm disabled, ipi=%d\n", __func__, id);
		return -EBUSY;
	}

	mutex_lock(&apu->send_lock);

	if (ipi_attrs[id].direction == IPI_HOST_INITIATE &&
	    apu->ipi_inbound_locked == IPI_LOCKED && !bypass_check(id)) {
		mutex_unlock(&apu->send_lock);
		return -EAGAIN;
	}

	ret = mtk_apu_deepidle_power_on_aputop(apu);
	if (ret) {
		dev_info(dev, "mtk_apu_deepidle_power_on_aputop failed\n");
		mutex_unlock(&apu->send_lock);
		return -ESHUTDOWN;
	}

	/*
	 * copy message payload to share buffer, need to do cache flush if
	 * the buffer is cacheable. currently not
	 */
	memcpy(apu->send_buf, data, len);

	send_hdr.send_cnt = APU_MBOX_SEND_CNT;
	send_hdr.hdr.id = id;
	send_hdr.hdr.len = len;
	send_hdr.hdr.csum = calculate_csum(data, len);
	send_hdr.hdr.serial_no = tx_serial_no++;

	ret = mbox_send_message(apu->ch, &send_hdr);
	if (ret < 0) {
		dev_err(dev, "%s: failed to send msg, ret=%d\n", __func__, ret);
		goto unlock_mutex;
	}
	ret = 0;

	apu->ipi_id = id;
	apu->ipi_id_ack[id] = false;

	/* poll ack from remote processor if wait_ms specified */
	if (wait_ms) {
		timeout = jiffies + msecs_to_jiffies(wait_ms);
		ret = wait_event_timeout(apu->ack_wq, apu->ipi_id_ack[id], timeout);

		apu->ipi_id_ack[id] = false;

		if (WARN(!ret, "apu ipi %d ack timeout!", id)) {
			ret = -EIO;
			goto unlock_mutex;
		} else {
			ret = 0;
		}
	}

	ipi_usage_cnt_update(apu, id, 1);

unlock_mutex:
	mutex_unlock(&apu->send_lock);
	ktime_get_ts64(&te);
	ts = timespec64_sub(te, ts);

	dev_dbg(dev, "%s: ipi_id=%d, len=%d, csum=%x, serial_no=%d, elapse=%lld\n",
		__func__, id, len, send_hdr.hdr.csum, send_hdr.hdr.serial_no, timespec64_to_ns(&ts));

	return ret;
}

int mtk_apu_ipi_lock(struct mtk_apu *apu)
{
	struct mtk_apu_ipi_desc *ipi;
	int i;
	bool ready_to_lock = true;

	if (mutex_trylock(&apu->send_lock) == 0)
		return -EBUSY;

	if (apu->ipi_inbound_locked == IPI_LOCKED) {
		dev_info(apu->dev, "%s: ipi already locked\n", __func__);
		mutex_unlock(&apu->send_lock);
		return 0;
	}

	spin_lock(&apu->usage_cnt_lock);
	for (i = 0; i < MTK_APU_IPI_MAX; i++) {
		ipi = &apu->ipi_desc[i];

		if (ipi_attrs[i].ack == IPI_WITH_ACK && ipi->usage_cnt != 0 && !bypass_check(i)) {
			spin_unlock(&apu->usage_cnt_lock);
			dev_info(apu->dev, "%s: ipi %d is still in use %d\n",
				 __func__, i, ipi->usage_cnt);
			spin_lock(&apu->usage_cnt_lock);
			ready_to_lock = false;
		}
	}

	if (!ready_to_lock) {
		spin_unlock(&apu->usage_cnt_lock);
		mutex_unlock(&apu->send_lock);
		return -EBUSY;
	}

	apu->ipi_inbound_locked = IPI_LOCKED;
	spin_unlock(&apu->usage_cnt_lock);
	mutex_unlock(&apu->send_lock);

	return 0;
}

void mtk_apu_ipi_unlock(struct mtk_apu *apu)
{
	mutex_lock(&apu->send_lock);

	if (apu->ipi_inbound_locked == IPI_UNLOCKED)
		dev_info(apu->dev, "%s: ipi already unlocked\n", __func__);

	spin_lock(&apu->usage_cnt_lock);
	apu->ipi_inbound_locked = IPI_UNLOCKED;
	spin_unlock(&apu->usage_cnt_lock);

	mutex_unlock(&apu->send_lock);
}

int mtk_apu_ipi_register(struct mtk_apu *apu, u32 id, ipi_handler_t handler, void *priv)
{
	if (!apu || id >= MTK_APU_IPI_MAX || WARN_ON(!handler)) {
		if (apu)
			dev_err(apu->dev, "%s failed. id=%d, handler=%ps, priv=%p\n",
				__func__, id, handler, priv);
		return -EINVAL;
	}

	dev_dbg(apu->dev, "%s: ipi=%d, handler=%ps, priv=%p", __func__, id, handler, priv);

	mutex_lock(&apu->ipi_desc[id].lock);
	apu->ipi_desc[id].handler = handler;
	apu->ipi_desc[id].priv = priv;
	mutex_unlock(&apu->ipi_desc[id].lock);

	return 0;
}

void mtk_apu_ipi_unregister(struct mtk_apu *apu, u32 id)
{
	if (!apu || id >= MTK_APU_IPI_MAX) {
		if (apu != NULL)
			dev_info(apu->dev, "%s: invalid id=%d\n", __func__, id);
		return;
	}

	mutex_lock(&apu->ipi_desc[id].lock);
	apu->ipi_desc[id].handler = NULL;
	apu->ipi_desc[id].priv = NULL;
	mutex_unlock(&apu->ipi_desc[id].lock);
}

static void mtk_apu_init_ipi_handler(void *data, unsigned int len, void *priv)
{
	struct mtk_apu *apu = priv;

	strscpy(apu->run.fw_ver, data, MTK_APU_FW_VER_LEN);

	apu->run.signaled = 1;
	wake_up_interruptible(&apu->run.wq);
}

static void mtk_apu_ipi_bottom_handle(struct mbox_client *cl, void *mssg)
{
	struct mtk_apu *apu = container_of(cl, struct mtk_apu, cl);
	u32 id, len;
	id = apu->ipi_task.id;
	len = apu->ipi_task.len;

	if (id < 0)
		return;

	mutex_lock(&apu->ipi_desc[id].lock);
	if (!apu->ipi_desc[id].handler) {
		dev_info(apu->dev, "IPI id=%d is not registered", id);
		mutex_unlock(&apu->ipi_desc[id].lock);
		return;
	}

	apu->ipi_desc[id].handler(temp_buf, len, apu->ipi_desc[id].priv);
	ipi_usage_cnt_update(apu, id, -1);
	mutex_unlock(&apu->ipi_desc[id].lock);

	apu->ipi_id_ack[id] = true;
	wake_up(&apu->ack_wq);
}

static void mtk_apu_ipi_handle_rx(struct mbox_client *cl, void *mssg)
{
	struct mtk_apu *apu = container_of(cl, struct mtk_apu, cl);
	struct mtk_share_obj *recv_obj = apu->recv_buf;
	struct mtk_apu_mbox_hdr *hdr = mssg;
	u32 calc_csum;

	if (hdr->len > MTK_APU_SHARE_BUFFER_SIZE) {
		dev_err(apu->dev, "IPI message too long(len %d, max %d)",
			 hdr->len, MTK_APU_SHARE_BUFFER_SIZE);
		apu->ipi_task.id = -1;
		return;
	}

	if (hdr->id >= MTK_APU_IPI_MAX) {
		dev_err(apu->dev, "no such IPI id = %d", hdr->id);
		apu->ipi_task.id = -1;
		return;
	}

	if (hdr->serial_no != rx_serial_no) {
		dev_warn(apu->dev, "unmatched serial_no: curr=%u, recv=%u\n",
			 rx_serial_no, hdr->serial_no);
	}
	rx_serial_no++;

	memcpy(temp_buf, &recv_obj->share_buf, hdr->len);
	calc_csum = calculate_csum(temp_buf, hdr->len);
	if (calc_csum != hdr->csum)
		dev_warn(apu->dev, "csum error: recv=0x%08x, calc=0x%08x\n", hdr->csum, calc_csum);

	apu->ipi_task.id = hdr->id;
	apu->ipi_task.len = hdr->len;
}

static int mtk_apu_send_ipi(struct platform_device *pdev, u32 id, void *buf,
			    unsigned int len, unsigned int wait)
{
	struct mtk_apu *apu = platform_get_drvdata(pdev);

	return mtk_apu_ipi_send(apu, id, buf, len, wait);
}

static int mtk_apu_register_ipi(struct platform_device *pdev, u32 id,
				ipi_handler_t handler, void *priv)
{
	struct mtk_apu *apu = platform_get_drvdata(pdev);

	return mtk_apu_ipi_register(apu, id, handler, priv);
}

static void mtk_apu_unregister_ipi(struct platform_device *pdev, u32 id)
{
	struct mtk_apu *apu = platform_get_drvdata(pdev);

	mtk_apu_ipi_unregister(apu, id);
}

static struct mtk_rpmsg_info mtk_rpmsg_info = {
	.send_ipi = mtk_apu_send_ipi,
	.register_ipi = mtk_apu_register_ipi,
	.unregister_ipi = mtk_apu_unregister_ipi,
	.ns_ipi_id = MTK_APU_IPI_NS_SERVICE,
};

static void mtk_apu_add_rpmsg_subdev(struct mtk_apu *apu)
{
	apu->rpmsg_subdev = mtk_rpmsg_create_rproc_subdev(to_platform_device(apu->dev),
							  &mtk_rpmsg_info);

	if (apu->rpmsg_subdev)
		rproc_add_subdev(apu->rproc, apu->rpmsg_subdev);
}

static void mtk_apu_remove_rpmsg_subdev(struct mtk_apu *apu)
{
	if (apu->rpmsg_subdev) {
		rproc_remove_subdev(apu->rproc, apu->rpmsg_subdev);
		mtk_rpmsg_destroy_rproc_subdev(apu->rpmsg_subdev);
		apu->rpmsg_subdev = NULL;
	}
}

static void mtk_apu_ipi_tx_done(struct mbox_client *cl, void *msg, int ret)
{
	struct mtk_apu *apu = container_of(cl, struct mtk_apu, cl);

	if (ret < 0)
		dev_err(apu->dev, "tx not complete: sent:0x%llx, ret:%d\n", (u64)msg, ret);
	else
		dev_dbg(apu->dev, "tx completed. sent:0x%llx, ret:%d\n", (u64)msg, ret);
}

void mtk_apu_ipi_config_remove(struct mtk_apu *apu)
{
	dma_free_coherent(apu->dev, MTK_APU_SHARE_BUF_SIZE, apu->recv_buf, apu->recv_buf_da);
}

int mtk_apu_ipi_config_init(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;
	struct mtk_apu_ipi_config *ipi_config;
	void *ipi_buf = NULL;
	dma_addr_t ipi_buf_da = 0;

	ipi_config = get_mtk_apu_config_user_ptr(apu->conf_buf, eMTK_APU_IPI_CONFIG);

	/* initialize shared buffer */
	ipi_buf = dma_alloc_coherent(dev, MTK_APU_SHARE_BUF_SIZE, &ipi_buf_da, GFP_KERNEL);
	if (!ipi_buf || !ipi_buf_da) {
		dev_err(dev, "failed to allocate ipi share memory\n");
		return -ENOMEM;
	}

	dev_dbg(dev, "%s: ipi_buf=%p, ipi_buf_da=%pad\n", __func__, ipi_buf, &ipi_buf_da);

	apu->recv_buf = ipi_buf;
	apu->recv_buf_da = ipi_buf_da;
	apu->send_buf = ipi_buf + sizeof(struct mtk_share_obj);
	apu->send_buf_da = ipi_buf_da + sizeof(struct mtk_share_obj);

	ipi_config->in_buf_da = apu->send_buf_da;
	ipi_config->out_buf_da = apu->recv_buf_da;

	return 0;
}

void mtk_apu_ipi_remove(struct mtk_apu *apu)
{
	if (apu->hw_logger_data != NULL)
		mtk_apu_hw_logger_unset_ipi_send(apu->hw_logger_data);
	if (!IS_ERR(apu->ch))
		mbox_free_channel(apu->ch);
	mtk_apu_remove_rpmsg_subdev(apu);
	mtk_apu_ipi_unregister(apu, MTK_APU_IPI_INIT);
}

int mtk_apu_ipi_init(struct platform_device *pdev, struct mtk_apu *apu)
{
	struct device *dev = apu->dev;
	struct mbox_client *cl;
	int i, ret;

	tx_serial_no = 0;
	rx_serial_no = 0;

	mutex_init(&apu->send_lock);
	spin_lock_init(&apu->usage_cnt_lock);

	g_apu = apu;

	for (i = 0; i < MTK_APU_IPI_MAX; i++) {
		mutex_init(&apu->ipi_desc[i].lock);
		lockdep_set_class_and_name(&apu->ipi_desc[i].lock, &ipi_lock_key[i],
					   ipi_attrs[i].name);
	}

	init_waitqueue_head(&apu->run.wq);
	init_waitqueue_head(&apu->ack_wq);

	/* APU initialization IPI register */
	ret = mtk_apu_ipi_register(apu, MTK_APU_IPI_INIT, mtk_apu_init_ipi_handler, apu);
	if (ret) {
		dev_info(dev, "failed to register ipi for init, ret=%d\n", ret);
		return ret;
	}

	/* add rpmsg subdev */
	mtk_apu_add_rpmsg_subdev(apu);

	cl = &apu->cl;
	cl->dev = dev;
	cl->tx_block = true;
	cl->tx_tout = 1000; /* timeout 1000ms */
	cl->knows_txdone = false;
	cl->rx_callback = mtk_apu_ipi_handle_rx;
	cl->rx_callback_bh = mtk_apu_ipi_bottom_handle;
	cl->tx_done = mtk_apu_ipi_tx_done;

	apu->ch = mbox_request_channel(cl, 0);

	if (IS_ERR(apu->ch)) {
		ret = PTR_ERR(apu->ch);
		dev_err_probe(dev, ret, "Failed to request mbox chan: %d\n", ret);
		goto remove_rpmsg_subdev;
	}

	if (apu->hw_logger_data != NULL)
		mtk_apu_hw_logger_set_ipi_send(apu->hw_logger_data, apu, &mtk_apu_ipi_send);

	return 0;

remove_rpmsg_subdev:
	if (!IS_ERR(apu->ch))
		mbox_free_channel(apu->ch);
	mtk_apu_remove_rpmsg_subdev(apu);
	mtk_apu_ipi_unregister(apu, MTK_APU_IPI_INIT);

	return ret;
}
