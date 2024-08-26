# SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
obj-$(CPTCFG_IWLMLD)   += iwlmld.o

iwlmld-y += mld.o notif.o mac80211.o fw.o power.o iface.o link.o rx.o mcc.o
iwlmld-$(CPTCFG_IWLWIFI_DEBUGFS) += debugfs.o

subdir-ccflags-y += -I$(srctree)/$(src)/../
