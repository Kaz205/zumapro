// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>

#include <linux/remoteproc/mtk_apu.h>
#include <linux/soc/mediatek/mtk_apu_secure.h>
#include "../mtk_apu_rproc.h"

enum MTK_APU_SMC_REVISER_CTRL {
	REVISER_CTRL_INIT = 0,
	REVISER_CTRL_BACKUP,
	REVISER_CTRL_RESTORE,
};

static int mtk_apu_setup_reviser(struct mtk_apu *apu, int boundary, int ns, int domain)
{
	struct device *dev = apu->dev;

	if (!apu->platdata->flags.secure_boot) {
		dev_err(dev, "Not support non-secure boot\n");
		return -EINVAL;
	}

	return mtk_apu_rv_smc_call(dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_SETUP_REVISER, 0);
}

static int mtk_apu_setup_devapc(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;

	return mtk_apu_rv_smc_call(dev, MTK_APUSYS_KERNEL_OP_DEVAPC_INIT_RCX, 0);
}

static int mtk_apu_reset_mp(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;

	if (!apu->platdata->flags.secure_boot) {
		dev_err(dev, "Not support non-secure boot\n");
		return -EINVAL;
	}

	return mtk_apu_rv_smc_call(dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_RESET_MP, 0);
}

static int mtk_apu_setup_boot(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;

	if (!apu->platdata->flags.secure_boot) {
		dev_err(dev, "Not support non-secure boot\n");
		return -EINVAL;
	}

	return mtk_apu_rv_smc_call(dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_SETUP_BOOT, 0);
}

static int mt8188_rproc_setup(struct mtk_apu *apu)
{
	int ns = 1; /* Non Secure */
	int domain = 0;
	int boundary = (u32) upper_32_bits(apu->code_da);
	int ret;

	ret = mtk_apu_setup_devapc(apu);
	if (ret) {
		dev_err(apu->dev, "Failed to setup devapc\n");
		return ret;
	}

	ret = mtk_apu_setup_reviser(apu, boundary, ns, domain);
	if (ret) {
		dev_err(apu->dev, "Failed to setup reviser\n");
		return ret;
	}

	ret = mtk_apu_reset_mp(apu);
	if (ret) {
		dev_err(apu->dev, "Failed to reset mp\n");
		return ret;
	}

	ret = mtk_apu_setup_boot(apu);
	if (ret) {
		dev_err(apu->dev, "Failed to setup boot\n");
		return ret;
	}

	return 0;
}

static int mt8188_rproc_start(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;

	if (!apu->platdata->flags.secure_boot) {
		dev_err(dev, "Not support non-secure boot\n");
		return -EINVAL;
	}

	return mtk_apu_rv_smc_call(dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_START_MP, 0);
}

static int mt8188_rproc_stop(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;

	if (!apu->platdata->flags.secure_boot) {
		dev_err(dev, "Not support non-secure boot\n");
		return -EINVAL;
	}

	return mtk_apu_rv_smc_call(dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_STOP_MP, 0);
}

static int mt8188_apu_power_on(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		dev_err(dev, "runtime PM get_sync failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int mt8188_apu_power_off(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;
	int ret, timeout;

	ret = pm_runtime_put_sync(dev);
	if (ret) {
		dev_err(dev, "%s: runtime PM put_sync(dev) failed: %d\n", __func__, ret);
		goto error_genpd;
	}

	/* polling APU TOP rpm state till suspended */
	timeout = 500;
	while (READ_ONCE(apu->top_genpd) && timeout-- > 0)
		msleep(20);

	if (timeout <= 0) {
		dev_warn(dev, "Wait for APU power off timed out!\n");
		mtk_apu_ipi_unlock(apu);
		ret = -ETIMEDOUT;
		goto error_genpd;
	}

	return 0;

error_genpd:
	pm_runtime_get_noresume(dev);

	return ret;
}

const struct mtk_apu_platdata mt8188_platdata = {
	.flags	= {
		.preload_firmware = true,
		.auto_boot = true,
		.kernel_load_image = true,
		.map_iova = true,
		.secure_boot = true
	},
	.config = {
		.up_code_buf_sz = 0x100000,
		.up_coredump_buf_sz = 0x180000,
		.regdump_buf_sz	= 0x10000,
		.mdla_coredump_buf_sz = 0x0,
		.mvpu_coredump_buf_sz = 0x0,
		.mvpu_sec_coredump_buf_sz = 0x0,
	},
	.ops	= {
		.start	= mt8188_rproc_start,
		.stop	= mt8188_rproc_stop,
		.setup = mt8188_rproc_setup,
		.power_on = mt8188_apu_power_on,
		.power_off = mt8188_apu_power_off,
	},
	.fw_name = "mediatek/mt8188/apusys.img",
};
