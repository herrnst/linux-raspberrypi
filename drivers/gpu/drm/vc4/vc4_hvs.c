// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 Broadcom
 */

/**
 * DOC: VC4 HVS module.
 *
 * The Hardware Video Scaler (HVS) is the piece of hardware that does
 * translation, scaling, colorspace conversion, and compositing of
 * pixels stored in framebuffers into a FIFO of pixels going out to
 * the Pixel Valve (CRTC).  It operates at the system clock rate (the
 * system audio clock gate, specifically), which is much higher than
 * the pixel clock rate.
 *
 * There is a single global HVS, with multiple output FIFOs that can
 * be consumed by the PVs.  This file just manages the resources for
 * the HVS, while the vc4_crtc.c code actually drives HVS setup for
 * each CRTC.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_vblank.h>

#include "vc4_drv.h"
#include "vc4_regs.h"

static const struct debugfs_reg32 hvs_regs[] = {
	VC4_REG32(SCALER_DISPCTRL),
	VC4_REG32(SCALER_DISPSTAT),
	VC4_REG32(SCALER_DISPID),
	VC4_REG32(SCALER_DISPECTRL),
	VC4_REG32(SCALER_DISPPROF),
	VC4_REG32(SCALER_DISPDITHER),
	VC4_REG32(SCALER_DISPEOLN),
	VC4_REG32(SCALER_DISPLIST0),
	VC4_REG32(SCALER_DISPLIST1),
	VC4_REG32(SCALER_DISPLIST2),
	VC4_REG32(SCALER_DISPLSTAT),
	VC4_REG32(SCALER_DISPLACT0),
	VC4_REG32(SCALER_DISPLACT1),
	VC4_REG32(SCALER_DISPLACT2),
	VC4_REG32(SCALER_DISPCTRL0),
	VC4_REG32(SCALER_DISPBKGND0),
	VC4_REG32(SCALER_DISPSTAT0),
	VC4_REG32(SCALER_DISPBASE0),
	VC4_REG32(SCALER_DISPCTRL1),
	VC4_REG32(SCALER_DISPBKGND1),
	VC4_REG32(SCALER_DISPSTAT1),
	VC4_REG32(SCALER_DISPBASE1),
	VC4_REG32(SCALER_DISPCTRL2),
	VC4_REG32(SCALER_DISPBKGND2),
	VC4_REG32(SCALER_DISPSTAT2),
	VC4_REG32(SCALER_DISPBASE2),
	VC4_REG32(SCALER_DISPALPHA2),
	VC4_REG32(SCALER_OLEDOFFS),
	VC4_REG32(SCALER_OLEDCOEF0),
	VC4_REG32(SCALER_OLEDCOEF1),
	VC4_REG32(SCALER_OLEDCOEF2),
};

void vc4_hvs_dump_state(struct vc4_hvs *hvs)
{
	struct drm_printer p = drm_info_printer(&hvs->pdev->dev);
	int i;

	drm_print_regset32(&p, &hvs->regset);

	DRM_INFO("HVS ctx:\n");
	for (i = 0; i < 64; i += 4) {
		DRM_INFO("0x%08x (%s): 0x%08x 0x%08x 0x%08x 0x%08x\n",
			 i * 4, i < HVS_BOOTLOADER_DLIST_END ? "B" : "D",
			 readl((u32 __iomem *)hvs->dlist + i + 0),
			 readl((u32 __iomem *)hvs->dlist + i + 1),
			 readl((u32 __iomem *)hvs->dlist + i + 2),
			 readl((u32 __iomem *)hvs->dlist + i + 3));
	}
}

static int vc4_hvs_debugfs_underrun(struct seq_file *m, void *data)
{
	struct drm_info_node *node = m->private;
	struct drm_device *dev = node->minor->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_printer p = drm_seq_file_printer(m);

	drm_printf(&p, "%d\n", atomic_read(&vc4->underrun));

	return 0;
}

static int vc4_hvs_debugfs_dlist(struct seq_file *m, void *data)
{
	struct drm_info_node *node = m->private;
	struct drm_device *dev = node->minor->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	struct drm_printer p = drm_seq_file_printer(m);
	unsigned int next_entry_start = 0;
	unsigned int i, j;
	u32 dlist_word, dispstat;

	for (i = 0; i < SCALER_CHANNELS_COUNT; i++) {
		dispstat = VC4_GET_FIELD(HVS_READ(SCALER_DISPSTATX(i)),
					 SCALER_DISPSTATX_MODE);
		if (dispstat == SCALER_DISPSTATX_MODE_DISABLED ||
		    dispstat == SCALER_DISPSTATX_MODE_EOF) {
			drm_printf(&p, "HVS chan %u disabled\n", i);
			continue;
		}

		drm_printf(&p, "HVS chan %u:\n", i);

		for (j = HVS_READ(SCALER_DISPLISTX(i)); j < 256; j++) {
			dlist_word = readl((u32 __iomem *)vc4->hvs->dlist + j);
			drm_printf(&p, "dlist: %02d: 0x%08x\n", j,
				   dlist_word);
			if (!next_entry_start ||
			    next_entry_start == j) {
				if (dlist_word & SCALER_CTL0_END)
					break;
				next_entry_start = j +
					VC4_GET_FIELD(dlist_word,
						      SCALER_CTL0_SIZE);
			}
		}
	}

	return 0;
}

static int vc5_hvs_debugfs_gamma(struct seq_file *m, void *data)
{
	struct drm_info_node *node = m->private;
	struct drm_device *dev = node->minor->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	struct drm_printer p = drm_seq_file_printer(m);
	unsigned int i, chan;
	u32 dispstat, dispbkgndx;

	for (chan = 0; chan < SCALER_CHANNELS_COUNT; chan++) {
		u32 x_c, grad;
		u32 offset = SCALER5_DSPGAMMA_START +
			chan * SCALER5_DSPGAMMA_CHAN_OFFSET;

		dispstat = VC4_GET_FIELD(HVS_READ(SCALER_DISPSTATX(chan)),
					 SCALER_DISPSTATX_MODE);
		if (dispstat == SCALER_DISPSTATX_MODE_DISABLED ||
		    dispstat == SCALER_DISPSTATX_MODE_EOF) {
			drm_printf(&p, "HVS channel %u: Channel disabled\n", chan);
			continue;
		}

		dispbkgndx = HVS_READ(SCALER_DISPBKGNDX(chan));
		if (!(dispbkgndx & SCALER_DISPBKGND_GAMMA)) {
			drm_printf(&p, "HVS channel %u: Gamma disabled\n", chan);
			continue;
		}

		drm_printf(&p, "HVS channel %u:\n", chan);
		drm_printf(&p, "  red:\n");
		for (i = 0; i < SCALER5_DSPGAMMA_NUM_POINTS; i++, offset += 8) {
			x_c = HVS_READ(offset);
			grad = HVS_READ(offset + 4);
			drm_printf(&p, "  %08x %08x - x %u, c %u, grad %u\n",
				   x_c, grad,
				   VC4_GET_FIELD(x_c, SCALER5_DSPGAMMA_OFF_X),
				   VC4_GET_FIELD(x_c, SCALER5_DSPGAMMA_OFF_C),
				   grad);
		}
		drm_printf(&p, "  green:\n");
		for (i = 0; i < SCALER5_DSPGAMMA_NUM_POINTS; i++, offset += 8) {
			x_c = HVS_READ(offset);
			grad = HVS_READ(offset + 4);
			drm_printf(&p, "  %08x %08x - x %u, c %u, grad %u\n",
				   x_c, grad,
				   VC4_GET_FIELD(x_c, SCALER5_DSPGAMMA_OFF_X),
				   VC4_GET_FIELD(x_c, SCALER5_DSPGAMMA_OFF_C),
				   grad);
		}
		drm_printf(&p, "  blue:\n");
		for (i = 0; i < SCALER5_DSPGAMMA_NUM_POINTS; i++, offset += 8) {
			x_c = HVS_READ(offset);
			grad = HVS_READ(offset + 4);
			drm_printf(&p, "  %08x %08x - x %u, c %u, grad %u\n",
				   x_c, grad,
				   VC4_GET_FIELD(x_c, SCALER5_DSPGAMMA_OFF_X),
				   VC4_GET_FIELD(x_c, SCALER5_DSPGAMMA_OFF_C),
				   grad);
		}

		/* Alpha only valid on channel 2 */
		if (chan != 2)
			continue;

		drm_printf(&p, "  alpha:\n");
		for (i = 0; i < SCALER5_DSPGAMMA_NUM_POINTS; i++, offset += 8) {
			x_c = HVS_READ(offset);
			grad = HVS_READ(offset + 4);
			drm_printf(&p, "  %08x %08x - x %u, c %u, grad %u\n",
				   x_c, grad,
				   VC4_GET_FIELD(x_c, SCALER5_DSPGAMMA_OFF_X),
				   VC4_GET_FIELD(x_c, SCALER5_DSPGAMMA_OFF_C),
				   grad);
		}
	}
	return 0;
}

/* The filter kernel is composed of dwords each containing 3 9-bit
 * signed integers packed next to each other.
 */
#define VC4_INT_TO_COEFF(coeff) (coeff & 0x1ff)
#define VC4_PPF_FILTER_WORD(c0, c1, c2)				\
	((((c0) & 0x1ff) << 0) |				\
	 (((c1) & 0x1ff) << 9) |				\
	 (((c2) & 0x1ff) << 18))

/* The whole filter kernel is arranged as the coefficients 0-16 going
 * up, then a pad, then 17-31 going down and reversed within the
 * dwords.  This means that a linear phase kernel (where it's
 * symmetrical at the boundary between 15 and 16) has the last 5
 * dwords matching the first 5, but reversed.
 */
#define VC4_LINEAR_PHASE_KERNEL(c0, c1, c2, c3, c4, c5, c6, c7, c8,	\
				c9, c10, c11, c12, c13, c14, c15)	\
	{VC4_PPF_FILTER_WORD(c0, c1, c2),				\
	 VC4_PPF_FILTER_WORD(c3, c4, c5),				\
	 VC4_PPF_FILTER_WORD(c6, c7, c8),				\
	 VC4_PPF_FILTER_WORD(c9, c10, c11),				\
	 VC4_PPF_FILTER_WORD(c12, c13, c14),				\
	 VC4_PPF_FILTER_WORD(c15, c15, 0)}

#define VC4_LINEAR_PHASE_KERNEL_DWORDS 6
#define VC4_KERNEL_DWORDS (VC4_LINEAR_PHASE_KERNEL_DWORDS * 2 - 1)

/* Recommended B=1/3, C=1/3 filter choice from Mitchell/Netravali.
 * http://www.cs.utexas.edu/~fussell/courses/cs384g/lectures/mitchell/Mitchell.pdf
 */
static const u32 mitchell_netravali_1_3_1_3_kernel[] =
	VC4_LINEAR_PHASE_KERNEL(0, -2, -6, -8, -10, -8, -3, 2, 18,
				50, 82, 119, 155, 187, 213, 227);

static int vc4_hvs_upload_linear_kernel(struct vc4_hvs *hvs,
					struct drm_mm_node *space,
					const u32 *kernel)
{
	int ret, i;
	u32 __iomem *dst_kernel;

	ret = drm_mm_insert_node(&hvs->dlist_mm, space, VC4_KERNEL_DWORDS);
	if (ret) {
		DRM_ERROR("Failed to allocate space for filter kernel: %d\n",
			  ret);
		return ret;
	}

	dst_kernel = hvs->dlist + space->start;

	for (i = 0; i < VC4_KERNEL_DWORDS; i++) {
		if (i < VC4_LINEAR_PHASE_KERNEL_DWORDS)
			writel(kernel[i], &dst_kernel[i]);
		else {
			writel(kernel[VC4_KERNEL_DWORDS - i - 1],
			       &dst_kernel[i]);
		}
	}

	return 0;
}

static void vc4_hvs_lut_load(struct vc4_hvs *hvs,
			     struct vc4_crtc *vc4_crtc)
{
	struct drm_crtc *crtc = &vc4_crtc->base;
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc->state);
	u32 i;

	/* The LUT memory is laid out with each HVS channel in order,
	 * each of which takes 256 writes for R, 256 for G, then 256
	 * for B.
	 */
	HVS_WRITE(SCALER_GAMADDR,
		  SCALER_GAMADDR_AUTOINC |
		  (vc4_state->assigned_channel * 3 * crtc->gamma_size));

	for (i = 0; i < crtc->gamma_size; i++)
		HVS_WRITE(SCALER_GAMDATA, vc4_crtc->lut_r[i]);
	for (i = 0; i < crtc->gamma_size; i++)
		HVS_WRITE(SCALER_GAMDATA, vc4_crtc->lut_g[i]);
	for (i = 0; i < crtc->gamma_size; i++)
		HVS_WRITE(SCALER_GAMDATA, vc4_crtc->lut_b[i]);
}

static void vc4_hvs_update_gamma_lut(struct vc4_hvs *hvs,
				     struct vc4_crtc *vc4_crtc)
{
	struct drm_crtc_state *crtc_state = vc4_crtc->base.state;
	struct drm_color_lut *lut = crtc_state->gamma_lut->data;
	u32 length = drm_color_lut_size(crtc_state->gamma_lut);
	u32 i;

	for (i = 0; i < length; i++) {
		vc4_crtc->lut_r[i] = drm_color_lut_extract(lut[i].red, 8);
		vc4_crtc->lut_g[i] = drm_color_lut_extract(lut[i].green, 8);
		vc4_crtc->lut_b[i] = drm_color_lut_extract(lut[i].blue, 8);
	}

	vc4_hvs_lut_load(hvs, vc4_crtc);
}

u8 vc4_hvs_get_fifo_frame_count(struct vc4_hvs *hvs, unsigned int fifo)
{
	u8 field = 0;

	switch (fifo) {
	case 0:
		field = VC4_GET_FIELD(HVS_READ(SCALER_DISPSTAT1),
				      SCALER_DISPSTAT1_FRCNT0);
		break;
	case 1:
		field = VC4_GET_FIELD(HVS_READ(SCALER_DISPSTAT1),
				      SCALER_DISPSTAT1_FRCNT1);
		break;
	case 2:
		field = VC4_GET_FIELD(HVS_READ(SCALER_DISPSTAT2),
				      SCALER_DISPSTAT2_FRCNT2);
		break;
	}

	return field;
}

static void vc5_hvs_write_gamma_entry(struct vc4_hvs *hvs,
				      u32 offset,
				      struct vc5_gamma_entry *gamma)
{
	HVS_WRITE(offset, gamma->x_c_terms);
	HVS_WRITE(offset + 4, gamma->grad_term);
}

static void vc5_hvs_lut_load(struct vc4_hvs *hvs,
			     struct vc4_crtc *vc4_crtc)
{
	struct drm_crtc_state *crtc_state = vc4_crtc->base.state;
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc_state);
	u32 i;
	u32 offset = SCALER5_DSPGAMMA_START +
		vc4_state->assigned_channel * SCALER5_DSPGAMMA_CHAN_OFFSET;

	for (i = 0; i < SCALER5_DSPGAMMA_NUM_POINTS; i++, offset += 8)
		vc5_hvs_write_gamma_entry(hvs, offset, &vc4_crtc->pwl_r[i]);
	for (i = 0; i < SCALER5_DSPGAMMA_NUM_POINTS; i++, offset += 8)
		vc5_hvs_write_gamma_entry(hvs, offset, &vc4_crtc->pwl_g[i]);
	for (i = 0; i < SCALER5_DSPGAMMA_NUM_POINTS; i++, offset += 8)
		vc5_hvs_write_gamma_entry(hvs, offset, &vc4_crtc->pwl_b[i]);

	if (vc4_state->assigned_channel == 2) {
		/* Alpha only valid on channel 2 */
		for (i = 0; i < SCALER5_DSPGAMMA_NUM_POINTS; i++, offset += 8)
			vc5_hvs_write_gamma_entry(hvs, offset, &vc4_crtc->pwl_a[i]);
	}
}

static void vc5_hvs_update_gamma_lut(struct vc4_hvs *hvs,
				     struct vc4_crtc *vc4_crtc)
{
	struct drm_crtc *crtc = &vc4_crtc->base;
	struct drm_color_lut *lut = crtc->state->gamma_lut->data;
	unsigned int step, i;
	u32 start, end;

#define VC5_HVS_UPDATE_GAMMA_ENTRY_FROM_LUT(pwl, chan)			\
	start = drm_color_lut_extract(lut[i * step].chan, 12);		\
	end = drm_color_lut_extract(lut[(i + 1) * step - 1].chan, 12);	\
									\
	/* Negative gradients not permitted by the hardware, so		\
	 * flatten such points out.					\
	 */								\
	if (end < start)						\
		end = start;						\
									\
	/* Assume 12bit pipeline.					\
	 * X evenly spread over full range (12 bit).			\
	 * C as U12.4 format.						\
	 * Gradient as U4.8 format.					\
	*/								\
	vc4_crtc->pwl[i] =						\
		VC5_HVS_SET_GAMMA_ENTRY(i << 8, start << 4,		\
				((end - start) << 4) / (step - 1))

	/* HVS5 has a 16 point piecewise linear function for each colour
	 * channel (including alpha on channel 2) on each display channel.
	 *
	 * Currently take a crude subsample of the gamma LUT, but this could
	 * be improved to implement curve fitting.
	 */
	step = crtc->gamma_size / SCALER5_DSPGAMMA_NUM_POINTS;
	for (i = 0; i < SCALER5_DSPGAMMA_NUM_POINTS; i++) {
		VC5_HVS_UPDATE_GAMMA_ENTRY_FROM_LUT(pwl_r, red);
		VC5_HVS_UPDATE_GAMMA_ENTRY_FROM_LUT(pwl_g, green);
		VC5_HVS_UPDATE_GAMMA_ENTRY_FROM_LUT(pwl_b, blue);
	}

	vc5_hvs_lut_load(hvs, vc4_crtc);
}

int vc4_hvs_get_fifo_from_output(struct vc4_hvs *hvs, unsigned int output)
{
	struct vc4_dev *vc4 = hvs->vc4;
	u32 reg;
	int ret;

	if (!vc4->is_vc5)
		return output;

	switch (output) {
	case 0:
		return 0;

	case 1:
		return 1;

	case 2:
		reg = HVS_READ(SCALER_DISPECTRL);
		ret = FIELD_GET(SCALER_DISPECTRL_DSP2_MUX_MASK, reg);
		if (ret == 0)
			return 2;

		return 0;

	case 3:
		reg = HVS_READ(SCALER_DISPCTRL);
		ret = FIELD_GET(SCALER_DISPCTRL_DSP3_MUX_MASK, reg);
		if (ret == 3)
			return -EPIPE;

		return ret;

	case 4:
		reg = HVS_READ(SCALER_DISPEOLN);
		ret = FIELD_GET(SCALER_DISPEOLN_DSP4_MUX_MASK, reg);
		if (ret == 3)
			return -EPIPE;

		return ret;

	case 5:
		reg = HVS_READ(SCALER_DISPDITHER);
		ret = FIELD_GET(SCALER_DISPDITHER_DSP5_MUX_MASK, reg);
		if (ret == 3)
			return -EPIPE;

		return ret;

	default:
		return -EPIPE;
	}
}

static int vc4_hvs_init_channel(struct vc4_hvs *hvs, struct drm_crtc *crtc,
				struct drm_display_mode *mode, bool oneshot)
{
	struct vc4_dev *vc4 = hvs->vc4;
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct vc4_crtc_state *vc4_crtc_state = to_vc4_crtc_state(crtc->state);
	unsigned int chan = vc4_crtc_state->assigned_channel;
	bool interlace = mode->flags & DRM_MODE_FLAG_INTERLACE;
	u32 dispbkgndx;
	u32 dispctrl;

	HVS_WRITE(SCALER_DISPCTRLX(chan), 0);
	HVS_WRITE(SCALER_DISPCTRLX(chan), SCALER_DISPCTRLX_RESET);
	HVS_WRITE(SCALER_DISPCTRLX(chan), 0);

	/* Turn on the scaler, which will wait for vstart to start
	 * compositing.
	 * When feeding the transposer, we should operate in oneshot
	 * mode.
	 */
	dispctrl = SCALER_DISPCTRLX_ENABLE;

	if (!vc4->is_vc5)
		dispctrl |= VC4_SET_FIELD(mode->hdisplay,
					  SCALER_DISPCTRLX_WIDTH) |
			    VC4_SET_FIELD(mode->vdisplay,
					  SCALER_DISPCTRLX_HEIGHT) |
			    (oneshot ? SCALER_DISPCTRLX_ONESHOT : 0);
	else
		dispctrl |= VC4_SET_FIELD(mode->hdisplay,
					  SCALER5_DISPCTRLX_WIDTH) |
			    VC4_SET_FIELD(mode->vdisplay,
					  SCALER5_DISPCTRLX_HEIGHT) |
			    (oneshot ? SCALER5_DISPCTRLX_ONESHOT : 0);

	HVS_WRITE(SCALER_DISPCTRLX(chan), dispctrl);

	dispbkgndx = HVS_READ(SCALER_DISPBKGNDX(chan));
	dispbkgndx &= ~SCALER_DISPBKGND_GAMMA;
	dispbkgndx &= ~SCALER_DISPBKGND_INTERLACE;

	if (crtc->state->gamma_lut)
		/* Enable gamma on if required */
		dispbkgndx |= SCALER_DISPBKGND_GAMMA;

	HVS_WRITE(SCALER_DISPBKGNDX(chan), dispbkgndx |
		  SCALER_DISPBKGND_AUTOHS |
		  (interlace ? SCALER_DISPBKGND_INTERLACE : 0));

	/* Reload the LUT, since the SRAMs would have been disabled if
	 * all CRTCs had SCALER_DISPBKGND_GAMMA unset at once.
	 */
	if (!vc4->is_vc5)
		vc4_hvs_lut_load(hvs, vc4_crtc);
	else
		vc5_hvs_lut_load(hvs, vc4_crtc);

	return 0;
}

void vc4_hvs_stop_channel(struct vc4_hvs *hvs, unsigned int chan)
{
	if (HVS_READ(SCALER_DISPCTRLX(chan)) & SCALER_DISPCTRLX_ENABLE)
		return;

	HVS_WRITE(SCALER_DISPCTRLX(chan),
		  HVS_READ(SCALER_DISPCTRLX(chan)) | SCALER_DISPCTRLX_RESET);
	HVS_WRITE(SCALER_DISPCTRLX(chan),
		  HVS_READ(SCALER_DISPCTRLX(chan)) & ~SCALER_DISPCTRLX_ENABLE);

	/* Once we leave, the scaler should be disabled and its fifo empty. */
	WARN_ON_ONCE(HVS_READ(SCALER_DISPCTRLX(chan)) & SCALER_DISPCTRLX_RESET);

	WARN_ON_ONCE(VC4_GET_FIELD(HVS_READ(SCALER_DISPSTATX(chan)),
				   SCALER_DISPSTATX_MODE) !=
		     SCALER_DISPSTATX_MODE_DISABLED);

	WARN_ON_ONCE((HVS_READ(SCALER_DISPSTATX(chan)) &
		      (SCALER_DISPSTATX_FULL | SCALER_DISPSTATX_EMPTY)) !=
		     SCALER_DISPSTATX_EMPTY);
}

static int vc4_hvs_gamma_check(struct drm_crtc *crtc,
			       struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_connector_state *conn_state;
	struct drm_connector *connector;
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	if (!vc4->is_vc5)
		return 0;

	if (!crtc_state->color_mgmt_changed)
		return 0;

	if (crtc_state->gamma_lut) {
		unsigned int len = drm_color_lut_size(crtc_state->gamma_lut);

		if (len != crtc->gamma_size) {
			DRM_DEBUG_KMS("Invalid LUT size; got %u, expected %u\n",
				      len, crtc->gamma_size);
			return -EINVAL;
		}
	}

	connector = vc4_get_crtc_connector(crtc, crtc_state);
	if (!connector)
		return -EINVAL;

	if (!(connector->connector_type == DRM_MODE_CONNECTOR_HDMIA))
		return 0;

	conn_state = drm_atomic_get_connector_state(state, connector);
	if (!conn_state)
		return -EINVAL;

	crtc_state->mode_changed = true;
	return 0;
}

int vc4_hvs_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc_state);
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_plane *plane;
	unsigned long flags;
	const struct drm_plane_state *plane_state;
	u32 dlist_count = 0;
	int ret;

	/* The pixelvalve can only feed one encoder (and encoders are
	 * 1:1 with connectors.)
	 */
	if (hweight32(crtc_state->connector_mask) > 1)
		return -EINVAL;

	drm_atomic_crtc_state_for_each_plane_state(plane, plane_state, crtc_state)
		dlist_count += vc4_plane_dlist_size(plane_state);

	dlist_count++; /* Account for SCALER_CTL0_END. */

	spin_lock_irqsave(&vc4->hvs->mm_lock, flags);
	ret = drm_mm_insert_node(&vc4->hvs->dlist_mm, &vc4_state->mm,
				 dlist_count);
	spin_unlock_irqrestore(&vc4->hvs->mm_lock, flags);
	if (ret)
		return ret;

	return vc4_hvs_gamma_check(crtc, state);
}

static void vc4_hvs_install_dlist(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc->state);

	HVS_WRITE(SCALER_DISPLISTX(vc4_state->assigned_channel),
		  vc4_state->mm.start);
}

static void vc4_hvs_update_dlist(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc->state);
	unsigned long flags;

	if (crtc->state->event) {
		crtc->state->event->pipe = drm_crtc_index(crtc);

		WARN_ON(drm_crtc_vblank_get(crtc) != 0);

		spin_lock_irqsave(&dev->event_lock, flags);

		if (!vc4_crtc->feeds_txp || vc4_state->txp_armed) {
			vc4_crtc->event = crtc->state->event;
			crtc->state->event = NULL;
		}

		spin_unlock_irqrestore(&dev->event_lock, flags);
	}

	spin_lock_irqsave(&vc4_crtc->irq_lock, flags);
	vc4_crtc->current_dlist = vc4_state->mm.start;
	spin_unlock_irqrestore(&vc4_crtc->irq_lock, flags);
}

void vc4_hvs_atomic_begin(struct drm_crtc *crtc,
			  struct drm_atomic_state *state)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc->state);
	unsigned long flags;

	spin_lock_irqsave(&vc4_crtc->irq_lock, flags);
	vc4_crtc->current_hvs_channel = vc4_state->assigned_channel;
	spin_unlock_irqrestore(&vc4_crtc->irq_lock, flags);
}

void vc4_hvs_atomic_enable(struct drm_crtc *crtc,
			   struct drm_atomic_state *state)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	bool oneshot = vc4_crtc->feeds_txp;

	vc4_hvs_install_dlist(crtc);
	vc4_hvs_update_dlist(crtc);
	vc4_hvs_init_channel(vc4->hvs, crtc, mode, oneshot);
}

void vc4_hvs_atomic_disable(struct drm_crtc *crtc,
			    struct drm_atomic_state *state)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_crtc_state *old_state = drm_atomic_get_old_crtc_state(state, crtc);
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(old_state);
	unsigned int chan = vc4_state->assigned_channel;

	vc4_hvs_stop_channel(vc4->hvs, chan);
}

void vc4_hvs_atomic_flush(struct drm_crtc *crtc,
			  struct drm_atomic_state *state)
{
	struct drm_crtc_state *old_state = drm_atomic_get_old_crtc_state(state,
									 crtc);
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc->state);
	unsigned int channel = vc4_state->assigned_channel;
	struct drm_plane *plane;
	struct vc4_plane_state *vc4_plane_state;
	bool debug_dump_regs = false;
	bool enable_bg_fill = false;
	u32 __iomem *dlist_start = vc4->hvs->dlist + vc4_state->mm.start;
	u32 __iomem *dlist_next = dlist_start;
	unsigned int zpos = 0;
	bool found = false;

	if (vc4_state->assigned_channel == VC4_HVS_CHANNEL_DISABLED)
		return;

	if (debug_dump_regs) {
		DRM_INFO("CRTC %d HVS before:\n", drm_crtc_index(crtc));
		vc4_hvs_dump_state(hvs);
	}

	/* Copy all the active planes' dlist contents to the hardware dlist. */
	do {
		found = false;

		drm_atomic_crtc_for_each_plane(plane, crtc) {
			if (plane->state->normalized_zpos != zpos)
				continue;

			/* Is this the first active plane? */
			if (dlist_next == dlist_start) {
				/* We need to enable background fill when a plane
				 * could be alpha blending from the background, i.e.
				 * where no other plane is underneath. It suffices to
				 * consider the first active plane here since we set
				 * needs_bg_fill such that either the first plane
				 * already needs it or all planes on top blend from
				 * the first or a lower plane.
				 */
				vc4_plane_state = to_vc4_plane_state(plane->state);
				enable_bg_fill = vc4_plane_state->needs_bg_fill;
			}

			dlist_next += vc4_plane_write_dlist(plane, dlist_next);

			found = true;
		}

		zpos++;
	} while (found);

	writel(SCALER_CTL0_END, dlist_next);
	dlist_next++;

	WARN_ON_ONCE(dlist_next - dlist_start != vc4_state->mm.size);

	if (enable_bg_fill)
		/* This sets a black background color fill, as is the case
		 * with other DRM drivers.
		 */
		HVS_WRITE(SCALER_DISPBKGNDX(channel),
			  HVS_READ(SCALER_DISPBKGNDX(channel)) |
			  SCALER_DISPBKGND_FILL);

	/* Only update DISPLIST if the CRTC was already running and is not
	 * being disabled.
	 * vc4_crtc_enable() takes care of updating the dlist just after
	 * re-enabling VBLANK interrupts and before enabling the engine.
	 * If the CRTC is being disabled, there's no point in updating this
	 * information.
	 */
	if (crtc->state->active && old_state->active) {
		vc4_hvs_install_dlist(crtc);
		vc4_hvs_update_dlist(crtc);
	}

	if (crtc->state->color_mgmt_changed) {
		u32 dispbkgndx = HVS_READ(SCALER_DISPBKGNDX(channel));

		if (crtc->state->gamma_lut) {
			if (!vc4->is_vc5) {
				vc4_hvs_update_gamma_lut(hvs, vc4_crtc);
				dispbkgndx |= SCALER_DISPBKGND_GAMMA;
			} else {
				vc5_hvs_update_gamma_lut(hvs, vc4_crtc);
			}
		} else {
			/* Unsetting DISPBKGND_GAMMA skips the gamma lut step
			 * in hardware, which is the same as a linear lut that
			 * DRM expects us to use in absence of a user lut.
			 *
			 * Do NOT change state dynamically for hvs5 as it
			 * inserts a delay in the pipeline that will cause
			 * stalls if enabled/disabled whilst running. The other
			 * should already be disabling/enabling the pipeline
			 * when gamma changes.
			 */
			if (!vc4->is_vc5)
				dispbkgndx &= ~SCALER_DISPBKGND_GAMMA;
		}
		HVS_WRITE(SCALER_DISPBKGNDX(channel), dispbkgndx);
	}

	if (debug_dump_regs) {
		DRM_INFO("CRTC %d HVS after:\n", drm_crtc_index(crtc));
		vc4_hvs_dump_state(hvs);
	}
}

void vc4_hvs_mask_underrun(struct vc4_hvs *hvs, int channel)
{
	u32 dispctrl = HVS_READ(SCALER_DISPCTRL);

	dispctrl &= ~SCALER_DISPCTRL_DSPEISLUR(channel);

	HVS_WRITE(SCALER_DISPCTRL, dispctrl);
}

void vc4_hvs_unmask_underrun(struct vc4_hvs *hvs, int channel)
{
	u32 dispctrl = HVS_READ(SCALER_DISPCTRL);

	dispctrl |= SCALER_DISPCTRL_DSPEISLUR(channel);

	HVS_WRITE(SCALER_DISPSTAT,
		  SCALER_DISPSTAT_EUFLOW(channel));
	HVS_WRITE(SCALER_DISPCTRL, dispctrl);
}

static void vc4_hvs_report_underrun(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	atomic_inc(&vc4->underrun);
	DRM_DEV_ERROR(dev->dev, "HVS underrun\n");
}

static irqreturn_t vc4_hvs_irq_handler(int irq, void *data)
{
	struct drm_device *dev = data;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	irqreturn_t irqret = IRQ_NONE;
	int channel;
	u32 control;
	u32 status;

	status = HVS_READ(SCALER_DISPSTAT);
	control = HVS_READ(SCALER_DISPCTRL);

	for (channel = 0; channel < SCALER_CHANNELS_COUNT; channel++) {
		/* Interrupt masking is not always honored, so check it here. */
		if (status & SCALER_DISPSTAT_EUFLOW(channel) &&
		    control & SCALER_DISPCTRL_DSPEISLUR(channel)) {
			vc4_hvs_mask_underrun(hvs, channel);
			vc4_hvs_report_underrun(dev);

			irqret = IRQ_HANDLED;
		}
	}

	/* Clear every per-channel interrupt flag. */
	HVS_WRITE(SCALER_DISPSTAT, SCALER_DISPSTAT_IRQMASK(0) |
				   SCALER_DISPSTAT_IRQMASK(1) |
				   SCALER_DISPSTAT_IRQMASK(2));

	return irqret;
}

static int vc4_hvs_bind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_dev *vc4 = to_vc4_dev(drm);
	struct vc4_hvs *hvs = NULL;
	int ret;
	u32 dispctrl;
	u32 reg;

	hvs = devm_kzalloc(&pdev->dev, sizeof(*hvs), GFP_KERNEL);
	if (!hvs)
		return -ENOMEM;

	hvs->vc4 = vc4;
	hvs->pdev = pdev;

	hvs->regs = vc4_ioremap_regs(pdev, 0);
	if (IS_ERR(hvs->regs))
		return PTR_ERR(hvs->regs);

	hvs->regset.base = hvs->regs;
	hvs->regset.regs = hvs_regs;
	hvs->regset.nregs = ARRAY_SIZE(hvs_regs);

	if (vc4->is_vc5) {
		unsigned long min_rate;
		unsigned long max_rate;

		hvs->core_clk = devm_clk_get(&pdev->dev, NULL);
		if (IS_ERR(hvs->core_clk)) {
			dev_err(&pdev->dev, "Couldn't get core clock\n");
			return PTR_ERR(hvs->core_clk);
		}

		max_rate = clk_get_max_rate(hvs->core_clk);
		if (max_rate >= 550000000)
			hvs->vc5_hdmi_enable_scrambling = true;

		min_rate = clk_get_min_rate(hvs->core_clk);
		if (min_rate >= 600000000)
			hvs->vc5_hdmi_enable_4096by2160 = true;

		ret = clk_prepare_enable(hvs->core_clk);
		if (ret) {
			dev_err(&pdev->dev, "Couldn't enable the core clock\n");
			return ret;
		}
	}

	if (!vc4->is_vc5)
		hvs->dlist = hvs->regs + SCALER_DLIST_START;
	else
		hvs->dlist = hvs->regs + SCALER5_DLIST_START;

	spin_lock_init(&hvs->mm_lock);

	/* Set up the HVS display list memory manager.  We never
	 * overwrite the setup from the bootloader (just 128b out of
	 * our 16K), since we don't want to scramble the screen when
	 * transitioning from the firmware's boot setup to runtime.
	 */
	drm_mm_init(&hvs->dlist_mm,
		    HVS_BOOTLOADER_DLIST_END,
		    (SCALER_DLIST_SIZE >> 2) - HVS_BOOTLOADER_DLIST_END);

	/* Set up the HVS LBM memory manager.  We could have some more
	 * complicated data structure that allowed reuse of LBM areas
	 * between planes when they don't overlap on the screen, but
	 * for now we just allocate globally.
	 */
	if (!vc4->is_vc5)
		/* 48k words of 2x12-bit pixels */
		drm_mm_init(&hvs->lbm_mm, 0, 48 * 1024);
	else
		/* 60k words of 4x12-bit pixels */
		drm_mm_init(&hvs->lbm_mm, 0, 60 * 1024);

	/* Upload filter kernels.  We only have the one for now, so we
	 * keep it around for the lifetime of the driver.
	 */
	ret = vc4_hvs_upload_linear_kernel(hvs,
					   &hvs->mitchell_netravali_filter,
					   mitchell_netravali_1_3_1_3_kernel);
	if (ret)
		return ret;

	vc4->hvs = hvs;

	reg = HVS_READ(SCALER_DISPECTRL);
	reg &= ~SCALER_DISPECTRL_DSP2_MUX_MASK;
	HVS_WRITE(SCALER_DISPECTRL,
		  reg | VC4_SET_FIELD(0, SCALER_DISPECTRL_DSP2_MUX));

	reg = HVS_READ(SCALER_DISPCTRL);
	reg &= ~SCALER_DISPCTRL_DSP3_MUX_MASK;
	HVS_WRITE(SCALER_DISPCTRL,
		  reg | VC4_SET_FIELD(3, SCALER_DISPCTRL_DSP3_MUX));

	reg = HVS_READ(SCALER_DISPEOLN);
	reg &= ~SCALER_DISPEOLN_DSP4_MUX_MASK;
	HVS_WRITE(SCALER_DISPEOLN,
		  reg | VC4_SET_FIELD(3, SCALER_DISPEOLN_DSP4_MUX));

	reg = HVS_READ(SCALER_DISPDITHER);
	reg &= ~SCALER_DISPDITHER_DSP5_MUX_MASK;
	HVS_WRITE(SCALER_DISPDITHER,
		  reg | VC4_SET_FIELD(3, SCALER_DISPDITHER_DSP5_MUX));

	dispctrl = HVS_READ(SCALER_DISPCTRL);

	dispctrl |= SCALER_DISPCTRL_ENABLE;
	dispctrl |= SCALER_DISPCTRL_DISPEIRQ(0) |
		    SCALER_DISPCTRL_DISPEIRQ(1) |
		    SCALER_DISPCTRL_DISPEIRQ(2);

	dispctrl &= ~(SCALER_DISPCTRL_DMAEIRQ |
		      SCALER_DISPCTRL_SLVWREIRQ |
		      SCALER_DISPCTRL_SLVRDEIRQ |
		      SCALER_DISPCTRL_DSPEIEOF(0) |
		      SCALER_DISPCTRL_DSPEIEOF(1) |
		      SCALER_DISPCTRL_DSPEIEOF(2) |
		      SCALER_DISPCTRL_DSPEIEOLN(0) |
		      SCALER_DISPCTRL_DSPEIEOLN(1) |
		      SCALER_DISPCTRL_DSPEIEOLN(2) |
		      SCALER_DISPCTRL_DSPEISLUR(0) |
		      SCALER_DISPCTRL_DSPEISLUR(1) |
		      SCALER_DISPCTRL_DSPEISLUR(2) |
		      SCALER_DISPCTRL_SCLEIRQ);

	HVS_WRITE(SCALER_DISPCTRL, dispctrl);

	ret = devm_request_irq(dev, platform_get_irq(pdev, 0),
			       vc4_hvs_irq_handler, 0, "vc4 hvs", drm);
	if (ret)
		return ret;

	vc4_debugfs_add_regset32(drm, "hvs_regs", &hvs->regset);
	vc4_debugfs_add_file(drm, "hvs_underrun", vc4_hvs_debugfs_underrun,
			     NULL);
	vc4_debugfs_add_file(drm, "hvs_dlists", vc4_hvs_debugfs_dlist,
			     NULL);
	if (vc4->is_vc5)
		vc4_debugfs_add_file(drm, "hvs_gamma", vc5_hvs_debugfs_gamma,
				     NULL);

	return 0;
}

static void vc4_hvs_unbind(struct device *dev, struct device *master,
			   void *data)
{
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_dev *vc4 = to_vc4_dev(drm);
	struct vc4_hvs *hvs = vc4->hvs;

	if (drm_mm_node_allocated(&vc4->hvs->mitchell_netravali_filter))
		drm_mm_remove_node(&vc4->hvs->mitchell_netravali_filter);

	drm_mm_takedown(&vc4->hvs->dlist_mm);
	drm_mm_takedown(&vc4->hvs->lbm_mm);

	clk_disable_unprepare(hvs->core_clk);

	vc4->hvs = NULL;
}

static const struct component_ops vc4_hvs_ops = {
	.bind   = vc4_hvs_bind,
	.unbind = vc4_hvs_unbind,
};

static int vc4_hvs_dev_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &vc4_hvs_ops);
}

static int vc4_hvs_dev_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &vc4_hvs_ops);
	return 0;
}

static const struct of_device_id vc4_hvs_dt_match[] = {
	{ .compatible = "brcm,bcm2711-hvs" },
	{ .compatible = "brcm,bcm2835-hvs" },
	{}
};

struct platform_driver vc4_hvs_driver = {
	.probe = vc4_hvs_dev_probe,
	.remove = vc4_hvs_dev_remove,
	.driver = {
		.name = "vc4_hvs",
		.of_match_table = vc4_hvs_dt_match,
	},
};
