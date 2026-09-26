// Text measurement and shaping, shared by the device and the host preview.
//
// This deliberately re-implements FBInk's OpenType line-breaking pass rather
// than calling fbink_print_ot(compute_only), for two reasons: compute_only
// reports a line count but not a width (so intrinsic sizing is impossible),
// and the host preview needs the same numbers without a framebuffer.
//
// The arithmetic mirrors fbink.c's print_ot codepath exactly, so what we
// measure is what FBInk draws:
//   size_px    = round(dpi / 72 * size_pt)
//   scale      = stbtt_ScaleForPixelHeight(font, size_px)
//   baseline   = ceil(scale * ascent)
//   descent    = ceil(scale * descent)      (negative)
//   line_gap   = ceil(scale * lineGap)
//   line_h     = baseline + |descent| + line_gap
//   advance    = round(scale * adv) + round(scale * kern)
// and lines are found by first fit over libunibreak's break opportunities.

#ifndef HOMESRV_UI_TEXT_H
#define HOMESRV_UI_TEXT_H

#include <stdbool.h>
#include <stddef.h>

#include "ui.h"

#define UI_FONT_STYLES 4

typedef struct UiFont UiFont;

typedef struct {
	UiFont *fonts[UI_FONT_STYLES];
	int     dpi;
} UiTextCtx;

typedef struct {
	int    w;          // widest line
	int    h;          // lines * line_h
	int    line_h;
	int    lines;
	bool   truncated;
} UiTextMetrics;

int  ui_text_init(UiTextCtx *tc, int dpi);
int  ui_text_load(UiTextCtx *tc, UiFontStyle style, const char *path);
void ui_text_free(UiTextCtx *tc);

// Lay out `text` into at most `max_w` pixels and, when max_lines > 0, at most
// that many lines. On truncation the text is ellipsized into `arena` unless
// no_ellipsis is set; *out_text always points at the string to draw.
void ui_text_measure(const UiTextCtx *tc, UiArena *arena,
		     const char *text, int size_pt, UiFontStyle style,
		     int max_w, int max_lines, bool no_ellipsis,
		     UiTextMetrics *out, const char **out_text);

// Row height for a size/style, independent of any string. Used to reserve
// space for a line that has not been composed yet.
int ui_text_line_height(const UiTextCtx *tc, int size_pt, UiFontStyle style);

// Pixel size FBInk will use for a point size at this DPI.
int ui_text_size_px(const UiTextCtx *tc, int size_pt);

// --- for painters that rasterise themselves (the host preview) -------------

// Break `text` into lines exactly as ui_text_measure did, invoking `emit` per
// line with its byte range and pixel width.
void ui_text_lines(const UiTextCtx *tc, const char *text, int size_pt,
		   UiFontStyle style, int max_w, int max_lines,
		   void (*emit)(void *ctx, const char *line, size_t len, int width),
		   void *ctx);

// Rasterise one line as an 8-bit coverage mask via a per-glyph callback.
// `blit` receives a glyph bitmap positioned relative to the line's origin,
// where y is measured down from the top of the line box.
void ui_text_render_line(const UiTextCtx *tc, const char *line, size_t len,
			 int size_pt, UiFontStyle style,
			 void (*blit)(void *ctx, int x, int y, int w, int h,
				      const unsigned char *cov, int stride),
			 void *ctx);

#endif
