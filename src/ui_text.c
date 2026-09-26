#include "ui_text.h"

#include "stb_truetype.h"
#include "linebreak.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UI_MAX_LINES 64
#define UI_ELLIPSIS  "…"

struct UiFont {
	stbtt_fontinfo  info;
	unsigned char  *data;
};

// FBInk rounds with these, and rounding direction changes break positions, so
// match it exactly.
static int iroundf(float f) { return (int) lroundf(f); }
static int iceilf(float f)  { return (int) ceilf(f); }

// ---- UTF-8 ---------------------------------------------------------------

static int u8_len(unsigned char b)
{
	if (b < 0x80) return 1;
	if ((b & 0xE0) == 0xC0) return 2;
	if ((b & 0xF0) == 0xE0) return 3;
	if ((b & 0xF8) == 0xF0) return 4;
	return 1;   // invalid lead byte: consume it so we always make progress
}

static uint32_t u8_next(const char *s, size_t *i)
{
	const unsigned char *p = (const unsigned char *) s + *i;
	int n = u8_len(p[0]);
	uint32_t c;
	switch (n) {
	case 2:  c = (uint32_t) (p[0] & 0x1F); break;
	case 3:  c = (uint32_t) (p[0] & 0x0F); break;
	case 4:  c = (uint32_t) (p[0] & 0x07); break;
	default: c = p[0]; break;
	}
	for (int k = 1; k < n; k++) {
		if ((p[k] & 0xC0) != 0x80) { n = k; break; }
		c = (c << 6) | (uint32_t) (p[k] & 0x3F);
	}
	*i += (size_t) n;
	return c;
}

// Step *i back to the start of the preceding codepoint.
static void u8_dec(const char *s, size_t *i)
{
	if (*i == 0) return;
	(*i)--;
	while (*i > 0 && ((unsigned char) s[*i] & 0xC0) == 0x80)
		(*i)--;
}

// ---- metrics -------------------------------------------------------------

typedef struct {
	stbtt_fontinfo *f;
	float           sf;
	int             size_px;
	int             baseline;   // max across all loaded styles, as FBInk does
	int             descent;    // negative
	int             line_gap;
	int             row_h;      // baseline + |descent| + line_gap
} UiMetrics;

static int ui_metrics(const UiTextCtx *tc, int size_pt, UiFontStyle style,
		      UiMetrics *m)
{
	memset(m, 0, sizeof(*m));

	if (size_pt <= 0) size_pt = 12;
	m->size_px = iroundf((float) tc->dpi / 72.0f * (float) size_pt);
	if (m->size_px < 1) m->size_px = 1;

	// Fall back to regular for styles that were not loaded, rather than
	// silently drawing nothing.
	UiFont *want = tc->fonts[style];
	if (!want) want = tc->fonts[UI_FNT_REGULAR];
	if (!want) return -1;
	m->f  = &want->info;
	m->sf = stbtt_ScaleForPixelHeight(m->f, (float) m->size_px);

	// FBInk derives one row height from the tallest of *all* loaded fonts, so
	// regular and bold text at the same point size share a baseline.
	for (int i = 0; i < UI_FONT_STYLES; i++) {
		if (!tc->fonts[i]) continue;
		stbtt_fontinfo *f = &tc->fonts[i]->info;
		float sf = stbtt_ScaleForPixelHeight(f, (float) m->size_px);
		int asc, desc, lg;
		stbtt_GetFontVMetrics(f, &asc, &desc, &lg);
		int s_bl = iceilf(sf * (float) asc);
		int s_de = iceilf(sf * (float) desc);
		int s_lg = iceilf(sf * (float) lg);
		if (s_bl > m->baseline)  m->baseline = s_bl;
		if (s_de < m->descent)   m->descent  = s_de;
		if (s_lg > m->line_gap)  m->line_gap = s_lg;
	}
	m->row_h = m->baseline + abs(m->descent) + m->line_gap;
	if (m->row_h < 1) m->row_h = 1;
	return 0;
}

// ---- line breaking -------------------------------------------------------

typedef struct {
	size_t start;   // byte offset, inclusive
	size_t end;     // byte offset, exclusive
} UiLine;

// Width in pixels of one byte range, using FBInk's ink-extent rule: the line
// ends at the right edge of the last glyph's bitmap, not at its advance.
static int ui_line_width(const UiMetrics *m, const char *s,
			 size_t start, size_t end)
{
	int curr_x = 0, lw = 0;
	size_t i = start;
	while (i < end) {
		uint32_t c = u8_next(s, &i);
		int gi = stbtt_FindGlyphIndex(m->f, (int) c);
		int adv, lsb, x0, y0, x1, y1;
		stbtt_GetGlyphHMetrics(m->f, gi, &adv, &lsb);
		stbtt_GetGlyphBitmapBox(m->f, gi, m->sf, m->sf, &x0, &y0, &x1, &y1);
		int gw = x1 - x0;
		if (curr_x + x0 < 0) curr_x += abs(curr_x + x0);
		lw = gw ? curr_x + x0 + gw : curr_x;
		curr_x += iroundf(m->sf * (float) adv);
		if (i < end) {
			size_t j = i;
			uint32_t c2 = u8_next(s, &j);
			int g2i = stbtt_FindGlyphIndex(m->f, (int) c2);
			curr_x += iroundf(m->sf * (float) stbtt_GetGlyphKernAdvance(m->f, gi, g2i));
		}
	}
	return lw;
}

// First-fit line breaking over libunibreak's opportunities.
//
// NOTE: like FBInk, we look up brks[] at each character's *first* byte. For
// multi-byte characters libunibreak records the opportunity at the *last*
// byte, so this misses breaks after non-ASCII characters. That is a faithful
// copy of FBInk's behaviour, and being no more permissive than the renderer is
// what keeps measured widths safe: finding an extra break here would make us
// measure a line narrower than FBInk goes on to draw.
static int ui_break_lines(const UiMetrics *m, const char *s, int max_w,
			  int max_lines, UiLine *out, bool *truncated)
{
	size_t n = strlen(s);
	*truncated = false;
	if (n == 0) return 0;
	if (max_w < 1) max_w = 1;

	int cap = (max_lines > 0 && max_lines < UI_MAX_LINES) ? max_lines : UI_MAX_LINES;

	char *brk = calloc(n + 1, 1);
	if (!brk) return 0;
	set_linebreaks_utf8((const utf8_t *) s, n + 1, "en", brk);

	size_t i = 0;
	int nl = 0;
	while (i < n && nl < cap) {
		size_t line_start = i;
		size_t line_end   = i;
		int    curr_x     = 0;
		bool   closed     = false;

		while (i < n) {
			if (brk[i] == LINEBREAK_MUSTBREAK) {
				// Break before this character and consume it.
				line_end = i;
				u8_next(s, &i);
				closed = true;
				break;
			}

			size_t cstart = i;
			uint32_t c = u8_next(s, &i);
			int gi = stbtt_FindGlyphIndex(m->f, (int) c);
			int adv, lsb, x0, y0, x1, y1;
			stbtt_GetGlyphHMetrics(m->f, gi, &adv, &lsb);
			stbtt_GetGlyphBitmapBox(m->f, gi, m->sf, m->sf,
						&x0, &y0, &x1, &y1);
			int gw = x1 - x0;
			if (curr_x + x0 < 0) curr_x += abs(curr_x + x0);
			int lw = gw ? curr_x + x0 + gw : curr_x;

			if (lw > max_w) {
				if (cstart == line_start) {
					// A single glyph wider than the box. Keep it so we
					// always make progress; it will be clipped.
					line_end = i;
					closed = true;
					break;
				}
				// Walk back to the last break opportunity on this line.
				size_t j = cstart, found = 0;
				bool have = false;
				while (j > line_start) {
					u8_dec(s, &j);
					if (brk[j] == LINEBREAK_ALLOWBREAK) {
						found = j;
						have = true;
						break;
					}
				}
				if (have) {
					// Break after the opportunity character, dropping it
					// (it is whitespace) from the rendered line.
					line_end = found;
					i = found;
					u8_next(s, &i);
				} else {
					// No opportunity: hard break before this character.
					line_end = cstart;
					i = cstart;
				}
				closed = true;
				break;
			}

			curr_x += iroundf(m->sf * (float) adv);
			if (i < n) {
				size_t j = i;
				uint32_t c2 = u8_next(s, &j);
				int g2i = stbtt_FindGlyphIndex(m->f, (int) c2);
				curr_x += iroundf(m->sf *
					(float) stbtt_GetGlyphKernAdvance(m->f, gi, g2i));
			}
			line_end = i;
		}

		if (!closed) line_end = i;   // ran out of string

		out[nl].start = line_start;
		out[nl].end   = line_end;
		nl++;
	}

	*truncated = (i < n);
	free(brk);
	return nl;
}

// ---- public API ----------------------------------------------------------

int ui_text_init(UiTextCtx *tc, int dpi)
{
	memset(tc, 0, sizeof(*tc));
	tc->dpi = dpi > 0 ? dpi : 212;
	return 0;
}

int ui_text_load(UiTextCtx *tc, UiFontStyle style, const char *path)
{
	if ((int) style < 0 || style >= UI_FONT_STYLES) return -1;

	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
	long size = ftell(f);
	if (size <= 0) { fclose(f); return -1; }
	rewind(f);

	unsigned char *data = malloc((size_t) size);
	if (!data) { fclose(f); return -1; }
	if (fread(data, 1, (size_t) size, f) != (size_t) size) {
		free(data); fclose(f); return -1;
	}
	fclose(f);

	UiFont *font = calloc(1, sizeof(*font));
	if (!font) { free(data); return -1; }
	font->data = data;
	if (!stbtt_InitFont(&font->info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
		free(font); free(data); return -1;
	}

	if (tc->fonts[style]) {
		free(tc->fonts[style]->data);
		free(tc->fonts[style]);
	}
	tc->fonts[style] = font;
	return 0;
}

void ui_text_free(UiTextCtx *tc)
{
	for (int i = 0; i < UI_FONT_STYLES; i++) {
		if (!tc->fonts[i]) continue;
		free(tc->fonts[i]->data);
		free(tc->fonts[i]);
		tc->fonts[i] = NULL;
	}
}

int ui_text_size_px(const UiTextCtx *tc, int size_pt)
{
	if (size_pt <= 0) size_pt = 12;
	return iroundf((float) tc->dpi / 72.0f * (float) size_pt);
}

int ui_text_line_height(const UiTextCtx *tc, int size_pt, UiFontStyle style)
{
	UiMetrics m;
	if (ui_metrics(tc, size_pt, style, &m) < 0) return size_pt * 2;
	return m.row_h;
}

void ui_text_measure(const UiTextCtx *tc, UiArena *arena,
		     const char *text, int size_pt, UiFontStyle style,
		     int max_w, int max_lines, bool no_ellipsis,
		     UiTextMetrics *out, const char **out_text)
{
	memset(out, 0, sizeof(*out));
	if (out_text) *out_text = text;
	if (!text || !*text) return;

	UiMetrics m;
	if (ui_metrics(tc, size_pt, style, &m) < 0) return;
	out->line_h = m.row_h;

	UiLine lines[UI_MAX_LINES];
	bool truncated = false;
	int nl = ui_break_lines(&m, text, max_w, max_lines, lines, &truncated);
	if (nl < 1) return;

	out->lines     = nl;
	out->truncated = truncated;
	// Height is lines * full row height, trailing line gap included. FBInk
	// decides how many lines fit by dividing the available height by that same
	// row height, so a box measured this way can never make it drop a line.
	out->h = nl * m.row_h;

	if (truncated && !no_ellipsis && arena) {
		// Trim the tail of the last line until it plus an ellipsis fits.
		size_t ls = lines[nl - 1].start;
		size_t le = lines[nl - 1].end;
		// One scratch buffer for the whole search: trimming a character at a
		// time would otherwise allocate a fresh copy per iteration.
		char *buf = ui_alloc(arena, (le - ls) + sizeof(UI_ELLIPSIS));
		while (le > ls) {
			size_t trial = le;
			// Drop trailing whitespace before measuring, so the ellipsis sits
			// snug against the last word.
			while (trial > ls && (text[trial - 1] == ' ' || text[trial - 1] == '\t'))
				trial--;
			size_t keep = trial - ls;
			memcpy(buf, text + ls, keep);
			memcpy(buf + keep, UI_ELLIPSIS, sizeof(UI_ELLIPSIS));
			if (ui_line_width(&m, buf, 0, strlen(buf)) <= max_w || keep == 0) {
				// Splice the trimmed last line back onto the earlier ones.
				size_t head = ls;
				size_t tail = strlen(buf);
				char *full = ui_alloc(arena, head + tail + 1);
				memcpy(full, text, head);
				memcpy(full + head, buf, tail + 1);
				if (out_text) *out_text = full;
				lines[nl - 1].end = head + tail;
				break;
			}
			u8_dec(text, &le);
		}
	}

	const char *measured = out_text ? *out_text : text;
	for (int i = 0; i < nl; i++) {
		int w = ui_line_width(&m, measured, lines[i].start, lines[i].end);
		if (w > out->w) out->w = w;
	}
}

void ui_text_lines(const UiTextCtx *tc, const char *text, int size_pt,
		   UiFontStyle style, int max_w, int max_lines,
		   void (*emit)(void *ctx, const char *line, size_t len, int width),
		   void *ctx)
{
	if (!text || !*text) return;
	UiMetrics m;
	if (ui_metrics(tc, size_pt, style, &m) < 0) return;

	UiLine lines[UI_MAX_LINES];
	bool truncated;
	int nl = ui_break_lines(&m, text, max_w, max_lines, lines, &truncated);
	for (int i = 0; i < nl; i++) {
		int w = ui_line_width(&m, text, lines[i].start, lines[i].end);
		emit(ctx, text + lines[i].start, lines[i].end - lines[i].start, w);
	}
}

void ui_text_render_line(const UiTextCtx *tc, const char *line, size_t len,
			 int size_pt, UiFontStyle style,
			 void (*blit)(void *ctx, int x, int y, int w, int h,
				      const unsigned char *cov, int stride),
			 void *ctx)
{
	UiMetrics m;
	if (ui_metrics(tc, size_pt, style, &m) < 0) return;

	int curr_x = 0;
	size_t i = 0;
	while (i < len) {
		uint32_t c = u8_next(line, &i);
		int gi = stbtt_FindGlyphIndex(m.f, (int) c);
		int adv, lsb, x0, y0, x1, y1;
		stbtt_GetGlyphHMetrics(m.f, gi, &adv, &lsb);
		stbtt_GetGlyphBitmapBox(m.f, gi, m.sf, m.sf, &x0, &y0, &x1, &y1);
		int gw = x1 - x0, gh = y1 - y0;
		if (curr_x + x0 < 0) curr_x += abs(curr_x + x0);

		if (gw > 0 && gh > 0) {
			unsigned char *bm = malloc((size_t) (gw * gh));
			if (bm) {
				stbtt_MakeGlyphBitmap(m.f, bm, gw, gh, gw, m.sf, m.sf, gi);
				blit(ctx, curr_x + x0, m.baseline + y0, gw, gh, bm, gw);
				free(bm);
			}
		}

		curr_x += iroundf(m.sf * (float) adv);
		if (i < len) {
			size_t j = i;
			uint32_t c2 = u8_next(line, &j);
			int g2i = stbtt_FindGlyphIndex(m.f, (int) c2);
			curr_x += iroundf(m.sf *
				(float) stbtt_GetGlyphKernAdvance(m.f, gi, g2i));
		}
	}
}
