# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_MTK_APUSYS_PM) += mtk_apu_pm.o

obj-$(CONFIG_MTK_APUSYS_SUPPORT) += mtk_apu_hw_logger.o

obj-$(CONFIG_MTK_APUSYS_SUPPORT) += mtk_apu_mdw.o

# declare apusys version
APUSYS_VERSION_MIDWARE = 2.0

# middleware makefile
ccflags-y += -I$(srctree)/drivers/soc/mediatek/apusys/midware/$(APUSYS_VERSION_MIDWARE)/
ccflags-y += -I$(srctree)/drivers/soc/mediatek/apusys/midware/$(APUSYS_VERSION_MIDWARE)/rv/

mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/mdw_mem_drv.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/mdw_mem_rsc.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/mdw_mem.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/mdw_mem_dma.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/mdw_mem_pool.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/mdw_drv.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/mdw_dev.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/mdw_ioctl.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/mdw_cmd.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/mdw_hs.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/mdw_sysfs.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/mdw_dbg.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/mdw_util.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/rv/mdw_rv.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/rv/mdw_rv_cmd.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/rv/mdw_rv_cmd_v3.o
mtk_apu_mdw-objs += midware/$(APUSYS_VERSION_MIDWARE)/rv/mdw_rv_dev.o

obj-$(CONFIG_MTK_APUSYS_SUPPORT) += mtk_apu_pwr_ipi_rx.o
