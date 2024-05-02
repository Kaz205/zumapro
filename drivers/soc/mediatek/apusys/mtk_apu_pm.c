// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/arm-smccc.h>
#include <linux/of_device.h>
#include <linux/pm_domain.h>
#include <linux/regulator/consumer.h>

#include <linux/soc/mediatek/mtk_apu_secure.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>

#define MTK_APU_TOP_DOMAIN_ID	(0)

#define to_mtk_apu_domain(gpd) container_of(gpd, struct mtk_apu_domain, genpd)

struct apusys {
	struct device *dev;
	struct regulator *vsram_supply;
	struct wakeup_source *ws;
	const struct mtk_apu_pm_data *data;
	struct genpd_onecell_data pd_data;
	struct generic_pm_domain *domains[];
};

struct mtk_apu_domain {
	struct generic_pm_domain genpd;
	const struct mtk_apu_domain_data *data;
	struct apusys *apusys;
	struct regulator *domain_supply;
};

struct mtk_apu_domain_data {
	int domain_idx;
	char *name;
};

struct mtk_apu_pm_data {
	const struct mtk_apu_domain_data *domains_data;
	int num_domains;
};

static const struct mtk_apu_domain_data mtk_apu_domain_data_mt8188[] = {
	{
		.domain_idx = MTK_APU_TOP_DOMAIN_ID,
		.name       = "mtk-apu-top",
	}
};

static const struct mtk_apu_pm_data mt8188_apu_pm_data = {
	.domains_data = mtk_apu_domain_data_mt8188,
	.num_domains  = ARRAY_SIZE(mtk_apu_domain_data_mt8188),
};

static int mtk_apu_top_power_on(struct generic_pm_domain *genpd)
{
	struct mtk_apu_domain *pd = to_mtk_apu_domain(genpd);
	struct apusys *apusys = pd->apusys;
	int ret, tmp;
	struct arm_smccc_res res;

	__pm_stay_awake(apusys->ws);

	if (apusys->vsram_supply) {
		ret = regulator_enable(apusys->vsram_supply);
		if (ret)
			return ret;
	}

	if (pd->domain_supply) {
		ret = regulator_enable(pd->domain_supply);
		if (ret)
			goto err_regulator;
	}

	arm_smccc_smc(MTK_SIP_APUSYS_CONTROL,
		      MTK_APUSYS_KERNEL_OP_APUSYS_PWR_TOP_ON,
		      0, 0, 0, 0, 0, 0, &res);
	if (res.a0) {
		dev_err(apusys->dev, "mtk apu top on fail: %lu\n", res.a0);
		ret = res.a0;
		goto err_top_on;
	}

	return 0;

err_top_on:
	if (pd->domain_supply) {
		tmp = regulator_disable(pd->domain_supply);
		if (tmp)
			dev_err(apusys->dev, "disable domain_supply fail: %d\n", tmp);
	}

err_regulator:
	if (apusys->vsram_supply) {
		tmp = regulator_disable(apusys->vsram_supply);
		if (tmp)
			dev_err(apusys->dev, "disable vsram_supply fail: %d\n", tmp);
	}

	return ret;
}

static int mtk_apu_top_power_off(struct generic_pm_domain *genpd)
{
	struct mtk_apu_domain *pd = to_mtk_apu_domain(genpd);
	struct apusys *apusys = pd->apusys;
	int ret, tmp;
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_APUSYS_CONTROL,
		      MTK_APUSYS_KERNEL_OP_APUSYS_PWR_TOP_OFF,
		      0, 0, 0, 0, 0, 0, &res);
	if (res.a0) {
		dev_err(apusys->dev, "mtk apu top off fail: %lu\n", res.a0);
		return res.a0;
	}

	if (pd->domain_supply) {
		tmp = regulator_disable(pd->domain_supply);
		if (tmp)
			ret = tmp;
	}

	if (apusys->vsram_supply) {
		tmp = regulator_disable(apusys->vsram_supply);
		if (tmp)
			ret = tmp;
	}

	__pm_relax(apusys->ws);

	return ret;
}

static struct generic_pm_domain *mtk_apu_add_one_domain(struct apusys *apusys,
							struct device_node *node)
{
	const struct mtk_apu_domain_data *domain_data;
	struct mtk_apu_domain *pd;
	int ret;
	u32 id;

	ret = of_property_read_u32(node, "reg", &id);
	if (ret) {
		dev_dbg(apusys->dev, "%pOF: invalid reg: %d\n", node, ret);
		return ERR_PTR(ret);
	}

	if (id >= apusys->data->num_domains) {
		dev_dbg(apusys->dev, "%pOF: invalid id %d\n", node, id);
		return ERR_PTR(-EINVAL);
	}

	domain_data = &apusys->data->domains_data[id];

	pd = devm_kzalloc(apusys->dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return ERR_PTR(-ENOMEM);

	pd->data   = domain_data;
	pd->apusys = apusys;

	pd->domain_supply = devm_regulator_get_optional(apusys->dev, "domain");
	if (IS_ERR(pd->domain_supply)) {
		ret = PTR_ERR(pd->domain_supply);
		if (ret != -EPROBE_DEFER) {
			dev_dbg(apusys->dev, "domain_supply fail, ret=%d", ret);
			apusys->vsram_supply = NULL;
		}
		return ERR_PTR(ret);
	}

	if (apusys->domains[id]) {
		ret = -EEXIST;
		dev_err(apusys->dev, "domain id %d already exists\n", id);
		goto err_put_clocks;
	}

	if (!pd->data->name)
		pd->genpd.name = node->name;
	else
		pd->genpd.name = pd->data->name;
	pd->genpd.power_off = mtk_apu_top_power_off;
	pd->genpd.power_on = mtk_apu_top_power_on;

	ret = pm_genpd_init(&pd->genpd, NULL, true);
	if (ret) {
		dev_err(apusys->dev, "init power domain fail\n");
		goto err_put_clocks;
	}

	apusys->domains[id] = &pd->genpd;

	return apusys->pd_data.domains[id];

err_put_clocks:
	return ERR_PTR(ret);
}

static void mtk_apu_remove_one_domain(struct mtk_apu_domain *pd)
{
	int ret;

	/*
	 * We're in the error cleanup already, so we only complain,
	 * but won't emit another error on top of the original one.
	 */
	ret = pm_genpd_remove(&pd->genpd);
	if (ret)
		dev_dbg(pd->apusys->dev,
			"Remove domain '%s' : %d - state may be inconsistent\n",
			pd->genpd.name, ret);
}

static void mtk_apu_domain_cleanup(struct apusys *apusys)
{
	struct generic_pm_domain *genpd;
	struct mtk_apu_domain *pd;
	int i;

	for (i = apusys->pd_data.num_domains - 1; i >= 0; i--) {
		genpd = apusys->pd_data.domains[i];
		if (genpd) {
			pd = to_mtk_apu_domain(genpd);
			mtk_apu_remove_one_domain(pd);
		}
	}
}

static const struct of_device_id mtk_apu_pm_of_match[] = {
	{
		.compatible = "mediatek,mt8188-apu-pm",
		.data = &mt8188_apu_pm_data,
	},
	{}
};
MODULE_DEVICE_TABLE(of, mtk_apu_pm_of_match);

static int mtk_apu_pm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct mtk_apu_pm_data *data;
	struct device_node *node;
	struct apusys *apusys;
	int ret;

	data = of_device_get_match_data(&pdev->dev);
	if (!data)
		return dev_err_probe(dev, -EINVAL, "No mtk apu power domain data\n");

	apusys = devm_kzalloc(dev, struct_size(apusys, domains, data->num_domains), GFP_KERNEL);
	if (!apusys)
		return -ENOMEM;

	platform_set_drvdata(pdev, apusys);
	apusys->dev = dev;
	apusys->data = data;
	apusys->pd_data.domains = apusys->domains;
	apusys->pd_data.num_domains = data->num_domains;

	/* To prevent system suspend while APU is running, register wakeup_source for apusys */
	apusys->ws = wakeup_source_register(NULL, "apuspm");

	apusys->vsram_supply = devm_regulator_get_optional(dev, "vsram");
	if (IS_ERR(apusys->vsram_supply)) {
		if (PTR_ERR(apusys->vsram_supply) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(apusys->vsram_supply),
					     "sram_supply fail\n");
		apusys->vsram_supply = NULL;
	}

	for_each_available_child_of_node(np, node) {
		struct generic_pm_domain *domain;

		domain = mtk_apu_add_one_domain(apusys, node);
		if (IS_ERR(domain)) {
			ret = PTR_ERR(domain);
			of_node_put(node);
			goto err_cleanup_domains;
		}
	}

	ret = of_genpd_add_provider_onecell(np, &apusys->pd_data);
	if (ret) {
		dev_dbg(dev, "failed to add provider: %d\n", ret);
		goto err_cleanup_domains;
	}

	return 0;

err_cleanup_domains:
	mtk_apu_domain_cleanup(apusys);
	wakeup_source_unregister(apusys->ws);
	return ret;
}

static int mtk_apu_pm_remove(struct platform_device *pdev)
{
	struct apusys *apusys = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	of_genpd_del_provider(dev->of_node);
	mtk_apu_domain_cleanup(apusys);
	wakeup_source_unregister(apusys->ws);

	return 0;
}

static struct platform_driver mtk_apu_pm_driver = {
	.probe = mtk_apu_pm_probe,
	.remove = mtk_apu_pm_remove,
	.driver = {
		.name = "mtk-apu-pm",
		.suppress_bind_attrs = true,
		.of_match_table = mtk_apu_pm_of_match,
	},
};
module_platform_driver(mtk_apu_pm_driver);
MODULE_DESCRIPTION("MediaTek APU Power Domain Driver");
MODULE_LICENSE("GPL v2");
