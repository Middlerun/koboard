#include "paint_stb.h"

#include "stb_truetype.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_SPRINTF
#include "stb_image_write.h"

#include <stdlib.h>
#include <string.h>

int stb_canvas_init(StbCanvas *cv, int w, int h, const UiTextCtx *tc)
{
	memset(cv, 0, sizeof(*cv));
	if (w < 1 || h < 1) return -1;
	cv->pix = malloc((size_t) w * (size_t) h);
	if (!cv->pix) return -1;
	cv->w = w;
	cv->h = h;
	cv->tc = tc;
	memset(cv->pix, 0xFF, (size_t) w * (size_t) h);
	return 0;
}

void stb_canvas_free(StbCanvas *cv)
{
	free(cv->pix);
	cv->pix = NULL;
}

void stb_canvas_clear(StbCanvas *cv, uint8_t gray)
{
	memset(cv->pix, gray, (size_t) cv->w * (size_t) cv->h);
}

int stb_canvas_write_png(const StbCanvas *cv, const char *path)
{
	return stbi_write_png(path, cv->w, cv->h, 1, cv->pix, cv->w) ? 0 : -1;
}

// ---- primitives ----------------------------------------------------------

static void cv_fill(void *ctx, UiRect r, uint8_t gray)
{
	StbCanvas *cv = ctx;
	int x0 = r.x < 0 ? 0 : r.x;
	int y0 = r.y < 0 ? 0 : r.y;
	int x1 = r.x + r.w > cv->w ? cv->w : r.x + r.w;
	int y1 = r.y + r.h > cv->h ? cv->h : r.y + r.h;
	for (int y = y0; y < y1; y++)
		memset(cv->pix + (size_t) y * (size_t) cv->w + x0, gray,
		       (size_t) (x1 - x0));
}

static void cv_stroke(void *ctx, UiRect r, int width, uint8_t gray)
{
	if (width < 1) return;
	cv_fill(ctx, (UiRect){ r.x, r.y, r.w, width }, gray);
	cv_fill(ctx, (UiRect){ r.x, r.y + r.h - width, r.w, width }, gray);
	cv_fill(ctx, (UiRect){ r.x, r.y, width, r.h }, gray);
	cv_fill(ctx, (UiRect){ r.x + r.w - width, r.y, width, r.h }, gray);
}

// ---- text ---------------------------------------------------------------

typedef struct {
	StbCanvas *cv;
	int        origin_x, origin_y;   // top-left of the current line box
	uint8_t    gray;
} GlyphCtx;

static void cv_blit(void *ctx, int gx, int gy, int gw, int gh,
		    const unsigned char *cov, int stride)
{
	GlyphCtx *g = ctx;
	StbCanvas *cv = g->cv;
	for (int j = 0; j < gh; j++) {
		int y = g->origin_y + gy + j;
		if (y < 0 || y >= cv->h) continue;
		const unsigned char *src = cov + (size_t) j * (size_t) stride;
		uint8_t *dst = cv->pix + (size_t) y * (size_t) cv->w;
		for (int i = 0; i < gw; i++) {
			int x = g->origin_x + gx + i;
			if (x < 0 || x >= cv->w) continue;
			unsigned a = src[i];
			if (!a) continue;
			// Coverage blend towards the pen colour.
			dst[x] = (uint8_t) ((dst[x] * (255u - a) + g->gray * a) / 255u);
		}
	}
}

typedef struct {
	StbCanvas       *cv;
	const UiTextRun *run;
	UiRect           rect;
	int              line_h;
	int              index;
} LineCtx;

static void cv_line(void *ctx, const char *line, size_t len, int width)
{
	LineCtx *lc = ctx;
	int x = lc->rect.x;
	switch (lc->run->align) {
	case UI_ALIGN_CENTER: x += (lc->rect.w - width) / 2; break;
	case UI_ALIGN_END:    x += lc->rect.w - width; break;
	default: break;
	}
	if (x < lc->rect.x) x = lc->rect.x;

	GlyphCtx g = {
		.cv = lc->cv,
		.origin_x = x,
		.origin_y = lc->rect.y + lc->index * lc->line_h,
		.gray = lc->run->gray,
	};
	ui_text_render_line(lc->cv->tc, line, len, lc->run->size_pt,
			    (UiFontStyle) lc->run->style, cv_blit, &g);
	lc->index++;
}

static void cv_text(void *ctx, UiRect r, const char *text, const UiTextRun *run)
{
	StbCanvas *cv = ctx;
	LineCtx lc = {
		.cv = cv,
		.run = run,
		.rect = r,
		.line_h = ui_text_line_height(cv->tc, run->size_pt,
					      (UiFontStyle) run->style),
		.index = 0,
	};
	ui_text_lines(cv->tc, text, run->size_pt, (UiFontStyle) run->style,
		      r.w, run->max_lines, cv_line, &lc);
}

UiPainter stb_painter(StbCanvas *cv)
{
	return (UiPainter){
		.ctx = cv,
		.fill_rect = cv_fill,
		.stroke_rect = cv_stroke,
		.draw_text = cv_text,
	};
}
