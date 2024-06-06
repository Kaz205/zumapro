# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_VIDEO_MEDIATEK_VCODEC) += mtk-vcodec-dec-mt8188.o \
				       mtk-vcodec-dec-hw-mt8188.o

mtk-vcodec-dec-mt8188-y := vdec/vdec_h264_if.o \
		vdec/vdec_vp8_if.o \
		vdec/vdec_vp8_req_if.o \
		vdec/vdec_vp9_if.o \
		vdec/vdec_vp9_req_lat_if.o \
		vdec/vdec_av1_req_lat_if.o \
		vdec/vdec_h264_req_if.o \
		vdec/vdec_h264_req_common.o \
		vdec/vdec_h264_req_multi_if.o \
		vdec/vdec_hevc_req_multi_if.o \
		mtk_vcodec_dec_drv.o \
		vdec_drv_if.o \
		vdec_vpu_if.o \
		vdec_msg_queue.o \
		mtk_vcodec_dec.o \
		mtk_vcodec_dec_stateful.o \
		mtk_vcodec_dec_stateless.o \
		mtk_vcodec_dec_pm.o \
		mtk_vcodec_dec_optee.o \

mtk-vcodec-dec-hw-mt8188-y := mtk_vcodec_dec_hw.o
