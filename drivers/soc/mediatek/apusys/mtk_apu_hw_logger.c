// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <asm/div64.h>
#include <asm/io.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/syscalls.h>

#include <linux/remoteproc/mtk_apu.h>
#include <linux/remoteproc/mtk_apu_config.h>
#include <linux/mailbox/mtk-apu-mailbox.h>
#include <linux/soc/mediatek/mtk_apu_hw_logger.h>

MODULE_IMPORT_NS(MTK_APU_MAILBOX);

#define APU_LOGTOP_BASE			(hw_logger_data->apu_logtop)
#define APU_LOGTOP_CON			(APU_LOGTOP_BASE + 0x0)
#define APU_LOG_BUF_ST_ADDR		(APU_LOGTOP_BASE + 0x74)
#define APU_LOG_BUF_T_SIZE		(APU_LOGTOP_BASE + 0x78)
#define APU_LOG_BUF_W_PTR		(APU_LOGTOP_BASE + 0x80)
#define APU_LOG_BUF_R_PTR		(APU_LOGTOP_BASE + 0x84)
#define APU_LOGTOP_CON_FLAG_ADDR	(APU_LOGTOP_CON)
#define APU_LOGTOP_CON_FLAG_SHIFT	(8)
#define APU_LOGTOP_CON_FLAG_MASK	(0xF << APU_LOGTOP_CON_FLAG_SHIFT)
#define APU_LOGTOP_CON_ST_ADDR_HI_ADDR	(APU_LOGTOP_CON)
#define APU_LOGTOP_CON_ST_ADDR_HI_SHIFT	(4)
#define APU_LOGTOP_CON_ST_ADDR_HI_MASK	(0x3 << APU_LOGTOP_CON_ST_ADDR_HI_SHIFT)

/* bit[11:8] */
#define LBC_FULL_FLAG	BIT(0)
#define LBC_ERR_FLAG	BIT(1)
#define OVWRITE_FLAG	BIT(2)
#define LOCKBUS_FLAG	BIT(3)

#define GET_MASK_BITS(x) ((ioread32(x##_ADDR) & x##_MASK) >> x##_SHIFT)
#define SET_MASK_BITS(x, y) \
	iowrite32(((ioread32(y##_ADDR) & ~y##_MASK) | ((x << y##_SHIFT) & y##_MASK)), y##_ADDR)

#define APUSYS_HWLOGR_DIR	"apusys_logger"
#define HWLOG_DEV_NAME		"apu_hw_logger"

#define HWLOG_LINE_MAX_LENS	128

#define HWLOG_LOG_SIZE		(1024 * 1024)
#define LOCAL_LOG_SIZE		(1024 * 1024)

#define LOG_W_OFS		(0x40)
#define LOG_ST_ADDR		(0x44)
#define LOG_T_SIZE		(0x4c)

struct buf_tracking {
	unsigned int __loc_log_w_ofs;
	unsigned int __loc_log_ov_flg;
	unsigned int __loc_log_sz;
	unsigned int __hw_log_r_ofs;
};

struct hw_log_buffer {
	char *hw_log_buf;
	dma_addr_t hw_log_buf_dma_addr;
};

struct hw_log_level_data {
	unsigned int level;
};

struct hw_log_wq {
	struct workqueue_struct *apusys_hwlog_wq;
	struct work_struct apusys_hwlog_task;
	wait_queue_head_t apusys_hwlog_wait;
	unsigned int w_ofs;
	unsigned int r_ofs;
	unsigned int t_size;
};

struct hw_log_seq_data {
	unsigned int w_ptr;
	unsigned int r_ptr;
	unsigned int ov_flg;
	unsigned int not_rd_sz;
	bool nonblock;
};

struct global_log_data {
	struct hw_log_seq_data norm_mode;		/* for debugfs normal dump */
	struct hw_log_seq_data lock_mode;		/* for debugfs lock dump */
	struct hw_log_seq_data mobile_lock_mode;	/* for debugfs mobile logger lock dump */
};

struct mtk_apu_hw_logger {
	/* dev */
	struct device *dev;
	void __iomem *apu_logtop;
	atomic_t apu_toplog_deep_idle;
	struct buf_tracking buf_track;
	struct global_log_data g_log;
	struct hw_log_wq wq_data;
	struct hw_log_buffer hw_log_buf;
	char *local_log_buf;
	struct mtk_apu *apu;
	struct dentry *log_root;
	struct hw_log_level_data hw_ipi_loglv_data;
	int (*apu_ipi_send_ptr)(struct mtk_apu*, u32, void*, u32, u32);
	struct mutex mutex;		/* for buffer copying */
	struct spinlock spinlock;	/* for accessing hw_logger members*/
};

static void hw_logger_buf_invalidate(struct mtk_apu_hw_logger *hw_logger_data)
{
	dma_sync_single_for_cpu(
		hw_logger_data->dev, hw_logger_data->hw_log_buf.hw_log_buf_dma_addr,
		HWLOG_LOG_SIZE, DMA_FROM_DEVICE);
}

static int hw_logger_buf_alloc(struct mtk_apu_hw_logger *hw_logger_data)
{
	int ret = 0;

	ret = dma_set_coherent_mask(hw_logger_data->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err(hw_logger_data->dev, "dma_set_coherent_mask fail (%d)\n", ret);
		ret = -ENOMEM;
		goto out;
	}

	/* local buffer for dump */
	hw_logger_data->local_log_buf = kzalloc(LOCAL_LOG_SIZE, GFP_KERNEL);
	if (!hw_logger_data->local_log_buf) {
		ret = -ENOMEM;
		goto out;
	}

	/* memory used by hw logger */
	hw_logger_data->hw_log_buf.hw_log_buf = kzalloc(HWLOG_LOG_SIZE, GFP_KERNEL);
	if (!hw_logger_data->hw_log_buf.hw_log_buf) {
		ret = -ENOMEM;
		goto out;
	}

	hw_logger_data->hw_log_buf.hw_log_buf_dma_addr = dma_map_single(hw_logger_data->dev,
		hw_logger_data->hw_log_buf.hw_log_buf, HWLOG_LOG_SIZE, DMA_FROM_DEVICE);
	ret = dma_mapping_error(hw_logger_data->dev, hw_logger_data->hw_log_buf.hw_log_buf_dma_addr);
	if (ret) {
		dev_err(hw_logger_data->dev, "dma_map_single fail for hw_log_buf_dma_addr (%d)\n", ret);
		ret = -ENOMEM;
		goto out;
	}

	dev_dbg(hw_logger_data->dev, "local_log_buf = %p\n", hw_logger_data->local_log_buf);
	dev_dbg(hw_logger_data->dev, "hw_log_buf = %pK, hw_log_buf_dma_addr = %pad\n",
		hw_logger_data->hw_log_buf.hw_log_buf,
		&hw_logger_data->hw_log_buf.hw_log_buf_dma_addr);

out:
	return ret;
}

static void hw_logger_buf_free(struct device *dev, struct mtk_apu_hw_logger *hw_logger_data)
{
	dma_unmap_single(dev, hw_logger_data->hw_log_buf.hw_log_buf_dma_addr, HWLOG_LOG_SIZE,
			 DMA_FROM_DEVICE);

	kfree(hw_logger_data->hw_log_buf.hw_log_buf);
	hw_logger_data->hw_log_buf.hw_log_buf = NULL;
	kfree(hw_logger_data->local_log_buf);
	hw_logger_data->local_log_buf = NULL;
}

static unsigned long long get_st_addr(struct mtk_apu_hw_logger *hw_logger_data)
{
	unsigned long long st_addr;

	st_addr = (unsigned long long) GET_MASK_BITS(APU_LOGTOP_CON_ST_ADDR_HI) << 32;
	st_addr = st_addr | ioread32(APU_LOG_BUF_ST_ADDR);

	return st_addr;
}

static unsigned int get_t_size(struct mtk_apu_hw_logger *hw_logger_data)
{
	return ioread32(APU_LOG_BUF_T_SIZE);
}

static unsigned long long get_w_ptr(struct mtk_apu_hw_logger *hw_logger_data)
{
	unsigned long long st_addr, w_ptr;
	unsigned int t_size;
	static bool err_log;

	st_addr = get_st_addr(hw_logger_data);
	t_size = get_t_size(hw_logger_data);

	/* sanity check */
	if (st_addr == 0 || t_size == 0) {
		/*
		 * return 0 here, it will not pass
		 * sanity check in __apu_logtop_copy_buf()
		 */
		/* only print the first error */
		if (!err_log)
			dev_warn(hw_logger_data->dev, "st_addr = 0x%llx, t_size = 0x%x\n", st_addr, t_size);
		err_log = true;
		return 0;
	}

	unsigned long long tmp = (2ULL << 34) + ((unsigned long long)ioread32(APU_LOG_BUF_W_PTR) << 2) - st_addr;
	w_ptr = do_div(tmp, t_size) + st_addr;

	/* print when back to normal */
	if (err_log) {
		dev_info(hw_logger_data->dev, "[ok] w_ptr = 0x%llx, st_addr = 0x%llx, t_size = 0x%x\n",
			w_ptr, st_addr, t_size);
		err_log = false;
	}

	return w_ptr;
}

static unsigned long long get_r_ptr(struct mtk_apu_hw_logger *hw_logger_data)
{
	return (unsigned long long)hw_logger_data->buf_track.__hw_log_r_ofs << 2;
}

static void set_r_ptr(unsigned long long r_ptr, struct mtk_apu_hw_logger *hw_logger_data)
{
	hw_logger_data->buf_track.__hw_log_r_ofs = lower_32_bits(r_ptr >> 2);
}

static struct platform_driver hw_logger_driver;
static int match(struct device *dev, const void *data)
{
	const struct device_node *node = (const struct device_node *)data;
	return dev->of_node == node && dev->driver == &hw_logger_driver.driver;
}

struct mtk_apu_hw_logger* get_mtk_apu_hw_logger_device(struct device_node *hw_logger_np)
{
	struct device *dev;
	struct mtk_apu_hw_logger *hw_logger_data;

	dev = bus_find_device(&platform_bus_type, NULL, hw_logger_np, match);
	if (!dev) {
		pr_err("hw_logger device not found\n");
		return NULL;
	}

	hw_logger_data = dev_get_drvdata(dev);
	put_device(dev);
	if (!hw_logger_data) {
		dev_err(dev, "hw_logger data not found\n");
		return NULL;
	}

	return hw_logger_data;
}
EXPORT_SYMBOL_NS(get_mtk_apu_hw_logger_device, MTK_APU_HW_LOGGER);

void mtk_apu_hw_log_level_ipi_handler(void *data, unsigned int len, void *priv)
{
	unsigned int log_level = *(unsigned int *)data;

	pr_info("log_level = 0x%x (%d)\n", log_level, len);
}
EXPORT_SYMBOL_NS(mtk_apu_hw_log_level_ipi_handler, MTK_APU_HW_LOGGER);

dma_addr_t mtk_apu_hw_logger_config_init(struct mtk_apu_hw_logger *hw_logger_data)
{
	unsigned long flags;

	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	hw_logger_data->g_log.norm_mode.w_ptr = 0;
	hw_logger_data->g_log.norm_mode.r_ptr = U32_MAX;
	hw_logger_data->g_log.norm_mode.ov_flg = 0;
	hw_logger_data->g_log.lock_mode.w_ptr = 0;
	hw_logger_data->g_log.lock_mode.r_ptr = U32_MAX;
	hw_logger_data->g_log.lock_mode.ov_flg = 0;
	hw_logger_data->g_log.mobile_lock_mode.w_ptr = 0;
	hw_logger_data->g_log.mobile_lock_mode.r_ptr = U32_MAX;
	hw_logger_data->g_log.mobile_lock_mode.ov_flg = 0;
	hw_logger_data->buf_track.__loc_log_sz = 0;
	atomic_set(&hw_logger_data->apu_toplog_deep_idle, 0);
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	return hw_logger_data->hw_log_buf.hw_log_buf_dma_addr;
}
EXPORT_SYMBOL_NS(mtk_apu_hw_logger_config_init, MTK_APU_HW_LOGGER);

static int __apu_logtop_copy_buf(unsigned int w_ofs, unsigned int r_ofs, unsigned int t_size,
	unsigned int st_addr, struct mtk_apu_hw_logger *hw_logger_data)
{
	unsigned long flags;
	unsigned int r_size;
	unsigned int log_w_ofs, log_ov_flg;
	int ret = 0;
	static bool err_log;

	if (!hw_logger_data->apu_logtop || !hw_logger_data->hw_log_buf.hw_log_buf
	    || !hw_logger_data->local_log_buf)
		return 0;

	dev_dbg(hw_logger_data->dev, "w_ofs = 0x%x, r_ofs = 0x%x, t_size = 0x%x\n", w_ofs, r_ofs, t_size);

	if (w_ofs == r_ofs)
		goto out;

	/* check copy size */
	if (w_ofs > r_ofs)
		r_size = w_ofs - r_ofs;
	else
		r_size = t_size - r_ofs + w_ofs;

	/* sanity check */
	if (r_size >= t_size)
		r_size = t_size;

	ret = r_size;

	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	log_w_ofs = hw_logger_data->buf_track.__loc_log_w_ofs;
	log_ov_flg = hw_logger_data->buf_track.__loc_log_ov_flg;
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	if (w_ofs + HWLOG_LINE_MAX_LENS > t_size ||
	    r_ofs + HWLOG_LINE_MAX_LENS > t_size ||
	    t_size == 0 || t_size > HWLOG_LOG_SIZE) {
		/* only print the first error */
		if (!err_log)
			dev_warn(hw_logger_data->dev, "w_ofs = 0x%x, r_ofs = 0x%x, t_size = 0x%x\n",
				 w_ofs, r_ofs, t_size);
		err_log = true;
		return 0;
	}

	if (log_w_ofs + HWLOG_LINE_MAX_LENS > LOCAL_LOG_SIZE) {
		/* only print the first error */
		if (!err_log)
			dev_warn(hw_logger_data->dev, "log_w_ofs = 0x%x\n", log_w_ofs);
		err_log = true;
		return 0;
	}

	/* print when back to normal */
	if (err_log) {
		dev_info(hw_logger_data->dev, "[ok] w_ofs = 0x%x, r_ofs = 0x%x, t_size = 0x%x\n",
			 w_ofs, r_ofs, t_size);
		err_log = false;
	}

	/* invalidate hw logger buf */
	hw_logger_buf_invalidate(hw_logger_data);

	while (r_size > 0) {
		/* copy hw logger buffer to local buffer */
		memcpy(hw_logger_data->local_log_buf + log_w_ofs,
			hw_logger_data->hw_log_buf.hw_log_buf + r_ofs, HWLOG_LINE_MAX_LENS);

		/* move local write pointer */
		log_w_ofs = (log_w_ofs + HWLOG_LINE_MAX_LENS) % LOCAL_LOG_SIZE;

		/* move hw logger read pointer */
		r_ofs = (r_ofs + HWLOG_LINE_MAX_LENS) % t_size;
		r_size -= HWLOG_LINE_MAX_LENS;

		hw_logger_data->buf_track.__loc_log_sz += HWLOG_LINE_MAX_LENS;
		if (hw_logger_data->buf_track.__loc_log_sz >= LOCAL_LOG_SIZE)
			log_ov_flg = 1;
	};

	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	hw_logger_data->buf_track.__loc_log_w_ofs = log_w_ofs;
	hw_logger_data->buf_track.__loc_log_ov_flg = log_ov_flg;
	hw_logger_data->g_log.lock_mode.not_rd_sz += ret;
	hw_logger_data->g_log.mobile_lock_mode.not_rd_sz += ret;
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	/* set read pointer */
	set_r_ptr(get_st_addr(hw_logger_data) + r_ofs, hw_logger_data);

	/*
	 * restore local read pointer if power down
	 */
	if (get_st_addr(hw_logger_data) == 0)
		set_r_ptr(((unsigned long long)st_addr << 2) + r_ofs, hw_logger_data);
out:
	return ret;
}

static void __get_r_w_sz(unsigned int *w_ofs, unsigned int *r_ofs, unsigned int *t_size,
			 unsigned int *st_addr, struct mtk_apu_hw_logger *hw_logger_data)
{
	dev_dbg(hw_logger_data->dev, "get_w_ptr() = 0x%llx, get_r_ptr() = 0x%llx\n",
		get_w_ptr(hw_logger_data), get_r_ptr(hw_logger_data));
	dev_dbg(hw_logger_data->dev, "get_st_addr() = 0x%llx, get_t_size() = 0x%x\n",
		get_st_addr(hw_logger_data), get_t_size(hw_logger_data));

	/*
	 * r_ptr may clear in deep idle
	 * read again if it had been clear
	 */
	if (get_r_ptr(hw_logger_data) == 0)
		set_r_ptr(get_st_addr(hw_logger_data), hw_logger_data);

	/* offset,size is only 32bit width */
	*w_ofs = get_w_ptr(hw_logger_data) - get_st_addr(hw_logger_data);
	*r_ofs = get_r_ptr(hw_logger_data) - get_st_addr(hw_logger_data);
	*t_size = get_t_size(hw_logger_data);
	if (st_addr)
		*st_addr = (get_st_addr(hw_logger_data) >> 2);
}

static void __get_r_w_sz_mbox(unsigned int *w_ofs, unsigned int *r_ofs, unsigned int *t_size,
			      unsigned int *st_addr, struct mtk_apu_hw_logger *hw_logger_data)
{
	unsigned long long l_st_addr;
	unsigned int l_st_ofs;

	l_st_ofs = mtk_apu_mbox_read(LOG_ST_ADDR);
	l_st_addr = (unsigned long long)l_st_ofs << 2;

	/* offset,size is only 32bit width */
	*w_ofs = mtk_apu_mbox_read(LOG_W_OFS);
	*r_ofs = get_r_ptr(hw_logger_data) - l_st_addr;
	*t_size = mtk_apu_mbox_read(LOG_T_SIZE);
	if (st_addr)
		*st_addr = (get_st_addr(hw_logger_data) >> 2);

	dev_dbg(hw_logger_data->dev, "ST_ADDR() = 0x%x, W_OFS = 0x%x, T_SIZE = 0x%x\n",
		l_st_ofs, *w_ofs, *t_size);
}

static int apu_logtop_copy_buf(struct mtk_apu_hw_logger *hw_logger_data)
{
	unsigned int w_ofs, r_ofs, t_size, st_addr;
	int ret = 0;
	static bool lock_fail;

	if (!mutex_trylock(&hw_logger_data->mutex)) {
		if (!lock_fail) {
			dev_dbg(hw_logger_data->dev, "lock fail\n");
			lock_fail = true;
		}
		return ret;
	} else if (lock_fail) {
		dev_dbg(hw_logger_data->dev, "lock success\n");
		lock_fail = false;
	}

	__get_r_w_sz(&w_ofs, &r_ofs, &t_size, &st_addr, hw_logger_data);

	/*
	 * Check again here. If deep idle is
	 * entered, the w_ofs, r_ofs, t_size
	 * can not be trusted.
	 */
	if (atomic_read(&hw_logger_data->apu_toplog_deep_idle)) {
		dev_info(hw_logger_data->dev, "in deep idle skip copy");
		goto out;
	}

	ret = __apu_logtop_copy_buf(w_ofs, r_ofs, t_size, st_addr, hw_logger_data);
out:
	mutex_unlock(&hw_logger_data->mutex);

	return ret;
}

static void apu_logtop_copy_buf_wq(struct work_struct *work)
{
	struct hw_log_wq *wq_data = container_of(work, struct hw_log_wq, apusys_hwlog_task);
	struct mtk_apu_hw_logger *hw_logger_data =
		container_of(wq_data, struct mtk_apu_hw_logger, wq_data);

	dev_dbg(hw_logger_data->dev, "in\n");

	mutex_lock(&hw_logger_data->mutex);

	__get_r_w_sz_mbox(&wq_data->w_ofs, &wq_data->r_ofs, &wq_data->t_size, NULL, hw_logger_data);

	__apu_logtop_copy_buf(wq_data->w_ofs, wq_data->r_ofs, wq_data->t_size, 0, hw_logger_data);

	/* force set 0 here to prevent racing between power up */
	set_r_ptr(0, hw_logger_data);

	mutex_unlock(&hw_logger_data->mutex);

	dev_dbg(hw_logger_data->dev, "out\n");
}

static int hw_logger_copy_buf(struct mtk_apu_hw_logger *hw_logger_data)
{
	int ret = 0;

	dev_dbg(hw_logger_data->dev, "in\n");

	if (atomic_read(&hw_logger_data->apu_toplog_deep_idle))
		goto out;

	ret = apu_logtop_copy_buf(hw_logger_data);

out:
	/* return copied size */
	return ret;
}

int mtk_apu_hw_logger_deep_idle_enter_pre(struct mtk_apu_hw_logger *hw_logger_data)
{
	dev_dbg(hw_logger_data->dev, "in\n");

	atomic_set(&hw_logger_data->apu_toplog_deep_idle, 1);

	__get_r_w_sz(&hw_logger_data->wq_data.w_ofs, &hw_logger_data->wq_data.r_ofs,
		     &hw_logger_data->wq_data.t_size, NULL, hw_logger_data);

	return 0;
}
EXPORT_SYMBOL_NS(mtk_apu_hw_logger_deep_idle_enter_pre, MTK_APU_HW_LOGGER);

int mtk_apu_hw_logger_deep_idle_enter_post(struct mtk_apu_hw_logger *hw_logger_data)
{
	dev_dbg(hw_logger_data->dev, "in\n");

	queue_work(hw_logger_data->wq_data.apusys_hwlog_wq,
		   &hw_logger_data->wq_data.apusys_hwlog_task);

	return 0;
}
EXPORT_SYMBOL_NS(mtk_apu_hw_logger_deep_idle_enter_post, MTK_APU_HW_LOGGER);

int mtk_apu_hw_logger_deep_idle_leave(struct mtk_apu_hw_logger *hw_logger_data)
{
	dev_dbg(hw_logger_data->dev, "in\n");

	/* clear read pointer */
	set_r_ptr(get_st_addr(hw_logger_data), hw_logger_data);

	atomic_set(&hw_logger_data->apu_toplog_deep_idle, 0);

	return 0;
}
EXPORT_SYMBOL_NS(mtk_apu_hw_logger_deep_idle_leave, MTK_APU_HW_LOGGER);

void mtk_apu_hw_logger_set_ipi_send(struct mtk_apu_hw_logger *hw_logger_data, struct mtk_apu *apu,
				    int (*func)(struct mtk_apu*, u32, void*, u32, u32))
{
	hw_logger_data->apu = apu;
	hw_logger_data->apu_ipi_send_ptr = func;
}
EXPORT_SYMBOL_NS(mtk_apu_hw_logger_set_ipi_send, MTK_APU_HW_LOGGER);

void mtk_apu_hw_logger_unset_ipi_send(struct mtk_apu_hw_logger *hw_logger_data)
{
	hw_logger_data->apu = NULL;
	hw_logger_data->apu_ipi_send_ptr = NULL;
}
EXPORT_SYMBOL_NS(mtk_apu_hw_logger_unset_ipi_send, MTK_APU_HW_LOGGER);

static int mtk_apu_get_dbglv(void *data, u64 *val)
{
	struct mtk_apu_hw_logger *hw_logger_data = data;

	*val = hw_logger_data->hw_ipi_loglv_data.level;

	return 0;
}

static int mtk_apu_set_dbglv(void *data, u64 val)
{
	int ret;
	struct mtk_apu_hw_logger *hw_logger_data = data;

	if (!hw_logger_data->apu_ipi_send_ptr) {
		dev_err(hw_logger_data->dev, "apu_ipi_send is not registered\n");
		return -EINVAL;
	}

	dev_dbg(hw_logger_data->dev, "set uP debug lv = 0x%llx\n", val);

	hw_logger_data->hw_ipi_loglv_data.level = val;

	ret = hw_logger_data->apu_ipi_send_ptr(hw_logger_data->apu, MTK_APU_IPI_LOG_LEVEL,
			&hw_logger_data->hw_ipi_loglv_data,
			sizeof(hw_logger_data->hw_ipi_loglv_data),
			1000);
	if (ret)
		dev_err(hw_logger_data->dev, "Failed for hw_logger log level send.\n");

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(mtk_apu_hwlogger_dbglv_fops, mtk_apu_get_dbglv, mtk_apu_set_dbglv,
	"%llu\n");

static int mtk_apu_hwlogger_attr_show(struct seq_file *s, void *data)
{
	unsigned long flags;
	struct mtk_apu_hw_logger *hw_logger_data = s->private;

	seq_printf(s, "hw_log_buf = %pK\n", hw_logger_data->hw_log_buf.hw_log_buf);

	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	seq_printf(s, "__loc_log_w_ofs = %d\n", hw_logger_data->buf_track.__loc_log_w_ofs);
	seq_printf(s, "__loc_log_ov_flg = %d\n", hw_logger_data->buf_track.__loc_log_ov_flg);
	seq_printf(s, "__loc_log_sz = %d\n", hw_logger_data->buf_track.__loc_log_sz);
	seq_printf(s, "norm_log.w_ptr = %d\n", hw_logger_data->g_log.norm_mode.w_ptr);
	seq_printf(s, "norm_log.r_ptr = %d\n", hw_logger_data->g_log.norm_mode.r_ptr);
	seq_printf(s, "norm_log.ov_flg = %d\n", hw_logger_data->g_log.norm_mode.ov_flg);
	seq_printf(s, "lock_log.w_ptr = %d\n", hw_logger_data->g_log.lock_mode.w_ptr);
	seq_printf(s, "lock_log.r_ptr = %d\n", hw_logger_data->g_log.lock_mode.r_ptr);
	seq_printf(s, "lock_log.ov_flg = %d\n",	hw_logger_data->g_log.lock_mode.ov_flg);
	seq_printf(s, "lock_log.not_rd_sz = %d\n", hw_logger_data->g_log.lock_mode.not_rd_sz);
	seq_printf(s, "mobile_lock_log.w_ptr = %d\n", hw_logger_data->g_log.mobile_lock_mode.w_ptr);
	seq_printf(s, "mobile_lock_log.r_ptr = %d\n", hw_logger_data->g_log.mobile_lock_mode.r_ptr);
	seq_printf(s, "mobile_lock_log.ov_flg = %d\n", hw_logger_data->g_log.mobile_lock_mode.ov_flg);
	seq_printf(s, "mobile_lock_log.not_rd_sz = %d\n", hw_logger_data->g_log.mobile_lock_mode.not_rd_sz);

	seq_printf(s, "mobile_lock_log.not_rd_sz = %d\n", hw_logger_data->g_log.mobile_lock_mode.not_rd_sz);

	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mtk_apu_hwlogger_attr);

static void *mtk_apu_hw_logger_seq_start(struct seq_file *s, loff_t *pos)
{
	struct hw_log_seq_data *pSData;
	unsigned int w_ptr, r_ptr, ov_flg = 0;
	unsigned long flags;
	struct mtk_apu_hw_logger *hw_logger_data = s->private;

	hw_logger_copy_buf(hw_logger_data);

	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	if (hw_logger_data->g_log.norm_mode.r_ptr == U32_MAX) {
		w_ptr = hw_logger_data->buf_track.__loc_log_w_ofs;
		ov_flg = hw_logger_data->buf_track.__loc_log_ov_flg;
		hw_logger_data->g_log.norm_mode.w_ptr = w_ptr;
		hw_logger_data->g_log.norm_mode.ov_flg = ov_flg;
		if (ov_flg)
			/* avoid stuck at while loop move one forward */
			r_ptr = (w_ptr + HWLOG_LINE_MAX_LENS) % HWLOG_LOG_SIZE;
		else
			r_ptr = 0;
		hw_logger_data->g_log.norm_mode.r_ptr = r_ptr;
	} else {
		w_ptr = hw_logger_data->g_log.norm_mode.w_ptr;
		r_ptr = hw_logger_data->g_log.norm_mode.r_ptr;
	}
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	dev_dbg(hw_logger_data->dev, "w_ptr = %d, r_ptr = %d, ov_flg = %d, *pos = %d\n",
		w_ptr, r_ptr, ov_flg, (unsigned int)*pos);

	if (w_ptr == r_ptr) {
		spin_lock_irqsave(&hw_logger_data->spinlock, flags);
		hw_logger_data->g_log.norm_mode.r_ptr = U32_MAX;
		spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);
		return NULL;
	}

	pSData = kzalloc(sizeof(struct hw_log_seq_data), GFP_KERNEL);
	if (!pSData)
		return NULL;

	pSData->w_ptr = w_ptr;
	pSData->ov_flg = ov_flg;
	if (ov_flg == 0)
		pSData->r_ptr = r_ptr;
	else
		pSData->r_ptr = w_ptr;

	return pSData;
}

static void *mtk_apu_hw_logger_seq_startl(struct seq_file *s, loff_t *pos)
{
	uint32_t w_ptr, r_ptr, ov_flg;
	struct hw_log_seq_data *pSData, *gpSData;
	unsigned long flags;
	struct mtk_apu_hw_logger *hw_logger_data = s->private;

	pSData = kzalloc(sizeof(struct hw_log_seq_data), GFP_KERNEL);
	if (!pSData)
		return NULL;

	if (s->file && s->file->f_flags & O_NONBLOCK) {
		pSData->nonblock = true;
		gpSData = &hw_logger_data->g_log.mobile_lock_mode;
	} else {
		pSData->nonblock = false;
		gpSData = &hw_logger_data->g_log.lock_mode;
	}

	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	w_ptr = hw_logger_data->buf_track.__loc_log_w_ofs;
	gpSData->w_ptr = w_ptr;
	if (gpSData->r_ptr == U32_MAX) {
		ov_flg = hw_logger_data->buf_track.__loc_log_ov_flg;
		gpSData->ov_flg = ov_flg;
		if (ov_flg)
			/* avoid stuck at while loop move one forward */
			r_ptr = (w_ptr + HWLOG_LINE_MAX_LENS) % HWLOG_LOG_SIZE;
		else
			r_ptr = 0;
		gpSData->r_ptr = r_ptr;
	} else {
		r_ptr = gpSData->r_ptr;
		/* check if overflow occurs */
		if (gpSData->not_rd_sz >= LOCAL_LOG_SIZE - HWLOG_LINE_MAX_LENS) {
			r_ptr = (w_ptr + HWLOG_LINE_MAX_LENS) % HWLOG_LOG_SIZE;
			gpSData->not_rd_sz = LOCAL_LOG_SIZE - HWLOG_LINE_MAX_LENS;
		}
	}
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	/* for ctrl-c to force exit the loop */
	do {
		/* force flush last hw buffer */
		hw_logger_copy_buf(hw_logger_data);

		spin_lock_irqsave(&hw_logger_data->spinlock, flags);
		w_ptr = hw_logger_data->buf_track.__loc_log_w_ofs;
		ov_flg = hw_logger_data->buf_track.__loc_log_ov_flg;
		spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

		if (w_ptr != r_ptr)
			break;
		 /* return if file is open as non blocking mode */
		else if (pSData->nonblock) {
			/* free here and return NULL, seq will stop */
			kfree(pSData);
			dev_dbg(hw_logger_data->dev, "END\n");
			return NULL;
		}
		usleep_range(10000, 15000);
	} while (!signal_pending(current) && w_ptr == r_ptr);

	dev_dbg(hw_logger_data->dev, "w_ptr = %d, r_ptr = %d, ov_flg = %d, *pos = %d\n",
		  w_ptr, r_ptr, ov_flg, (unsigned int)*pos);

	pSData->w_ptr = w_ptr;
	pSData->r_ptr = r_ptr;
	pSData->ov_flg = ov_flg;

	if (signal_pending(current)) {
		dev_dbg(hw_logger_data->dev, "BREAK w_ptr = %d, r_ptr = %d, ov_flg = %d\n",
			w_ptr, r_ptr, ov_flg);
		spin_lock_irqsave(&hw_logger_data->spinlock, flags);
		gpSData->r_ptr = U32_MAX;
		spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);
		/* free here and return NULL, seq will stop */
		kfree(pSData);
		return NULL;
	}

	return pSData;
}

static void *mtk_apu_hw_logger_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct hw_log_seq_data *pSData = v;
	unsigned long flags;
	struct mtk_apu_hw_logger *hw_logger_data = s->private;

	dev_dbg(hw_logger_data->dev,
		"w_ptr = %d, r_ptr = %d, ov_flg = %d, *pos = %d\n",
		pSData->w_ptr, pSData->r_ptr,
		pSData->ov_flg, (unsigned int)*pos);

	pSData->r_ptr = (pSData->r_ptr + HWLOG_LINE_MAX_LENS) % LOCAL_LOG_SIZE;
	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	hw_logger_data->g_log.norm_mode.r_ptr = pSData->r_ptr;
	/* just prevent warning */
	*pos = pSData->r_ptr + 1;
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	if (pSData->r_ptr != pSData->w_ptr)
		return pSData;

	dev_dbg(hw_logger_data->dev,
		"END norm_log.w_ptr = %d, norm_log.r_ptr = %d, lock_log.ov_flg = %d\n",
		hw_logger_data->g_log.norm_mode.w_ptr, hw_logger_data->g_log.norm_mode.r_ptr,
		hw_logger_data->g_log.lock_mode.ov_flg);

	kfree(pSData);
	return NULL;
}

static void *mtk_apu_hw_logger_seq_nextl(struct seq_file *s, void *v, loff_t *pos)
{
	struct hw_log_seq_data *pSData = v, *gpSData;
	unsigned long flags;
	struct mtk_apu_hw_logger *hw_logger_data = s->private;

	if (pSData->nonblock)
		gpSData = &hw_logger_data->g_log.mobile_lock_mode;
	else
		gpSData = &hw_logger_data->g_log.lock_mode;

	dev_dbg(hw_logger_data->dev,
		"w_ptr = %d, r_ptr = %d, ov_flg = %d, nonblock = %d, *pos = %d\n",
		pSData->w_ptr, pSData->r_ptr,
		pSData->ov_flg, pSData->nonblock, (unsigned int)*pos);

	pSData->r_ptr = (pSData->r_ptr + HWLOG_LINE_MAX_LENS) % LOCAL_LOG_SIZE;
	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	gpSData->r_ptr = pSData->r_ptr;
	gpSData->not_rd_sz -= HWLOG_LINE_MAX_LENS;
	/* just prevent warning */
	*pos = pSData->r_ptr + 1;
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	if (pSData->r_ptr != pSData->w_ptr)
		return pSData;

	dev_dbg(hw_logger_data->dev,
		"END gpSData w_ptr = %d, r_ptr = %d, nonblock = %d, ov_flg = %d\n",
		gpSData->w_ptr, gpSData->r_ptr, gpSData->nonblock,
		gpSData->ov_flg);

	kfree(pSData);
	return NULL;
}

static void mtk_apu_hw_logger_seq_stop(struct seq_file *s, void *v)
{
	kfree(v);
}

static int mtk_apu_hw_logger_seq_show(struct seq_file *s, void *v)
{
	struct hw_log_seq_data *pSData = v;
	static unsigned int prevIsBinary;
	struct mtk_apu_hw_logger *hw_logger_data = s->private;

	dev_dbg(hw_logger_data->dev, "(%04d)(%d) %s", pSData->r_ptr,
		(int)strlen(hw_logger_data->local_log_buf + pSData->r_ptr),
		hw_logger_data->local_log_buf + pSData->r_ptr);

	if ((hw_logger_data->local_log_buf[pSData->r_ptr] == 0xA5) &&
		(hw_logger_data->local_log_buf[pSData->r_ptr+1] == 0xA5)) {
		prevIsBinary = 1;
		seq_write(s, hw_logger_data->local_log_buf + pSData->r_ptr, HWLOG_LINE_MAX_LENS);
	} else {
		if (prevIsBinary)
			seq_putc(s, '\n');
		prevIsBinary = 0;
		/*
		 * force null-terminated
		 * prevent overflow from printing non-string content
		 */
		*(hw_logger_data->local_log_buf + pSData->r_ptr + HWLOG_LINE_MAX_LENS - 1) = '\0';
		seq_printf(s, "%s",
			hw_logger_data->local_log_buf + pSData->r_ptr);
	}

	return 0;
}

static unsigned int seq_poll(struct file *file, poll_table *wait)
{
	unsigned int ret = 0;
	struct mtk_apu_hw_logger *hw_logger_data;

	if (!(file->f_mode & FMODE_READ))
		return ret;

	hw_logger_data = ((struct seq_file *)file->private_data)->private;

	/* force flush last hw buffer */
	hw_logger_copy_buf(hw_logger_data);

	poll_wait(file, &hw_logger_data->wq_data.apusys_hwlog_wait, wait);

	if (hw_logger_data->g_log.mobile_lock_mode.r_ptr != hw_logger_data->buf_track.__loc_log_w_ofs)
		ret = POLLIN | POLLRDNORM;

	return ret;
}

static const struct seq_operations mtk_apu_hw_logger_log_sops = {
	.start = mtk_apu_hw_logger_seq_start,
	.next  = mtk_apu_hw_logger_seq_next,
	.stop  = mtk_apu_hw_logger_seq_stop,
	.show  = mtk_apu_hw_logger_seq_show
};

static const struct seq_operations seq_ops_lock = {
	.start = mtk_apu_hw_logger_seq_startl,
	.next  = mtk_apu_hw_logger_seq_nextl,
	.stop  = mtk_apu_hw_logger_seq_stop,
	.show  = mtk_apu_hw_logger_seq_show
};

static int debug_sqopen_lock(struct inode *inode, struct file *file)
{
	int res = seq_open(file, &seq_ops_lock);

	if (!res)
		((struct seq_file *)file->private_data)->private = inode->i_private;
	else
		pr_err("%s: seq_open failed\n", __func__);

	return res;
}

DEFINE_SEQ_ATTRIBUTE(mtk_apu_hw_logger_log);

static const struct file_operations hw_loggerSeqLogL_ops = {
	.owner   = THIS_MODULE,
	.open    = debug_sqopen_lock,
	.poll    = seq_poll,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release
};

static void clear_local_log_buf(struct mtk_apu_hw_logger *hw_logger_data)
{
	unsigned long flags;

	mutex_lock(&hw_logger_data->mutex);

	memset(hw_logger_data->local_log_buf, 0, LOCAL_LOG_SIZE);

	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	hw_logger_data->buf_track.__loc_log_w_ofs = 0;
	hw_logger_data->buf_track.__loc_log_ov_flg = 0;

	hw_logger_data->g_log.norm_mode.w_ptr = 0;
	hw_logger_data->g_log.norm_mode.r_ptr = U32_MAX;
	hw_logger_data->g_log.norm_mode.ov_flg = 0;
	hw_logger_data->g_log.norm_mode.not_rd_sz = 0;
	hw_logger_data->g_log.lock_mode.w_ptr = 0;
	hw_logger_data->g_log.lock_mode.r_ptr = U32_MAX;
	hw_logger_data->g_log.lock_mode.ov_flg = 0;
	hw_logger_data->g_log.lock_mode.not_rd_sz = 0;

	hw_logger_data->g_log.mobile_lock_mode.not_rd_sz = 0;
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	mutex_unlock(&hw_logger_data->mutex);
}

static int mtk_apu_hwlogger_clear_show(struct seq_file *s, void *data)
{
	struct mtk_apu_hw_logger *hw_logger_data = s->private;

	clear_local_log_buf(hw_logger_data);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mtk_apu_hwlogger_clear);

static void hw_logger_remove_debugfs(struct mtk_apu_hw_logger *hw_logger_data)
{
	if (hw_logger_data->log_root)
		debugfs_remove_recursive(hw_logger_data->log_root);
}

static int hw_logger_create_debugfs(struct mtk_apu_hw_logger *hw_logger_data)
{
	struct dentry *log_devinfo;
	struct dentry *log_devattr;
	struct dentry *log_seqlog;
	struct dentry *log_seqlogL;
	struct dentry *log_clear;
	int ret = 0;

	hw_logger_data->log_root = debugfs_create_dir(APUSYS_HWLOGR_DIR, NULL);
	ret = IS_ERR_OR_NULL(hw_logger_data->log_root);
	if (ret) {
		dev_err(hw_logger_data->dev, "create dir fail (%d)\n", ret);
		goto out;
	}

	/* create device table info */
	log_devinfo = debugfs_create_file("log_level", 0440, hw_logger_data->log_root,
					   hw_logger_data, &mtk_apu_hwlogger_dbglv_fops);
	ret = IS_ERR_OR_NULL(log_devinfo);
	if (ret) {
		dev_err(hw_logger_data->dev, "create devinfo fail (%d)\n", ret);
		goto out;
	}

	log_seqlog = debugfs_create_file("seq_log", 0440, hw_logger_data->log_root,
					  hw_logger_data, &mtk_apu_hw_logger_log_fops);
	ret = IS_ERR_OR_NULL(log_seqlog);
	if (ret) {
		dev_err(hw_logger_data->dev, "create seqlog fail (%d)\n", ret);
		goto out;
	}

	log_seqlogL = debugfs_create_file("seq_logl", 0440, hw_logger_data->log_root,
					   hw_logger_data, &hw_loggerSeqLogL_ops);
	ret = IS_ERR_OR_NULL(log_seqlogL);
	if (ret) {
		dev_err(hw_logger_data->dev, "create seqlogL fail (%d)\n", ret);
		goto out;
	}

	log_devattr = debugfs_create_file("state", 0440, hw_logger_data->log_root,
					   hw_logger_data, &mtk_apu_hwlogger_attr_fops);
	ret = IS_ERR_OR_NULL(log_devattr);
	if (ret) {
		dev_err(hw_logger_data->dev, "create attr fail (%d)\n", ret);
		goto out;
	}

	log_clear = debugfs_create_file("clear_log", 0200, hw_logger_data->log_root,
					 hw_logger_data, &mtk_apu_hwlogger_clear_fops);
	ret = IS_ERR_OR_NULL(log_clear);
	if (ret) {
		dev_err(hw_logger_data->dev, "create clear fail (%d)\n", ret);
		goto out;
	}

	return 0;

out:
	hw_logger_remove_debugfs(hw_logger_data);
	return ret;
}

static int hw_logger_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret = 0;
	struct mtk_apu_hw_logger *hw_logger_data;

	dev_dbg(dev, "%s ++\n", __func__);

	hw_logger_data = devm_kzalloc(dev, sizeof(*hw_logger_data), GFP_KERNEL);
	if (!hw_logger_data) {
		dev_err(dev, "Failed to allocate memory for hw_logger_data\n");
		return -ENOMEM;
	}

	hw_logger_data->dev = dev;

	mutex_init(&hw_logger_data->mutex);
	spin_lock_init(&hw_logger_data->spinlock);

	init_waitqueue_head(&hw_logger_data->wq_data.apusys_hwlog_wait);

	hw_logger_data->apu_logtop = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR((void const *)hw_logger_data->apu_logtop)) {
		dev_err(dev, "apu_logtop remap base fail\n");
		ret = -ENOMEM;
		return ret;
	}

	ret = hw_logger_buf_alloc(hw_logger_data);
	if (ret) {
		dev_err(dev, "hw_logger_buf_alloc fail\n");
		goto remove_hw_log_buf;
	}

	ret = hw_logger_create_debugfs(hw_logger_data);
	if (ret) {
		dev_err(dev, "hw_logger_create_debugfs fail\n");
		goto remove_debugfs;
	}

	/* Used for deep idle enter */
	hw_logger_data->wq_data.apusys_hwlog_wq = create_workqueue("apusys_hwlog_wq");
	INIT_WORK(&hw_logger_data->wq_data.apusys_hwlog_task, apu_logtop_copy_buf_wq);

	dev_set_drvdata(dev, hw_logger_data);

	pm_runtime_enable(dev);

	dev_dbg(dev, "%s --\n", __func__);

	return 0;

remove_debugfs:
	hw_logger_remove_debugfs(hw_logger_data);

remove_hw_log_buf:
	hw_logger_buf_free(dev, hw_logger_data);

	return ret;
}

static int hw_logger_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_apu_hw_logger *hw_logger_data = dev_get_drvdata(dev);

	pm_runtime_disable(dev);

	flush_workqueue(hw_logger_data->wq_data.apusys_hwlog_wq);
	destroy_workqueue(hw_logger_data->wq_data.apusys_hwlog_wq);
	hw_logger_remove_debugfs(hw_logger_data);

	hw_logger_buf_free(dev, hw_logger_data);

	return 0;
}

static const struct of_device_id apusys_hw_logger_of_match[] = {
	{ .compatible = "mediatek,apusys-hw-logger"},
	{},
};
MODULE_DEVICE_TABLE(of, apusys_hw_logger_of_match);

static struct platform_driver hw_logger_driver = {
	.probe = hw_logger_probe,
	.remove = hw_logger_remove,
	.driver = {
		.name = HWLOG_DEV_NAME,
		.of_match_table = of_match_ptr(apusys_hw_logger_of_match),
	}
};

module_platform_driver(hw_logger_driver);
MODULE_DESCRIPTION("MediaTek APU Hardware Logger Driver");
MODULE_LICENSE("GPL v2");