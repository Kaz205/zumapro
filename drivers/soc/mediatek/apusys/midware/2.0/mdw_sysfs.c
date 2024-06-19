// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/uaccess.h>

#include "mdw_cmn.h"

static ssize_t reserv_time_remain_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct mdw_device *mdev = dev_get_drvdata(dev);
	uint32_t num;

	/* get dma normal task num */
	num = mdev->dev_funcs->get_info(mdev, MDW_INFO_RESERV_TIME_REMAIN);

	return sprintf(buf, "%u\n", num);
}
static DEVICE_ATTR_RO(reserv_time_remain);

static ssize_t dsp_task_num_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct mdw_device *mdev = dev_get_drvdata(dev);
	uint32_t num;

	/* get dma normal task num */
	num = mdev->dev_funcs->get_info(mdev, MDW_INFO_NORMAL_TASK_DSP);

	return sprintf(buf, "%u\n", num);
}
static DEVICE_ATTR_RO(dsp_task_num);

static ssize_t dla_task_num_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct mdw_device *mdev = dev_get_drvdata(dev);
	uint32_t num;

	/* get dla normal task num */
	num = mdev->dev_funcs->get_info(mdev, MDW_INFO_NORMAL_TASK_DLA);

	return sprintf(buf, "%u\n", num);
}
static DEVICE_ATTR_RO(dla_task_num);

static ssize_t dma_task_num_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct mdw_device *mdev = dev_get_drvdata(dev);
	uint32_t num;

	/* get dma normal task num */
	num = mdev->dev_funcs->get_info(mdev, MDW_INFO_NORMAL_TASK_DMA);

	return sprintf(buf, "%u\n", num);
}
static DEVICE_ATTR_RO(dma_task_num);

static struct attribute *mdw_task_attrs[] = {
	&dev_attr_dsp_task_num.attr,
	&dev_attr_dla_task_num.attr,
	&dev_attr_dma_task_num.attr,
	NULL,
};

static struct attribute_group mdw_devinfo_attr_group = {
	.name	= "queue",
	.attrs	= mdw_task_attrs,
};

static struct attribute *mdw_sched_attrs[] = {
	&dev_attr_reserv_time_remain.attr,
	NULL,
};

static struct attribute_group mdw_sched_attr_group = {
	.name	= "sched",
	.attrs	= mdw_sched_attrs,
};

static ssize_t mem_statistics_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return 0;
}
static DEVICE_ATTR_RO(mem_statistics);

static struct attribute *mdw_mem_attrs[] = {
	&dev_attr_mem_statistics.attr,
	NULL,
};

static struct attribute_group mdw_mem_attr_group = {
	.name	= "memory",
	.attrs	= mdw_mem_attrs,
};

static ssize_t ulog_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct mdw_device *mdev = dev_get_drvdata(dev);
	uint32_t log_lv;

	log_lv = mdev->dev_funcs->get_info(mdev, MDW_INFO_ULOG);

	return sprintf(buf, "%u\n", log_lv);
}

static ssize_t ulog_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdw_device *mdev = dev_get_drvdata(dev);
	uint32_t val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret) {
		dev_err(mdev->dev, "failed to parse ulog value\n");
		return ret;
	} else {
		ret = mdev->dev_funcs->set_param(mdev, MDW_INFO_ULOG, val);
		if (ret) {
			dev_err(mdev->dev, "failed to set ulog value\n");
			return ret;
		}
	}

	return count;
}
static DEVICE_ATTR_RW(ulog);

static ssize_t klog_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct mdw_device *mdev = dev_get_drvdata(dev);
	uint32_t log_lv;

	log_lv = mdev->dev_funcs->get_info(mdev, MDW_INFO_KLOG);
	return sprintf(buf, "%u\n", log_lv);
}

static ssize_t klog_store(struct device *dev,
	struct device_attribute *attr, const char *buf,
	size_t count)
{
	struct mdw_device *mdev = dev_get_drvdata(dev);
	uint32_t val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret) {
		dev_err(mdev->dev, "failed to parse klog value\n");
		return ret;
	} else {
		ret = mdev->dev_funcs->set_param(mdev, MDW_INFO_KLOG, val);
		if (ret) {
			dev_err(mdev->dev, "failed to set klog value\n");
			return ret;
		}
	}

	return count;
}
static DEVICE_ATTR_RW(klog);

static struct attribute *mdw_log_attrs[] = {
	&dev_attr_ulog.attr,
	&dev_attr_klog.attr,
	NULL,
};

static struct attribute_group mdw_log_attr_group = {
	.name	= "log",
	.attrs	= mdw_log_attrs,
};

static const struct attribute_group *mdw_attribute_groups[] = {
	&mdw_devinfo_attr_group,
	&mdw_sched_attr_group,
	&mdw_log_attr_group,
	&mdw_mem_attr_group,
	NULL
};

int mdw_sysfs_init(struct mdw_device *mdev)
{
	int ret;

	/* create in
	 * /sys/devices/platform/soc/19001000.remoteproc/19001000.remoteproc.apu-mdw-rpmsg.4.-1
	 */
	ret = device_add_groups(mdev->dev, mdw_attribute_groups);
	if (ret)
		dev_err(mdev->dev, "[error] create mdw sysfs entries failed: %d\n", ret);

	return ret;
}

void mdw_sysfs_deinit(struct mdw_device *mdev)
{
	device_remove_groups(mdev->dev, mdw_attribute_groups);
}
