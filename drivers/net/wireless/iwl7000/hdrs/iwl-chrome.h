#ifndef __IWL_CHROME
#define __IWL_CHROME
/* This file is pre-included from the Makefile (cc command line)
 *
 * ChromeOS backport definitions
 * Copyright (C) 2016-2017 Intel Deutschland GmbH
 * Copyright (C) 2018-2024 Intel Corporation
 */

#include <linux/version.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/idr.h>
#include <linux/vmalloc.h>

/* get the CPTCFG_* preprocessor symbols */
#include <hdrs/config.h>

#define LINUX_VERSION_IS_LESS(x1,x2,x3) (LINUX_VERSION_CODE < KERNEL_VERSION(x1,x2,x3))
#define LINUX_VERSION_IS_GEQ(x1,x2,x3)  (LINUX_VERSION_CODE >= KERNEL_VERSION(x1,x2,x3))
#define LINUX_VERSION_IN_RANGE(x1,x2,x3, y1,y2,y3) \
        (LINUX_VERSION_IS_GEQ(x1,x2,x3) && LINUX_VERSION_IS_LESS(y1,y2,y3))
#define LINUX_BACKPORT(sym) backport_ ## sym

#include <hdrs/mac80211-exp.h>

#include <net/genetlink.h>
#include <linux/crypto.h>
#include <linux/moduleparam.h>
#include <linux/debugfs.h>
#include <linux/hrtimer.h>
#include <crypto/algapi.h>
#include <linux/pci.h>
#include <linux/if_vlan.h>
#include "net/fq.h"

#include <hdrs/net/dropreason.h>

#ifndef DECLARE_FLEX_ARRAY
/**
 * __DECLARE_FLEX_ARRAY() - Declare a flexible array usable in a union
 *
 * @TYPE: The type of each flexible array element
 * @NAME: The name of the flexible array member
 *
 * In order to have a flexible array member in a union or alone in a
 * struct, it needs to be wrapped in an anonymous struct with at least 1
 * named member, but that member can be empty.
+ */
#define __DECLARE_FLEX_ARRAY(TYPE, NAME)       \
	struct {			       \
		struct { } __empty_ ## NAME;   \
		TYPE NAME[];		       \
	}

/**
 * DECLARE_FLEX_ARRAY() - Declare a flexible array usable in a union
 *
 * @TYPE: The type of each flexible array element
 * @NAME: The name of the flexible array member
 *
 * In order to have a flexible array member in a union or alone in a
 * struct, it needs to be wrapped in an anonymous struct with at least 1
 * named member, but that member can be empty.
 */
#define DECLARE_FLEX_ARRAY(TYPE, NAME) \
	__DECLARE_FLEX_ARRAY(TYPE, NAME)
#endif

#ifndef __struct_group

/**
 * __struct_group() - Create a mirrored named and anonyomous struct
 *
 * @TAG: The tag name for the named sub-struct (usually empty)
 * @NAME: The identifier name of the mirrored sub-struct
 * @ATTRS: Any struct attributes (usually empty)
 * @MEMBERS: The member declarations for the mirrored structs
 *
 * Used to create an anonymous union of two structs with identical layout
 * and size: one anonymous and one named. The former's members can be used
 * normally without sub-struct naming, and the latter can be used to
 * reason about the start, end, and size of the group of struct members.
 * The named struct can also be explicitly tagged for layer reuse, as well
 * as both having struct attributes appended.
 */
#define __struct_group(TAG, NAME, ATTRS, MEMBERS...) \
	union { \
		struct { MEMBERS } ATTRS; \
		struct TAG { MEMBERS } ATTRS NAME; \
	}

#endif /* __struct_group */

#ifndef struct_group

/**
 * struct_group() - Wrap a set of declarations in a mirrored struct
 *
 * @NAME: The identifier name of the mirrored sub-struct
 * @MEMBERS: The member declarations for the mirrored structs
 *
 * Used to create an anonymous union of two structs with identical
 * layout and size: one anonymous and one named. The former can be
 * used normally without sub-struct naming, and the latter can be
 * used to reason about the start, end, and size of the group of
 * struct members.
 */
#define struct_group(NAME, MEMBERS...)	\
	__struct_group(/* no tag */, NAME, /* no attrs */, MEMBERS)

#endif /* struct_group */

/*
 * Need to include these here, otherwise we get the regular kernel ones
 * pre-including them makes it work, even though later the kernel ones
 * are included again, but they (hopefully) have the same include guard
 * ifdef/define so the second time around nothing happens
 *
 * We still keep them in the correct directory so if they don't exist in
 * the kernel (e.g. bitfield.h won't) the preprocessor can find them.
 */
#include <hdrs/linux/bitfield.h>
#include <hdrs/linux/ieee80211.h>
#include <hdrs/linux/average.h>
#include <hdrs/net/ieee80211_radiotap.h>
#include <linux/if_ether.h>
#include <net/cfg80211.h>
#include <linux/errqueue.h>
#include <generated/utsrelease.h>
#include <net/ieee80211_radiotap.h>
#include <crypto/hash.h>
#include <net/dsfield.h>
#include "version.h"

/* mac80211 & backport - order matters, need this inbetween */
#include <hdrs/backports.h>

#include <hdrs/net/codel.h>
#include <hdrs/net/mac80211.h>

/* artifacts of backports - never in upstream */
#define genl_info_snd_portid(__genl_info) (__genl_info->snd_portid)
#define NETLINK_CB_PORTID(__skb) NETLINK_CB(cb->skb).portid
#define netlink_notify_portid(__notify) __notify->portid
#define __genl_const const
#define __genl_ro_after_init __ro_after_init
#define netdev_tstats(dev)	dev->tstats
#define netdev_assign_tstats(dev, e)	dev->tstats = (e);

static inline struct netlink_ext_ack *genl_info_extack(struct genl_info *info)
{
	return info->extack;
}

#endif /* __IWL_CHROME */
