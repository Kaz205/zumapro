/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#ifndef MTK_APU_LOADIMAGE_H
#define MTK_APU_LOADIMAGE_H

#define APUSYS_MEM_IOVA_ALIGN 0x100000			/* 1M (for secure iova mapping) */
#define APUSYS_FW_ALIGN 16				/* for mdla dma alignment limitation */

/******************************************************************************
 * 1. New entries must be appended to the end of the structure.
 * 2. Do NOT use conditional option such as #ifdef inside the structure.
 */
enum PT_ID_APUSYS {
	PT_ID_APUSYS_FW,
	PT_ID_APUSYS_XFILE,
	PT_ID_MDLA_FW_BOOT,
	PT_ID_MDLA_FW_MAIN,
	PT_ID_MDLA_XFILE,
	PT_ID_MVPU_FW,
	PT_ID_MVPU_XFILE,
	PT_ID_MVPU_SEC_FW,
	PT_ID_MVPU_SEC_XFILE
};

#define PT_MAGIC			0x58901690

struct ptimg_hdr_t {
	u32 magic;     /* magic number*/
	u32 hdr_size;  /* header size */
	u32 img_size;  /* img size */
	u32 align;     /* alignment */
	u32 id;        /* image id */
	u32 addr;      /* memory addr */
};

#endif /* MTK_APU_LOADIMAGE_H */