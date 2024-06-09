# SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
obj-$(CPTCFG_IWLMLD)   += iwlmld.o

iwlmld-y += mld.o notif.o mac80211.o hcmd.o fw.o
iwlmld-$(CPTCFG_IWLWIFI_DEBUGFS) += debugfs.o

subdir-ccflags-y += -I$(srctree)/$(src)/../
