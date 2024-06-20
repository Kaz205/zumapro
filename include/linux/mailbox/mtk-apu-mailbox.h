/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 MediaTek Inc.
 *
 */

#ifndef __MTK_APU_MAILBOX_H__
#define __MTK_APU_MAILBOX_H__

void mtk_apu_mbox_write(u32 val, u32 offset);
u32 mtk_apu_mbox_read(u32 offset);

#endif /* __MTK_APU_MAILBOX_H__ */
