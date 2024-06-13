// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/arm-smccc.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#include <linux/remoteproc/mtk_apu.h>
#include <linux/remoteproc/mtk_apu_config.h>
#include <linux/soc/mediatek/mtk_apu_hw_logger.h>
#include <linux/soc/mediatek/mtk_apu_secure.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
#include "mtk_apu_loadimage.h"
#include "mtk_apu_rproc.h"

MODULE_IMPORT_NS(MTK_APU_HW_LOGGER);

#define APU_RUN_WAIT_CNT 3

uint32_t mtk_apu_rv_smc_call(struct device *dev, uint32_t smc_id, uint32_t a2)
{
	struct arm_smccc_res res;

	dev_info(dev, "%s: smc call %d\n", __func__, smc_id);

	arm_smccc_smc(MTK_SIP_APUSYS_CONTROL, smc_id, a2, 0, 0, 0, 0, 0, &res);
	if (((int) res.a0) < 0)
		dev_info(dev, "%s: smc call %d return error(%ld)\n", __func__, smc_id, res.a0);

	return res.a0;
}

static void mtk_apu_dram_boot_remove(struct mtk_apu *apu)
{
	void *domain = iommu_get_domain_for_dev(apu->dev);

	if (apu->platdata->flags.secure_boot && !apu->platdata->flags.map_iova)
		return;

	if (domain != NULL)
		iommu_unmap(domain, MTK_APU_SEC_FW_IOVA, apu->apusys_sec_mem_size);
}

static int mtk_apu_dram_boot_init(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;
	int ret = 0;
	int map_sg_sz = 0;
	void *domain;
	struct sg_table sgt;
	phys_addr_t pa;
	u32 boundary;
	u64 iova;

	if (apu->platdata->flags.secure_boot && !apu->platdata->flags.map_iova)
		return 0;

	if (!apu->platdata->flags.bypass_iommu) {
		domain = iommu_get_domain_for_dev(apu->dev);
		if (domain == NULL) {
			dev_info(dev, "%s: iommu_get_domain_for_dev fail\n", __func__);
			return -ENOMEM;
		}
	}

	if (!apu->platdata->flags.bypass_iommu) {
		apu->code_buf = (void *) apu->apu_sec_mem_base +
				apu->apusys_sec_info->up_code_buf_ofs;

		apu->code_da = MTK_APU_SEC_FW_IOVA;
		/* Map reserved code buffer to MTK_APU_SEC_FW_IOVA */
		ret = iommu_map(domain, MTK_APU_SEC_FW_IOVA, apu->apusys_sec_mem_start,
				apu->apusys_sec_mem_size, IOMMU_READ | IOMMU_WRITE);

		dma_sync_single_for_device(apu->dev, MTK_APU_SEC_FW_IOVA,
					   apu->apusys_sec_mem_size, 0);

		if (ret) {
			dev_info(dev, "%s: iommu_map fail(%d)\n", __func__, ret);
			return ret;
		}

		dev_info(dev, "%s: iommu_map done\n", __func__);
		return ret;
	}

	/* Allocate code buffer */
	apu->code_buf = dma_alloc_coherent(apu->dev, CODE_BUF_SIZE, &apu->code_da, GFP_KERNEL);
	if (apu->code_buf == NULL || apu->code_da == 0) {
		dev_info(dev, "%s: dma_alloc_coherent fail\n", __func__);
		return -ENOMEM;
	}

	boundary = (u32) upper_32_bits(apu->code_da);
	iova = CODE_BUF_DA | ((u64) boundary << 32);
	dev_info(dev, "%s: boundary = %u, iova = 0x%llx\n", __func__, boundary, iova);

	if (!apu->platdata->flags.bypass_iommu) {
		sgt.sgl = NULL;
		/* Convert IOVA to sgtable */
		ret = dma_get_sgtable(apu->dev, &sgt, apu->code_buf, apu->code_da, CODE_BUF_SIZE);
		if (ret < 0 || sgt.sgl == NULL) {
			dev_info(dev, "get sgtable fail\n");
			ret = -EINVAL;
			goto free_code_buf;
		}

		dev_info(dev, "%s: sgt.nents = %d, sgt.orig_nents = %d\n",
			 __func__, sgt.nents, sgt.orig_nents);
		/* Map sg_list to MD32_BOOT_ADDR */
		map_sg_sz = iommu_map_sg(domain, iova, sgt.sgl, sgt.nents, IOMMU_READ|IOMMU_WRITE);
		dev_info(dev, "%s: sgt.nents = %d, sgt.orig_nents = %d\n",
			 __func__, sgt.nents, sgt.orig_nents);
		dev_info(dev, "%s: map_sg_sz = %d\n", __func__, map_sg_sz);
		if (map_sg_sz != CODE_BUF_SIZE)
			dev_info(dev, "%s: iommu_map_sg fail(%d)\n", __func__, ret);

		pa = iommu_iova_to_phys(domain, iova + CODE_BUF_SIZE - SZ_4K);
		dev_info(dev, "%s: pa = 0x%llx\n", __func__, (uint64_t) pa);
		if (!pa) // pa should not be null
			dev_info(dev, "%s: check pa fail(0x%llx)\n", __func__, (uint64_t) pa);
	}

	dev_info(dev, "%s: apu->code_buf = 0x%llx, apu->code_da = 0x%llx\n",
		 __func__, (uint64_t) apu->code_buf, (uint64_t) apu->code_da);

	return ret;

free_code_buf:
	dma_free_coherent(apu->dev, CODE_BUF_SIZE, apu->code_buf, apu->code_da);

	return ret;
}

static void mtk_apu_setup_sec_mem(struct mtk_apu *apu)
{
	int32_t ret;
	struct device *dev = apu->dev;

	if (!apu->platdata->flags.secure_boot)
		return;

	ret = mtk_apu_rv_smc_call(dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_SETUP_SEC_MEM, 0);
	dev_info(dev, "%s: %d\n", __func__, ret);
}

static void *mtk_apu_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem)
{
	void *ptr = NULL;
	struct mtk_apu *apu = (struct mtk_apu *)rproc->priv;

	if (da < DRAM_OFFSET + CODE_BUF_SIZE) {
		ptr = apu->code_buf + (da - DRAM_OFFSET);
		dev_dbg(apu->dev, "%s: (DRAM): da = 0x%llx, len = 0x%zx\n", __func__, da, len);
	} else if (da >= TCM_OFFSET && da < TCM_OFFSET + TCM_SIZE) {
		if (apu->md32_tcm)
			ptr = apu->md32_tcm + (da - TCM_OFFSET);
		dev_dbg(apu->dev, "%s: (TCM): da = 0x%llx, len = 0x%zx\n", __func__, da, len);
	}
	return ptr;
}

static int __mtk_apu_run(struct rproc *rproc)
{
	struct mtk_apu *apu = (struct mtk_apu *)rproc->priv;
	const struct mtk_apu_hw_ops *hw_ops = &apu->platdata->ops;
	struct device *dev = apu->dev;
	struct mtk_apu_run *run = &apu->run;
	struct timespec64 begin, end, delta;
	int wait_cnt = APU_RUN_WAIT_CNT;
	int ret;

	if (!hw_ops->start) {
		WARN_ON(1);
		return -EINVAL;
	}

	hw_ops->power_on(apu);
	hw_ops->start(apu);

	while (wait_cnt > 0) {
		/* check if boot success */
		ktime_get_ts64(&begin);

		ret = wait_event_interruptible_timeout(run->wq, run->signaled, msecs_to_jiffies(10000));

		ktime_get_ts64(&end);

		if (ret == 0) {
			dev_err(dev, "APU initialization time out\n");
			apu->bypass_pwr_off_chk = true;
			goto stop;
		} else if (ret == -ERESTARTSYS) {
			dev_dbg(dev, "wait for APU interrupted by a signal\n");
			wait_cnt -= 1;
		} else {
			break;
		}
	}

	if (wait_cnt == 0) {
		dev_err(dev, "failed to wait APU interrupt\n");
		apu->bypass_pwr_off_chk = true;
		goto stop;
	}

	apu->boot_done = true;
	delta = timespec64_sub(end, begin);
	dev_info(dev,
		 "APU uP boot done. boot time: %llu s, %llu ns. fw_ver: %s\n",
		 (uint64_t)delta.tv_sec, (uint64_t)delta.tv_nsec, run->fw_ver);

	return 0;

stop:
	if (!hw_ops->stop) {
		WARN_ON(1);
		return -EINVAL;
	}
	hw_ops->stop(apu);

	return ret;
}

static int mtk_apu_start(struct rproc *rproc)
{
	struct mtk_apu *apu = (struct mtk_apu *)rproc->priv;
	int ret;

	/* prevent MPU violation when secure_boot is enabled */
	if (!apu->platdata->flags.secure_boot || apu->platdata->flags.map_iova) {
		apu->apusys_sec_info = (struct apusys_secure_info_t *)
						(apu->apu_sec_mem_base +
						apu->platdata->config.up_code_buf_sz);

		dev_dbg(apu->dev, "up_fw_ofs = 0x%x, up_fw_sz = 0x%x\n",
			apu->apusys_sec_info->up_fw_ofs,
			apu->apusys_sec_info->up_fw_sz);

		dev_dbg(apu->dev, "up_xfile_ofs = 0x%x, up_xfile_sz = 0x%x\n",
			apu->apusys_sec_info->up_xfile_ofs,
			apu->apusys_sec_info->up_xfile_sz);
	}

	ret = mtk_apu_config_setup(apu);
	if (ret)
		goto out_mtk_apu_start;

	ret = mtk_apu_dram_boot_init(apu);
	if (ret)
		goto remove_mtk_apu_config_setup;

	mtk_apu_setup_sec_mem(apu);

	dev_info(apu->dev, "%s: try to boot uP\n", __func__);
	ret = __mtk_apu_run(rproc);
	if (ret)
		goto remove_mtk_apu_dram_boot;

	return ret;

remove_mtk_apu_dram_boot:
	mtk_apu_dram_boot_remove(apu);

remove_mtk_apu_config_setup:
	mtk_apu_config_remove(apu);

out_mtk_apu_start:
	return ret;
}

static int mtk_apu_stop(struct rproc *rproc)
{
	struct mtk_apu *apu = (struct mtk_apu *)rproc->priv;
	const struct mtk_apu_hw_ops *hw_ops = &apu->platdata->ops;

	if (!hw_ops->stop) {
		WARN_ON(1);
		return -EINVAL;
	}
	hw_ops->stop(apu);

	mtk_apu_dram_boot_remove(apu);
	mtk_apu_config_remove(apu);

	return 0;
}

static int mtk_apu_prepare(struct rproc *rproc)
{
	struct mtk_apu *apu = rproc->priv;
	struct device_node *data_np;
	struct resource r;
	int ret;

	/* get reserved memory */
	data_np = of_parse_phandle(apu->pdev->dev.of_node, "memory-region", 0);
	if (!data_np) {
		dev_err(apu->dev, "No reserved memory region found.\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(data_np, 0, &r);
	if (ret) {
		dev_err(apu->dev, "failed to parse reserved memory: %d\n", ret);
		of_node_put(data_np);
		return ret;
	}
	of_node_put(data_np);

	apu->resv_mem_pa = r.start;
	apu->resv_mem_va = memremap(r.start, resource_size(&r), MEMREMAP_WB);
	dev_dbg(apu->dev, "Reserved memory %pR mapped to %p\n\n", &r, apu->resv_mem_va);
	memset(apu->resv_mem_va, 0, resource_size(&r));

	ret = mtk_apu_mem_init(apu);
	if (ret)
		goto out_mtk_apu_prepare;

	ret = mtk_apu_ipi_init(apu->pdev, apu);
	if (ret)
		goto out_mtk_apu_prepare;

	ret = mtk_apu_deepidle_init(apu);
	if (ret)
		goto remove_mtk_apu_ipi;

	ret = mtk_apu_debug_init(apu);
	if (ret)
		goto remove_mtk_apu_deepidle;

	ret = mtk_apu_timesync_init(apu);
	if (ret)
		goto remove_mtk_apu_debug;

	ret = mtk_apu_exception_init(apu->pdev, apu);
	if (ret)
		goto remove_mtk_apu_timesync;

	return ret;

remove_mtk_apu_timesync:
	mtk_apu_timesync_remove(apu);

remove_mtk_apu_debug:
	mtk_apu_debug_remove(apu);

remove_mtk_apu_deepidle:
	mtk_apu_deepidle_exit(apu);

remove_mtk_apu_ipi:
	mtk_apu_ipi_remove(apu);

out_mtk_apu_prepare:
	return ret;
}

static int mtk_apu_unprepare(struct rproc *rproc)
{
	struct mtk_apu *apu = rproc->priv;
	memunmap(apu->resv_mem_va);

	mtk_apu_exception_exit(apu->pdev, apu);
	mtk_apu_timesync_remove(apu);
	mtk_apu_debug_remove(apu);
	mtk_apu_deepidle_exit(apu);
	mtk_apu_ipi_remove(apu);

	return 0;
}

static const struct rproc_ops mtk_apu_ops = {
	.start		= mtk_apu_start,
	.stop		= mtk_apu_stop,
	.load		= mtk_apu_load,
	.prepare	= mtk_apu_prepare,
	.unprepare	= mtk_apu_unprepare,
	.da_to_va	= mtk_apu_da_to_va,
};

static int mtk_apu_power_genpd_notifier(struct notifier_block *nb, unsigned long event, void *data)
{
	struct mtk_apu *apu = container_of(nb, struct mtk_apu, genpd_nb);

	switch (event) {
	case GENPD_NOTIFY_OFF:
		apu->top_genpd = false;
		dev_dbg(apu->dev, "%s: apu top off\n", __func__);
		break;
	case GENPD_NOTIFY_ON:
		apu->top_genpd = true;
		dev_dbg(apu->dev, "%s: apu top on\n", __func__);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static int mtk_apu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *hw_logger_np;
	struct rproc *rproc;
	struct mtk_apu *apu;
	const struct mtk_apu_platdata *data;
	const struct mtk_apu_hw_ops *hw_ops;
	int ret = 0;

	data = of_device_get_match_data(dev);
	hw_ops = &data->ops;

	rproc = devm_rproc_alloc(dev, np->name, &mtk_apu_ops, data->fw_name, sizeof(struct mtk_apu));
	if (!rproc)
		return dev_err_probe(dev, -ENOMEM, "unable to allocate remoteproc\n");

	rproc->auto_boot = data->flags.auto_boot;
	rproc->sysfs_read_only = true;

	apu = rproc->priv;
	apu->rproc = rproc;
	apu->pdev = pdev;
	apu->dev = dev;
	apu->platdata = data;
	apu->genpd_nb.notifier_call = mtk_apu_power_genpd_notifier;

	hw_logger_np = of_parse_phandle(np, "mediatek,hw-logger", 0);
	if (!hw_logger_np)
		return dev_err_probe(dev, -ENOENT, "failed to get hw-logger phandle\n");

	apu->hw_logger_data = get_mtk_apu_hw_logger_device(hw_logger_np);

	of_node_put(hw_logger_np);

	if (!apu->hw_logger_data)
		return dev_err_probe(dev, -EPROBE_DEFER, "failed to get hw-logger data\n");

	platform_set_drvdata(pdev, apu);

	spin_lock_init(&apu->reg_lock);

	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);

	ret = rproc_add(rproc);
	if (ret < 0) {
		dev_info(dev, "boot fail ret:%d\n", ret);
		goto err_put_device;
	}

	pm_runtime_put_sync(&pdev->dev);

	dev_pm_genpd_add_notifier(dev, &apu->genpd_nb);

	return 0;

err_put_device:
	pm_runtime_put_sync(&apu->pdev->dev);
	pm_runtime_disable(&pdev->dev);

	return ret;
}

static int mtk_apu_remove(struct platform_device *pdev)
{
	struct mtk_apu *apu = platform_get_drvdata(pdev);

	dev_pm_genpd_remove_notifier(&pdev->dev);
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	rproc_del(apu->rproc);

	return 0;
}

extern const struct mtk_apu_platdata mt8188_platdata;
static const struct of_device_id mtk_apu_of_match[] = {
	{ .compatible = "mediatek,mt8188-apusys-rv", .data = &mt8188_platdata},
	{},
};
MODULE_DEVICE_TABLE(of, mtk_apu_of_match);

static struct platform_driver mtk_apu_driver = {
	.probe = mtk_apu_probe,
	.remove = mtk_apu_remove,
	.driver = {
		.name = "mtk-apu",
		.of_match_table = of_match_ptr(mtk_apu_of_match),
	},
};

module_platform_driver(mtk_apu_driver);
MODULE_DESCRIPTION("MTK APUSys Driver");
MODULE_LICENSE("GPL");
