// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/math64.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/sched/clock.h>

#include "mdw_cmn.h"
#include "mdw_mem_dma.h"
#include "mdw_mem_pool.h"

#define mdw_cmd_show(c, f) \
	f("cmd(0x%llx/0x%llx/0x%llx/0x%llx/%d/%u)param(%u/%u/%u/%u/"\
	"%u/%u/%u)subcmds(%u/%p/%u/%u)pid(%d/%d)(%d)\n", \
	(uint64_t) c->mpriv, c->uid, c->kid, c->rvid, c->id, kref_read(&c->ref), \
	c->priority, c->hardlimit, c->softlimit, \
	c->power_save, c->power_plcy, c->power_dtime, \
	c->app_type, c->num_subcmds, c->cmdbufs, \
	c->num_cmdbufs, c->size_cmdbufs, \
	c->pid, c->tgid, task_pid_nr(current))

static void mdw_cmd_cmdbuf_out(struct mdw_fpriv *mpriv, struct mdw_cmd *c)
{
	struct mdw_subcmd_kinfo *ksubcmd = NULL;
	unsigned int i = 0, j = 0;

	if (!c->cmdbufs)
		return;

	/* flush cmdbufs and execinfos */
	if (mdw_mem_invalidate(mpriv, c->cmdbufs))
		dev_info(mpriv->mdev->dev, "[warn] s(0x%llx)c(0x%llx) invalidate cmdbufs(%u) fail\n",
				(uint64_t)mpriv, c->kid, c->cmdbufs->size);

	for (i = 0; i < c->num_subcmds; i++) {
		ksubcmd = &c->ksubcmds[i];
		for (j = 0; j < ksubcmd->info->num_cmdbufs; j++) {
			if (!ksubcmd->ori_cbs[j]) {
				dev_info(mpriv->mdev->dev, "[warn] no ori mems(%d-%d)\n", i, j);
				continue;
			}

			/* cmdbuf copy out */
			if (ksubcmd->cmdbufs[j].direction != MDW_CB_IN) {
				memcpy(ksubcmd->ori_cbs[j]->vaddr,
					(void *)ksubcmd->kvaddrs[j],
					ksubcmd->ori_cbs[j]->size);
			}
		}
	}
}

static void mdw_cmd_put_cmdbufs(struct mdw_fpriv *mpriv, struct mdw_cmd *c)
{
	struct mdw_subcmd_kinfo *ksubcmd = NULL;
	unsigned int i = 0, j = 0;

	if (!c->cmdbufs)
		return;

	for (i = 0; i < c->num_subcmds; i++) {
		ksubcmd = &c->ksubcmds[i];
		for (j = 0; j < ksubcmd->info->num_cmdbufs; j++) {
			if (!ksubcmd->ori_cbs[j]) {
				dev_info(mpriv->mdev->dev, "[warn] no ori mems(%d-%d)\n", i, j);
				continue;
			}

			/* put mem */
			mdw_mem_put(mpriv, ksubcmd->ori_cbs[j]);
			ksubcmd->ori_cbs[j] = NULL;
		}
	}

	mdw_mem_pool_free(c->cmdbufs);
	c->cmdbufs = NULL;
}

static int mdw_cmd_get_cmdbufs(struct mdw_fpriv *mpriv, struct mdw_cmd *c)
{
	unsigned int i = 0, j = 0, ofs = 0;
	int ret = -EINVAL;
	struct mdw_subcmd_kinfo *ksubcmd = NULL;
	struct mdw_mem *m = NULL;
	struct apusys_cmdbuf *acbs = NULL;

	if (!c->size_cmdbufs || c->cmdbufs)
		goto out;

	c->cmdbufs = mdw_mem_pool_alloc(&mpriv->cmd_buf_pool, c->size_cmdbufs,
		MDW_DEFAULT_ALIGN);
	if (!c->cmdbufs) {
		dev_err(mpriv->mdev->dev, "[error] s(0x%llx)c(0x%llx) alloc buffer for duplicate fail\n",
				(uint64_t) mpriv, c->kid);
		ret = -ENOMEM;
		goto out;
	}

	/* alloc mem for duplicated cmdbuf */
	for (i = 0; i < c->num_subcmds; i++) {
		mdw_cmd_debug("sc(0x%llx-%u) #cmdbufs(%u)\n",
			c->kid, i, c->ksubcmds[i].info->num_cmdbufs);

		acbs = kcalloc(c->ksubcmds[i].info->num_cmdbufs, sizeof(*acbs), GFP_KERNEL);
		if (!acbs)
			goto free_cmdbufs;

		ksubcmd = &c->ksubcmds[i];
		for (j = 0; j < ksubcmd->info->num_cmdbufs; j++) {
			/* calc align */
			if (ksubcmd->cmdbufs[j].align)
				ofs = MDW_ALIGN(ofs, ksubcmd->cmdbufs[j].align);
			else
				ofs = MDW_ALIGN(ofs, MDW_DEFAULT_ALIGN);

			mdw_cmd_debug("sc(0x%llx-%u) cb#%u offset(%u)\n",
				c->kid, i, j, ofs);

			/* get mem from handle */
			m = mdw_mem_get(mpriv, ksubcmd->cmdbufs[j].handle);
			if (!m) {
				dev_err(mpriv->mdev->dev, "[error] sc(0x%llx-%u) cb#%u(%llu) get fail\n",
						c->kid, i, j, ksubcmd->cmdbufs[j].handle);
				ret = -EINVAL;
				goto free_cmdbufs;
			}
			/* check mem boundary */
			if (m->vaddr == NULL ||
				ksubcmd->cmdbufs[j].size != m->size) {
				dev_err(mpriv->mdev->dev, "[error] sc(0x%llx-%u) cb#%u invalid range(%p/%u/%u)\n",
						c->kid, i, j, m->vaddr, ksubcmd->cmdbufs[j].size, m->size);
				ret = -EINVAL;
				goto free_cmdbufs;
			}

			/* cmdbuf copy in */
			if (ksubcmd->cmdbufs[j].direction != MDW_CB_OUT) {
				memcpy(c->cmdbufs->vaddr + ofs,
					m->vaddr,
					ksubcmd->cmdbufs[j].size);
			}

			/* record buffer info */
			ksubcmd->ori_cbs[j] = m;
			ksubcmd->kvaddrs[j] =
				(uint64_t)(c->cmdbufs->vaddr + ofs);
			ksubcmd->daddrs[j] =
				(uint64_t)(c->cmdbufs->device_va + ofs);
			ofs += ksubcmd->cmdbufs[j].size;

			mdw_cmd_debug("sc(0x%llx-%u) cb#%u (0x%llx/0x%llx/%u)\n",
				c->kid, i, j,
				ksubcmd->kvaddrs[j],
				ksubcmd->daddrs[j],
				ksubcmd->cmdbufs[j].size);

			acbs[j].kva = (void *)ksubcmd->kvaddrs[j];
			acbs[j].size = ksubcmd->cmdbufs[j].size;
		}

		ret = mdw_dev_validation(mpriv, ksubcmd->info->type,
			c, acbs, ksubcmd->info->num_cmdbufs);
		kfree(acbs);
		acbs = NULL;
		if (ret) {
			dev_err(mpriv->mdev->dev, "[error] sc(0x%llx-%u) dev(%u) validate cb(%u) fail(%d)\n",
					c->kid, i, ksubcmd->info->type, ksubcmd->info->num_cmdbufs, ret);
			goto free_cmdbufs;
		}
	}
	/* flush cmdbufs */
	if (mdw_mem_flush(mpriv, c->cmdbufs))
		dev_info(mpriv->mdev->dev, "[warn] s(0x%llx) c(0x%llx) flush cmdbufs(%u) fail\n",
				(uint64_t)mpriv, c->kid, c->cmdbufs->size);

	ret = 0;
	goto out;

free_cmdbufs:
	mdw_cmd_put_cmdbufs(mpriv, c);
	kfree(acbs);
out:
	mdw_cmd_debug("ret(%d)\n", ret);
	return ret;
}

static unsigned int mdw_cmd_create_infos(struct mdw_fpriv *mpriv,
	struct mdw_cmd *c)
{
	unsigned int i = 0, j = 0, total_size = 0, tmp_size = 0;
	struct mdw_subcmd_exec_info *sc_einfo = NULL;
	int ret = -ENOMEM;

	c->einfos = c->exec_infos->vaddr;
	if (!c->einfos) {
		dev_err(mpriv->mdev->dev, "[error] invalid exec info addr\n");
		return -EINVAL;
	} else {
		/* clear run infos for return */
		memset(c->exec_infos->vaddr, 0, c->exec_infos->size);
	}
	sc_einfo = &c->einfos->sc;

	for (i = 0; i < c->num_subcmds; i++) {
		c->ksubcmds[i].info = &c->subcmds[i];
		mdw_cmd_debug("subcmd(%u)(%u/%u/%u/%u/%u/%u/%u/%u/0x%llx)\n",
			i, c->subcmds[i].type,
			c->subcmds[i].suggest_time, c->subcmds[i].vlm_usage,
			c->subcmds[i].vlm_ctx_id, c->subcmds[i].vlm_force,
			c->subcmds[i].boost, c->subcmds[i].pack_id,
			c->subcmds[i].num_cmdbufs, c->subcmds[i].cmdbufs);

		/* kva for oroginal buffer */
		c->ksubcmds[i].ori_cbs = kcalloc(c->subcmds[i].num_cmdbufs,
			sizeof(c->ksubcmds[i].ori_cbs), GFP_KERNEL);
		if (!c->ksubcmds[i].ori_cbs)
			goto free_cmdbufs;

		/* record kva for duplicate */
		c->ksubcmds[i].kvaddrs = kcalloc(c->subcmds[i].num_cmdbufs,
			sizeof(*c->ksubcmds[i].kvaddrs), GFP_KERNEL);
		if (!c->ksubcmds[i].kvaddrs)
			goto free_cmdbufs;

		/* record dva for cmdbufs */
		c->ksubcmds[i].daddrs = kcalloc(c->subcmds[i].num_cmdbufs,
			sizeof(*c->ksubcmds[i].daddrs), GFP_KERNEL);
		if (!c->ksubcmds[i].daddrs)
			goto free_cmdbufs;

		/* allocate for subcmd cmdbuf */
		c->ksubcmds[i].cmdbufs = kcalloc(c->subcmds[i].num_cmdbufs,
			sizeof(*c->ksubcmds[i].cmdbufs), GFP_KERNEL);
		if (!c->ksubcmds[i].cmdbufs)
			goto free_cmdbufs;

		/* copy cmdbuf info */
		if (copy_from_user(c->ksubcmds[i].cmdbufs,
			(void __user *)c->subcmds[i].cmdbufs,
			c->subcmds[i].num_cmdbufs *
			sizeof(*c->ksubcmds[i].cmdbufs))) {
			goto free_cmdbufs;
		}

		c->ksubcmds[i].sc_einfo = &sc_einfo[i];

		/* accumulate cmdbuf size with alignment */
		for (j = 0; j < c->subcmds[i].num_cmdbufs; j++) {
			c->num_cmdbufs++;
			/* alignment */
			if (c->ksubcmds[i].cmdbufs[j].align) {
				tmp_size = MDW_ALIGN(total_size,
					c->ksubcmds[i].cmdbufs[j].align);
			} else {
				tmp_size = MDW_ALIGN(total_size, MDW_DEFAULT_ALIGN);
			}
			if (tmp_size < total_size) {
				dev_err(mpriv->mdev->dev, "[error] cmdbuf(%u,%u) size align overflow(%u/%u/%u)\n",
						i, j, total_size, c->ksubcmds[i].cmdbufs[j].align, tmp_size);
				goto free_cmdbufs;
			}
			total_size = tmp_size;

			/* accumulator */
			tmp_size = total_size + c->ksubcmds[i].cmdbufs[j].size;
			if (tmp_size < total_size) {
				dev_err(mpriv->mdev->dev, "[error] cmdbuf(%u,%u) size overflow(%u/%u/%u)\n",
						i, j, total_size, c->ksubcmds[i].cmdbufs[j].size, tmp_size);
				goto free_cmdbufs;
			}
			total_size = tmp_size;
		}
	}
	c->size_cmdbufs = total_size;

	mdw_cmd_debug("sc(0x%llx) cb_num(%u) total size(%u)\n",
		c->kid, c->num_cmdbufs, c->size_cmdbufs);

	ret = mdw_cmd_get_cmdbufs(mpriv, c);
	if (ret)
		goto free_cmdbufs;

	goto out;

free_cmdbufs:
	for (i = 0; i < c->num_subcmds; i++) {
		/* free dvaddrs */
		kfree(c->ksubcmds[i].daddrs);
		c->ksubcmds[i].daddrs = NULL;

		/* free kvaddrs */
		kfree(c->ksubcmds[i].kvaddrs);
		c->ksubcmds[i].kvaddrs = NULL;

		/* free ori kvas */
		kfree(c->ksubcmds[i].ori_cbs);
		c->ksubcmds[i].ori_cbs = NULL;

		/* free cmdbufs */
		kfree(c->ksubcmds[i].cmdbufs);
		c->ksubcmds[i].cmdbufs = NULL;
	}

out:
	return ret;
}

static void mdw_cmd_delete_infos(struct mdw_fpriv *mpriv, struct mdw_cmd *c)
{
	unsigned int i = 0;

	mdw_cmd_put_cmdbufs(mpriv, c);

	for (i = 0; i < c->num_subcmds; i++) {
		/* free dvaddrs */
		kfree(c->ksubcmds[i].daddrs);
		c->ksubcmds[i].daddrs = NULL;

		/* free kvaddrs */
		kfree(c->ksubcmds[i].kvaddrs);
		c->ksubcmds[i].kvaddrs = NULL;

		/* free ori kvas */
		kfree(c->ksubcmds[i].ori_cbs);
		c->ksubcmds[i].ori_cbs = NULL;

		/* free cmdbufs */
		kfree(c->ksubcmds[i].cmdbufs);
		c->ksubcmds[i].cmdbufs = NULL;
	}
}

void mdw_cmd_mpriv_release(struct mdw_fpriv *mpriv)
{
	struct mdw_cmd *c = NULL;
	uint32_t id = 0;

	if (!atomic_read(&mpriv->active) && !atomic_read(&mpriv->active_cmds)) {
		mdw_flw_debug("s(0x%llx) release cmd\n", (uint64_t)mpriv);
		idr_for_each_entry(&mpriv->cmds, c, id) {
			idr_remove(&mpriv->cmds, id);
			mdw_cmd_delete(c);
		}
		mdw_flw_debug("s(0x%llx) release mem\n", (uint64_t)mpriv);
		mutex_lock(&mpriv->mdev->mctl_mtx);
		mdw_mem_mpriv_release(mpriv);
		mutex_unlock(&mpriv->mdev->mctl_mtx);
	}
}

static uint64_t mdw_fence_ctx_alloc(struct mdw_device *mdev)
{
	uint64_t idx = 0, ctx = 0;

	mutex_lock(&mdev->f_mtx);
	idx = find_first_zero_bit(mdev->fence_ctx_mask, mdev->num_fence_ctx);
	if (idx >= mdev->num_fence_ctx) {
		ctx = dma_fence_context_alloc(1);
		dev_info(mdev->dev, "[warn] no free fence ctx(%llu), alloc ctx(%llu)\n", idx, ctx);
	} else {
		set_bit(idx, mdev->fence_ctx_mask);
		ctx = mdev->base_fence_ctx + idx;
	}
	mutex_unlock(&mdev->f_mtx);
	mdw_cmd_debug("alloc fence ctx(%llu) idx(%llu) base(%llu)\n",
		ctx, idx, mdev->base_fence_ctx);

	return ctx;
}

static void mdw_fence_ctx_free(struct mdw_device *mdev, uint64_t ctx)
{
	int idx = 0;

	idx = ctx - mdev->base_fence_ctx;
	if (idx < 0 || idx >= mdev->num_fence_ctx) {
		mdw_cmd_debug("out of range ctx(%llu/%llu)\n", ctx, mdev->base_fence_ctx);
		return;
	}

	mutex_lock(&mdev->f_mtx);
	if (!test_bit(idx, mdev->fence_ctx_mask))
		dev_info(mdev->dev, "[warn] ctx state conflict(%d)\n", idx);
	else
		clear_bit(idx, mdev->fence_ctx_mask);
	mutex_unlock(&mdev->f_mtx);
}

static const char *mdw_fence_get_driver_name(struct dma_fence *fence)
{
	return "apu_mdw";
}

static const char *mdw_fence_get_timeline_name(struct dma_fence *fence)
{
	struct mdw_fence *f =
		container_of(fence, struct mdw_fence, base_fence);

	return f->name;
}

static bool mdw_fence_enable_signaling(struct dma_fence *fence)
{
	return true;
}

static void mdw_fence_release(struct dma_fence *fence)
{
	struct mdw_fence *mf =
		container_of(fence, struct mdw_fence, base_fence);

	mdw_flw_debug("fence release, fence(%s/%llu-%llu)\n",
		mf->name, mf->base_fence.context, mf->base_fence.seqno);
	mdw_fence_ctx_free(mf->mdev, mf->base_fence.context);
	kfree(mf);
}

static const struct dma_fence_ops mdw_fence_ops = {
	.get_driver_name =  mdw_fence_get_driver_name,
	.get_timeline_name =  mdw_fence_get_timeline_name,
	.enable_signaling =  mdw_fence_enable_signaling,
	.wait = dma_fence_default_wait,
	.release =  mdw_fence_release,
};

static int mdw_fence_init(struct mdw_cmd *c, int fd)
{
	int ret = 0;
	struct mdw_device *mdev = c->mpriv->mdev;

	c->fence = kzalloc(sizeof(*c->fence), GFP_KERNEL);
	if (!c->fence)
		return -ENOMEM;

	if (snprintf(c->fence->name, sizeof(c->fence->name), "%d:%s", fd, c->comm) <= 0)
		pr_info("[warn] %s: set fance name fail\n", __func__);
	c->fence->mdev = c->mpriv->mdev;
	spin_lock_init(&c->fence->lock);
	dma_fence_init(&c->fence->base_fence, &mdw_fence_ops,
		&c->fence->lock, mdw_fence_ctx_alloc(mdev),
		atomic_add_return(1, &c->mpriv->exec_seqno));

	mdw_flw_debug("fence init, c(0x%llx) fence(%s/%llu-%llu)\n",
		(uint64_t)c, c->fence->name, c->fence->base_fence.context,
		c->fence->base_fence.seqno);

	return ret;
}

static void mdw_cmd_unvoke_map(struct mdw_cmd *c)
{
	struct mdw_cmd_map_invoke *cm_invoke = NULL, *tmp = NULL;

	list_for_each_entry_safe(cm_invoke, tmp, &c->map_invokes, c_node) {
		list_del(&cm_invoke->c_node);
		mdw_cmd_debug("s(0x%llx)c(0x%llx) unvoke m(0x%llx/%u)\n",
			(uint64_t)c->mpriv, (uint64_t)c,
			cm_invoke->map->m->device_va,
			cm_invoke->map->m->dva_size);
		cm_invoke->map->put(cm_invoke->map);
		kfree(cm_invoke);
	}
}

static void mdw_cmd_release(struct kref *ref)
{
	struct mdw_cmd *c =
			container_of(ref, struct mdw_cmd, ref);
	struct mdw_fpriv *mpriv = c->mpriv;

	mdw_cmd_show(c, mdw_drv_debug);
	if (c->del_internal)
		c->del_internal(c);
	mutex_lock(&mpriv->mdev->mctl_mtx);
	mdw_cmd_unvoke_map(c);
	mutex_unlock(&mpriv->mdev->mctl_mtx);
	mdw_cmd_delete_infos(c->mpriv, c);
	mdw_mem_put(c->mpriv, c->exec_infos);
	kfree(c->adj_matrix);
	kfree(c->ksubcmds);
	kfree(c->subcmds);
	kfree(c);

	mpriv->put(mpriv);
}

static void mdw_cmd_put(struct mdw_cmd *c)
{
	kref_put(&c->ref, mdw_cmd_release);
}

static void mdw_cmd_get(struct mdw_cmd *c)
{
	kref_get(&c->ref);
}

void mdw_cmd_delete(struct mdw_cmd *c)
{
	mdw_cmd_show(c, mdw_drv_debug);
	mdw_cmd_put(c);
}

static void mdw_cmd_delete_async(struct mdw_cmd *c)
{
	struct mdw_device *mdev = c->mpriv->mdev;

	/* add cmd list to delete */
	mutex_lock(&mdev->c_mtx);
	list_add_tail(&c->d_node, &mdev->d_cmds);
	mutex_unlock(&mdev->c_mtx);

	schedule_work(&mdev->c_wk);
}

static int mdw_cmd_sanity_check(struct mdw_cmd *c)
{
	if (c->priority >= MDW_PRIORITY_MAX ||
		c->num_subcmds > MDW_SUBCMD_MAX ||
		c->num_links > c->num_subcmds) {
		pr_err("[error] %s: s(0x%llx)cmd invalid(0x%llx/0x%llx)(%u/%u/%u)\n",
				__func__, (uint64_t)c->mpriv, c->uid, c->kid,
				c->priority, c->num_subcmds, c->num_links);
		return -EINVAL;
	}

	if (c->exec_infos->size != sizeof(struct mdw_cmd_exec_info) +
		c->num_subcmds * sizeof(struct mdw_subcmd_exec_info)) {
		pr_err("[error] %s: s(0x%llx)cmd invalid(0x%llx/0x%llx) einfo(%u/%zu)\n",
				__func__, (uint64_t)c->mpriv, c->uid, c->kid,
				c->exec_infos->size,
				sizeof(struct mdw_cmd_exec_info) +
				c->num_subcmds * sizeof(struct mdw_subcmd_exec_info));
		return -EINVAL;
	}

	return 0;
}

static int mdw_cmd_adj_check(struct mdw_cmd *c)
{
	uint32_t i = 0, j = 0;

	for (i = 0; i < c->num_subcmds; i++) {
		for (j = 0; j < c->num_subcmds; j++) {
			if (i == j) {
				c->adj_matrix[i * c->num_subcmds + j] = 0;
				continue;
			}

			if (i < j)
				continue;

			if (!c->adj_matrix[i * c->num_subcmds + j] ||
				!c->adj_matrix[i + j * c->num_subcmds])
				continue;

			pr_err("[error] %s: s(0x%llx)c(0x%llx/0x%llx) adj matrix(%u/%u) fail\n",
					__func__, (uint64_t)c->mpriv, c->uid, c->kid, i, j);
			return -EINVAL;
		}
	}

	return 0;
}

static int mdw_cmd_link_check(struct mdw_cmd *c)
{
	uint32_t i = 0;

	for (i = 0; i < c->num_links; i++) {
		if (c->links[i].producer_idx > c->num_subcmds ||
			c->links[i].consumer_idx > c->num_subcmds ||
			!c->links[i].x || !c->links[i].y ||
			!c->links[i].va) {
			pr_err("[error] %s: link(%u) invalid(%u/%u)(%llu/%llu)(0x%llx)\n",
					__func__, i,
					c->links[i].producer_idx,
					c->links[i].consumer_idx,
					c->links[i].x,
					c->links[i].y,
					c->links[i].va);
			return -EINVAL;
		}
	}
	return 0;
}

static int mdw_cmd_sc_sanity_check(struct mdw_cmd *c)
{
	unsigned int i = 0;

	/* subcmd info */
	for (i = 0; i < c->num_subcmds; i++) {
		if (c->subcmds[i].type >= MDW_DEV_MAX ||
			c->subcmds[i].vlm_ctx_id >= MDW_SUBCMD_MAX ||
			c->subcmds[i].boost > MDW_BOOST_MAX ||
			c->subcmds[i].pack_id >= MDW_SUBCMD_MAX) {
			pr_err("[error] %s: subcmd(%u) invalid (%u/%u/%u)\n",
					__func__, i, c->subcmds[i].type,
					c->subcmds[i].boost,
					c->subcmds[i].pack_id);
			return -EINVAL;
		}
	}

	return 0;
}

static int mdw_cmd_run(struct mdw_fpriv *mpriv, struct mdw_cmd *c)
{
	struct mdw_device *mdev = mpriv->mdev;
	struct dma_fence *f = &c->fence->base_fence;
	int ret = 0;

	mdw_cmd_show(c, mdw_cmd_debug);

	c->start_ts = sched_clock();
	ret = mdev->dev_funcs->run_cmd(mpriv, c);
	if (ret) {
		dev_err(mpriv->mdev->dev, "[error] s(0x%llx) run cmd(0x%llx) fail(%d)\n",
				(uint64_t) c->mpriv, c->kid, ret);
		dma_fence_set_error(f, ret);
		if (dma_fence_signal(f)) {
			dev_info(mpriv->mdev->dev, "[warn] c(0x%llx) signal fence fail\n", (uint64_t)c);
			if (f->ops->get_timeline_name && f->ops->get_driver_name) {
				dev_info(mpriv->mdev->dev, "[warn]  fence name(%s-%s)\n",
						f->ops->get_driver_name(f), f->ops->get_timeline_name(f));
			}
		}
		dma_fence_put(f);
	} else {
		mdw_flw_debug("s(0x%llx) cmd(0x%llx) run\n",
			(uint64_t)c->mpriv, c->kid);
	}

	return ret;
}

static void mdw_cmd_check_rets(struct mdw_cmd *c, int ret)
{
	uint32_t idx = 0;
	DECLARE_BITMAP(tmp, 64);

	memcpy(&tmp, &c->einfos->c.sc_rets, sizeof(c->einfos->c.sc_rets));

	/* extract fail subcmd */
	do {
		idx = find_next_bit((unsigned long *)&tmp, c->num_subcmds, idx);
		if (idx >= c->num_subcmds)
			break;

		pr_info("[warn] %s: sc(0x%llx-#%u) type(%u) softlimit(%u) boost(%u) fail\n",
				__func__, c->kid, idx, c->subcmds[idx].type,
				c->softlimit, c->subcmds[idx].boost);

		idx++;
	} while (idx < c->num_subcmds);
}

static int mdw_cmd_complete(struct mdw_cmd *c, int ret)
{
	struct dma_fence *f = &c->fence->base_fence;
	struct mdw_fpriv *mpriv = c->mpriv;

	mutex_lock(&c->mtx);

	c->end_ts = sched_clock();
	c->einfos->c.total_us = div_u64(c->end_ts - c->start_ts, 1000);
	mdw_flw_debug("s(0x%llx) c(%s/0x%llx/0x%llx/0x%llx) ret(%d) sc_rets(0x%llx) complete, pid(%d/%d)(%d)\n",
		(uint64_t)mpriv, c->comm, c->uid, c->kid, c->rvid,
		ret, c->einfos->c.sc_rets,
		c->pid, c->tgid, task_pid_nr(current));

	/* check subcmds return value */
	if (c->einfos->c.sc_rets) {
		if (!ret)
			ret = -EIO;

		mdw_cmd_check_rets(c, ret);
	}
	c->einfos->c.ret = ret;

	if (ret) {
		pr_err("[error] %s: s(0x%llx) c(%s/0x%llx/0x%llx/0x%llx) ret(%d/0x%llx) time(%llu) pid(%d/%d)\n",
				__func__, (uint64_t)mpriv, c->comm, c->uid, c->kid, c->rvid,
				ret, c->einfos->c.sc_rets,
				c->einfos->c.total_us, c->pid, c->tgid);
		dma_fence_set_error(f, ret);
	} else {
		mdw_flw_debug("s(0x%llx) c(%s/0x%llx/0x%llx/0x%llx) ret(%d/0x%llx) time(%llu) pid(%d/%d)\n",
			(uint64_t)mpriv, c->comm, c->uid, c->kid, c->rvid,
			ret, c->einfos->c.sc_rets,
			c->einfos->c.total_us, c->pid, c->tgid);
	}

	mdw_cmd_cmdbuf_out(mpriv, c);

	/* signal done */
	c->fence = NULL;
	atomic_dec(&c->is_running);
	if (dma_fence_signal(f)) {
		pr_info("[warn] %s: c(0x%llx) signal fence fail\n", __func__, (uint64_t)c);
		if (f->ops->get_timeline_name && f->ops->get_driver_name) {
			pr_info("[warn] %s:  fence name(%s-%s)\n",
					__func__, f->ops->get_driver_name(f), f->ops->get_timeline_name(f));
		}
	}
	dma_fence_put(f);
	atomic_dec(&mpriv->active_cmds);
	mutex_unlock(&c->mtx);

	/* check mpriv to clean cmd */
	mutex_lock(&mpriv->mtx);
	mdw_cmd_mpriv_release(mpriv);
	mutex_unlock(&mpriv->mtx);

	/* put cmd execution ref */
	mdw_cmd_put(c);

	return 0;
}

static void mdw_cmd_trigger_func(struct work_struct *wk)
{
	struct mdw_cmd *c =
		container_of(wk, struct mdw_cmd, t_wk);
	int ret = 0;

	if (c->wait_fence) {
		dma_fence_wait(c->wait_fence, false);
		dma_fence_put(c->wait_fence);
	}

	mdw_flw_debug("s(0x%llx) c(0x%llx) wait fence done, start run\n",
		(uint64_t)c->mpriv, c->kid);
	mutex_lock(&c->mtx);
	ret = mdw_cmd_run(c->mpriv, c);
	mutex_unlock(&c->mtx);

	/* put cmd execution ref */
	if (ret) {
		atomic_dec(&c->is_running);
		mdw_cmd_put(c);
	}
}

static struct mdw_cmd *mdw_cmd_create(struct mdw_fpriv *mpriv,
	union mdw_cmd_args *args)
{
	struct mdw_cmd_in *in = (struct mdw_cmd_in *)args;
	struct mdw_cmd *c = NULL;

	/* check num subcmds maximum */
	if (in->exec.num_subcmds > MDW_SUBCMD_MAX) {
		dev_err(mpriv->mdev->dev, "[error] too much subcmds(%u)\n", in->exec.num_subcmds);
		goto out;
	}

	/* alloc mdw cmd */
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		goto out;

	mutex_init(&c->mtx);
	INIT_LIST_HEAD(&c->map_invokes);
	c->mpriv = mpriv;
	atomic_set(&c->is_running, 0);

	/* setup cmd info */
	c->pid = task_pid_nr(current);
	c->tgid = task_tgid_nr(current);
	c->kid = (uint64_t)c;
	c->uid = in->exec.uid;
	get_task_comm(c->comm, current);
	c->priority = in->exec.priority;
	c->hardlimit = in->exec.hardlimit;
	c->softlimit = in->exec.softlimit;
	c->power_save = in->exec.power_save;
	c->power_plcy = in->exec.power_plcy;
	c->power_dtime = in->exec.power_dtime;
	c->fastmem_ms = in->exec.fastmem_ms;
	c->app_type = in->exec.app_type;
	c->num_subcmds = in->exec.num_subcmds;
	c->num_links = in->exec.num_links;
	c->exec_infos = mdw_mem_get(mpriv, in->exec.exec_infos);
	if (!c->exec_infos) {
		dev_err(mpriv->mdev->dev, "[error] get exec info fail\n");
		goto free_cmd;
	}

	/* check input params */
	if (mdw_cmd_sanity_check(c)) {
		dev_err(mpriv->mdev->dev, "[error] cmd sanity check fail\n");
		goto put_execinfos;
	}

	/* subcmds/ksubcmds */
	c->subcmds = kzalloc(c->num_subcmds * sizeof(*c->subcmds), GFP_KERNEL);
	if (!c->subcmds)
		goto put_execinfos;
	if (copy_from_user(c->subcmds, (void __user *)in->exec.subcmd_infos,
		c->num_subcmds * sizeof(*c->subcmds))) {
		dev_err(mpriv->mdev->dev, "[error] copy subcmds fail\n");
		goto free_subcmds;
	}
	if (mdw_cmd_sc_sanity_check(c)) {
		dev_err(mpriv->mdev->dev, "[error] sc sanity check fail\n");
		goto free_subcmds;
	}

	c->ksubcmds = kzalloc(c->num_subcmds * sizeof(*c->ksubcmds),
		GFP_KERNEL);
	if (!c->ksubcmds)
		goto free_subcmds;

	/* adj matrix */
	c->adj_matrix = kzalloc(c->num_subcmds *
		c->num_subcmds * sizeof(uint8_t), GFP_KERNEL);
	if (!c->adj_matrix)
		goto free_ksubcmds;
	if (copy_from_user(c->adj_matrix, (void __user *)in->exec.adj_matrix,
		(c->num_subcmds * c->num_subcmds * sizeof(uint8_t)))) {
		dev_err(mpriv->mdev->dev, "[error] copy adj matrix fail\n");
		goto free_adj;
	}
	if (mpriv->mdev->mdw_klog & MDW_DBG_CMD) {
		print_hex_dump(KERN_INFO, "[apusys] adj matrix: ",
			DUMP_PREFIX_OFFSET, 16, 1, c->adj_matrix,
			c->num_subcmds * c->num_subcmds, 0);
	}
	if (mdw_cmd_adj_check(c))
		goto free_adj;

	/* link */
	if (c->num_links) {
		c->links = kcalloc(c->num_links, sizeof(*c->links), GFP_KERNEL);
		if (!c->links)
			goto free_adj;
		if (copy_from_user(c->links, (void __user *)in->exec.links,
			c->num_links * sizeof(*c->links))) {
			dev_err(mpriv->mdev->dev, "[error] copy links fail\n");
			goto free_link;
		}
	}
	if (mdw_cmd_link_check(c))
		goto free_link;

	/* create infos */
	if (mdw_cmd_create_infos(mpriv, c)) {
		dev_err(mpriv->mdev->dev, "[error] create cmd info fail\n");
		goto free_link;
	}

	c->mpriv->get(c->mpriv);
	c->complete = mdw_cmd_complete;
	INIT_WORK(&c->t_wk, &mdw_cmd_trigger_func);
	kref_init(&c->ref);
	mdw_cmd_show(c, mdw_drv_debug);

	goto out;

free_link:
	kfree(c->links);
free_adj:
	kfree(c->adj_matrix);
free_ksubcmds:
	kfree(c->ksubcmds);
free_subcmds:
	kfree(c->subcmds);
put_execinfos:
	mdw_mem_put(mpriv, c->exec_infos);
free_cmd:
	kfree(c);
	c = NULL;
out:
	return c;
}

static int mdw_cmd_ioctl_del(struct mdw_fpriv *mpriv, union mdw_cmd_args *args)
{
	struct mdw_cmd_in *in = (struct mdw_cmd_in *)args;
	struct mdw_cmd *c = NULL;
	int ret = 0;

	mutex_lock(&mpriv->mtx);
	c = (struct mdw_cmd *)idr_find(&mpriv->cmds, in->id);
	if (!c) {
		ret = -EINVAL;
		dev_info(mpriv->mdev->dev, "[warn] can't find id(%lld)\n", in->id);
	} else {
		if (c != idr_remove(&mpriv->cmds, c->id))
			dev_info(mpriv->mdev->dev, "[warn] remove cmd idr conflict(0x%llx/%d)\n", c->kid, c->id);
		mdw_cmd_delete(c);
	}
	mutex_unlock(&mpriv->mtx);

	return ret;
}

static int mdw_cmd_ioctl_run(struct mdw_fpriv *mpriv, union mdw_cmd_args *args)
{
	struct mdw_cmd_in *in = (struct mdw_cmd_in *)args;
	struct mdw_cmd *c = NULL, *priv_c = NULL;
	struct sync_file *sync_file = NULL;
	int ret = 0, fd = 0, wait_fd = 0, is_running = 0;

	/* get wait fd */
	wait_fd = in->exec.fence;

	mutex_lock(&mpriv->mtx);
	/* get stale cmd */
	c = (struct mdw_cmd *)idr_find(&mpriv->cmds, in->id);
	if (!c) {
		/* no stale cmd, create cmd */
		mdw_cmd_debug("s(0x%llx) create new\n", (uint64_t)mpriv);
	} else if (in->op == MDW_CMD_IOCTL_RUN_STALE) {
		is_running = atomic_read(&c->is_running);
		if (is_running) {
			dev_err(mpriv->mdev->dev, "[error] s(0x%llx) c(0x%llx) is running(%d), can't execute again\n",
					(uint64_t)mpriv, (uint64_t)c, is_running);
			ret = -ETXTBSY;
			goto out;
		}
		/* run stale cmd */
		mdw_cmd_debug("s(0x%llx) run stale(0x%llx)\n",
			(uint64_t)mpriv, (uint64_t)c);
		goto exec;
	} else {
		/* release stale cmd and create new */
		mdw_cmd_debug("s(0x%llx) delete stale(0x%llx) and create new\n",
			(uint64_t)mpriv, (uint64_t)c);
		priv_c = c;
		c = NULL;
		if (priv_c != idr_remove(&mpriv->cmds, priv_c->id)) {
			dev_info(mpriv->mdev->dev, "[warn] remove cmd idr conflict(0x%llx/%d)\n",
					priv_c->kid, priv_c->id);
		}
	}

	/* create cmd */
	c = mdw_cmd_create(mpriv, args);
	if (!c) {
		dev_err(mpriv->mdev->dev, "[error] create cmd fail\n");
		ret = -EINVAL;
		goto out;
	}
	memset(args, 0, sizeof(*args));

	/* alloc idr */
	c->id = idr_alloc(&mpriv->cmds, c, MDW_CMD_IDR_MIN, MDW_CMD_IDR_MAX, GFP_KERNEL);
	if (c->id < MDW_CMD_IDR_MIN) {
		dev_err(mpriv->mdev->dev, "[error] alloc idr fail(%d)\n", c->id);
		goto delete_cmd;
	}

exec:
	mutex_lock(&c->mtx);
	/* get sync_file fd */
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		dev_err(mpriv->mdev->dev, "[error] get unused fd fail\n");
		ret = -EINVAL;
		goto delete_idr;
	}
	if (mdw_fence_init(c, fd)) {
		dev_err(mpriv->mdev->dev, "[error] cmd init fence fail\n");
		goto put_fd;
	}
	sync_file = sync_file_create(&c->fence->base_fence);
	if (!sync_file) {
		dev_err(mpriv->mdev->dev, "[error] create sync file fail\n");
		dma_fence_put(&c->fence->base_fence);
		ret = -ENOMEM;
		goto put_fd;
	}

	/* get cmd execution ref */
	atomic_inc(&c->is_running);
	mdw_cmd_get(c);

	/* check wait fence from other module */
	mdw_flw_debug("s(0x%llx)c(0x%llx) wait fence(%d)...\n",
			(uint64_t)c->mpriv, c->kid, wait_fd);
	c->wait_fence = sync_file_get_fence(wait_fd);
	if (!c->wait_fence) {
		mdw_flw_debug("s(0x%llx)c(0x%llx) no wait fence, trigger directly\n",
			(uint64_t)c->mpriv, c->kid);
		ret = mdw_cmd_run(mpriv, c);
	} else {
		/* wait fence from wq */
		schedule_work(&c->t_wk);
	}

	if (ret) {
		/* put cmd execution ref */
		atomic_dec(&c->is_running);
		mdw_cmd_put(c);
		goto put_file;
	}

	/* assign fd */
	fd_install(fd, sync_file->file);

	/* get ref for cmd exec */
	atomic_inc(&mpriv->active_cmds);

	/* return fd */
	args->out.exec.fence = fd;
	args->out.exec.id = c->id;
	mdw_flw_debug("async fd(%d) id(%d)\n", fd, c->id);
	mutex_unlock(&c->mtx);
	goto out;

put_file:
	fput(sync_file->file);
put_fd:
	put_unused_fd(fd);
delete_idr:
	if (c != idr_remove(&mpriv->cmds, c->id))
		dev_info(mpriv->mdev->dev, "[warn] remove cmd idr conflict(0x%llx/%d)\n", c->kid, c->id);
	mutex_unlock(&c->mtx);
delete_cmd:
	mdw_cmd_delete(c);
out:
	mutex_unlock(&mpriv->mtx);
	if (priv_c)
		mdw_cmd_delete_async(priv_c);

	return ret;
}

int mdw_cmd_ioctl(struct mdw_fpriv *mpriv, void *data)
{
	union mdw_cmd_args *args = (union mdw_cmd_args *)data;
	int ret = 0;

	mdw_flw_debug("s(0x%llx) op::%d\n", (uint64_t)mpriv, args->in.op);

	switch (args->in.op) {
	case MDW_CMD_IOCTL_RUN:
	case MDW_CMD_IOCTL_RUN_STALE:
		ret = mdw_cmd_ioctl_run(mpriv, args);
		break;
	case MDW_CMD_IOCTL_DEL:
		ret = mdw_cmd_ioctl_del(mpriv, args);
		break;

	default:
		ret = -EINVAL;
		break;
	}
	mdw_flw_debug("done\n");

	return ret;
}
