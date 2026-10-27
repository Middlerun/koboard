#include "paint_fbink.h"

#include <stdio.h>
#include <string.h>

static void fb_fill(void *ctx, UiRect r, uint8_t gray)
{
	FbPainter *fp = ctx;
	if (r.w <= 0 || r.h <= 0) return;

	fbink_set_bg_pen_gray(gray, false, false);
	FBInkRect rect = {
		(unsigned short) r.x, (unsigned short) r.y,
		(unsigned short) r.w, (unsigned short) r.h,
	};
	fbink_cls(fp->fbfd, fp->cfg, &rect, false);
	fbink_set_bg_pen_gray(0xFFu, false, false);
}

static void fb_stroke(void *ctx, UiRect r, int width, uint8_t gray)
{
	if (width < 1) return;
	fb_fill(ctx, (UiRect){ r.x, r.y, r.w, width }, gray);
	fb_fill(ctx, (UiRect){ r.x, r.y + r.h - width, r.w, width }, gray);
	fb_fill(ctx, (UiRect){ r.x, r.y, width, r.h }, gray);
	fb_fill(ctx, (UiRect){ r.x + r.w - width, r.y, width, r.h }, gray);
}

static void fb_text(void *ctx, UiRect r, const char *text, const UiTextRun *run)
{
	FbPainter *fp = ctx;
	if (r.w <= 0 || r.h <= 0) return;

	// FBInk positions text with margins against the whole screen, and wraps
	// inside the resulting area. Handing it exactly the rect the layout
	// computed makes it re-derive the same line breaks we measured.
	FBInkOTConfig ot = { 0 };
	ot.margins.top   = (short) r.y;
	ot.margins.left  = (short) r.x;
	ot.margins.right = (short) (fp->screen_w - (r.x + r.w));
	ot.size_pt       = (unsigned short) run->size_pt;
	ot.is_formatted  = false;

	switch (run->style) {
	case UI_FNT_BOLD:        ot.style = FNT_BOLD; break;
	case UI_FNT_ITALIC:      ot.style = FNT_ITALIC; break;
	case UI_FNT_BOLD_ITALIC: ot.style = FNT_BOLD_ITALIC; break;
	default:                 ot.style = FNT_REGULAR; break;
	}

	// Text colour comes from the pen, not from cfg.fg_color: FBInk only maps
	// fg_color onto the pen during init, never per print call.
	FBInkConfig cfg = *fp->cfg;
	// Alignment, on the other hand, is read from the FBInkConfig and not from
	// the OT config -- a quirk worth keeping in exactly one place.
	cfg.is_centered = (run->align == UI_ALIGN_CENTER);
	if (run->align == UI_ALIGN_END) cfg.halign = EDGE;
	else if (run->align == UI_ALIGN_CENTER) cfg.halign = CENTER;
	else cfg.halign = NONE;

	// Both pens matter. FBInk anti-aliases between them, so leaving the
	// background pen white while drawing white text gives a zero-contrast
	// blend -- the glyph box comes out solid white and the label disappears.
	// The background we pass is the one the tree already painted underneath.
	fbink_set_fg_pen_gray(run->gray, false, false);
	fbink_set_bg_pen_gray(run->bg_gray, false, false);

	FBInkOTFit fit = { 0 };
	int bottom = fbink_print_ot(fp->fbfd, text, &ot, &cfg, &fit);

	fbink_set_fg_pen_gray(0x00u, false, false);
	fbink_set_bg_pen_gray(0xFFu, false, false);

	if (bottom < 0) {
		fprintf(stderr, "koboard: print_ot failed (%d) at %d,%d %dx%d "
			"%dpt fg=%u bg=%u: %.40s\n",
			bottom, r.x, r.y, r.w, r.h, run->size_pt,
			run->gray, run->bg_gray, text);
		return;
	}
	// A disagreement here means our measurement and FBInk's renderer parted
	// ways, which is the one thing the host preview cannot catch. Log it
	// rather than let the layout drift silently.
	if (fit.rendered_lines && fit.rendered_lines != run->max_lines) {
		fp->mismatches++;
		fprintf(stderr, "koboard: line mismatch (measured %d, drew %u) "
			"at %d,%d %dx%d %dpt: %.40s\n",
			run->max_lines, fit.rendered_lines,
			r.x, r.y, r.w, r.h, run->size_pt, text);
	}
}

UiPainter fbink_painter(FbPainter *fp)
{
	return (UiPainter){
		.ctx = fp,
		.fill_rect = fb_fill,
		.stroke_rect = fb_stroke,
		.draw_text = fb_text,
	};
}
