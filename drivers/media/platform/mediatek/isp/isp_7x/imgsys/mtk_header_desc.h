/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 MediaTek Inc.
 *
 */

#ifndef _HEADER_DESC_
#define _HEADER_DESC_

#include <linux/videodev2.h>

#define COMPACT_USE
struct v4l2_ext_plane {
#ifndef COMPACT_USE
	__u32			bytesused;
	__u32			length;
#endif
	union {
#ifndef COMPACT_USE
		__u32		mem_offset;
		__u64		userptr;
#endif
		struct {
			__s32		fd;
			__u32		offset;
			__u64		phyaddr;
		} dma_buf;
	} m;
#ifndef COMPACT_USE
	__u32			data_offset;
	__u32			reserved[11];
#else
	__u64			reserved[2];
#endif
};

#define IMGBUF_MAX_PLANES (3)

struct v4l2_ext_buffer {
#ifndef COMPACT_USE
	__u32			index;
	__u32			type;
	__u32			flags;
	__u32			field;
	__u64			timestamp;
	__u32			sequence;
	__u32			memory;
#endif
	struct v4l2_ext_plane	planes[IMGBUF_MAX_PLANES];
	__u32			num_planes;
#ifndef COMPACT_USE
	__u32			reserved[11];
#else
	__u64			reserved[2];
#endif
};

struct mtk_imgsys_crop {
	struct v4l2_rect	c;
	struct v4l2_fract	left_subpix;
	struct v4l2_fract	top_subpix;
	struct v4l2_fract	width_subpix;
	struct v4l2_fract	height_subpix;
};

struct plane_pix_format {
	__u32		sizeimage;
	__u32		bytesperline;
} __packed;

struct pix_format_mplane {
	__u32				width;
	__u32				height;
	__u32				pixelformat;
	struct plane_pix_format	plane_fmt[IMGBUF_MAX_PLANES];
} __packed;

struct buf_format {
	union {
		struct pix_format_mplane	pix_mp;  /* V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE */
	} fmt;
};

struct buf_info {
	struct v4l2_ext_buffer buf;
	struct buf_format fmt;
	struct mtk_imgsys_crop crop;
	/* struct v4l2_rect compose; */
	__u32 rotation;
	__u32 hflip;
	__u32 vflip;
	__u8  resizeratio;
	__u32  secu;
};

#define FRAME_BUF_MAX (1)
struct frameparams {
	struct buf_info bufs[FRAME_BUF_MAX];
};

#define SCALE_MAX (1)
#define TIME_MAX (12)

struct header_desc {
	__u32 fparams_tnum;
	struct frameparams fparams[TIME_MAX][SCALE_MAX];
};

#define TMAX (16)
struct header_desc_norm {
	__u32 fparams_tnum;
	struct frameparams fparams[TMAX][SCALE_MAX];
};

#define IMG_MAX_HW_DMAS     72

struct singlenode_desc_norm {
	__u8 dmas_enable[IMG_MAX_HW_DMAS][TMAX];
	struct header_desc_norm	dmas[IMG_MAX_HW_DMAS];
	struct header_desc_norm	tuning_meta;
	struct header_desc_norm	ctrl_meta;
};

#endif
