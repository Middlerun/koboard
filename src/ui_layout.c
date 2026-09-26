#include "ui_layout.h"

#include <stdio.h>
#include <string.h>

#define UI_OVERFLOW_KEY "__overflow__"

// Slack added to an intrinsically sized text node's width; see measure_text.
#define UI_TEXT_SLACK 2

typedef struct {
	const UiTextCtx *tc;
	UiArena         *arena;
} Ctx;

static int pad_main_lead(const UiNode *n)
{
	return n->border + (n->dir == UI_ROW ? n->pad[3] : n->pad[0]);
}
static int pad_main_trail(const UiNode *n)
{
	return n->border + (n->dir == UI_ROW ? n->pad[1] : n->pad[2]);
}
static int pad_cross_lead(const UiNode *n)
{
	return n->border + (n->dir == UI_ROW ? n->pad[0] : n->pad[3]);
}
static int pad_cross_trail(const UiNode *n)
{
	return n->border + (n->dir == UI_ROW ? n->pad[2] : n->pad[1]);
}

// Resolve a length against the space the parent offered. Returns -1 for AUTO,
// meaning "ask the content".
static int resolve_len(UiLen l, int avail)
{
	switch (l.kind) {
	case UI_LEN_PX:  return l.val < 0 ? 0 : l.val;
	case UI_LEN_PCT: return avail > 0 ? avail * l.val / 100 : 0;
	default:         return -1;   // AUTO, and FLEX which the parent resolves
	}
}

static void measure_node(Ctx *c, UiNode *n, int max_w, int max_h,
			 int fix_w, int fix_h);

// ---- text ----------------------------------------------------------------

static void measure_text(Ctx *c, UiNode *n, int max_w, int max_h,
			 int fix_w, int fix_h)
{
	int pad_h = n->pad[1] + n->pad[3] + 2 * n->border;
	int pad_v = n->pad[0] + n->pad[2] + 2 * n->border;

	int box_w = fix_w >= 0 ? fix_w : resolve_len(n->w, max_w);
	int box_h = fix_h >= 0 ? fix_h : resolve_len(n->h, max_h);

	int inner_w = (box_w >= 0 ? box_w : max_w) - pad_h;
	if (inner_w < 1) inner_w = 1;

	// A definite height caps the line count, which is what keeps a long title
	// from running past the bottom of its card without any manual y checks.
	int max_lines = n->max_lines;
	if (box_h >= 0) {
		int row = ui_text_line_height(c->tc, n->size_pt, n->style);
		int fit = row > 0 ? (box_h - pad_v) / row : 0;
		if (fit < 1) fit = 1;
		if (max_lines == 0 || fit < max_lines) max_lines = fit;
	}

	UiTextMetrics tm;
	const char *drawn = n->text;
	ui_text_measure(c->tc, c->arena, n->text, n->size_pt, n->style,
			inner_w, max_lines, n->no_ellipsis, &tm, &drawn);

	n->draw_text = drawn;
	n->line_h    = (int16_t) tm.line_h;
	n->lines     = (uint8_t) (tm.lines > 255 ? 255 : tm.lines);
	n->truncated = tm.truncated;

	if (box_w < 0) {
		// Round the intrinsic width up a little. Sizing a node to the exact
		// measured ink width leaves no room for FBInk's own measurement to
		// land a pixel wider, and when it does its centring maths underflows
		// (it subtracts in unsigned) and the text is drawn off-screen.
		box_w = tm.w + pad_h + UI_TEXT_SLACK;
		if (box_w > max_w) box_w = max_w;
	}
	if (box_h < 0) box_h = tm.h + pad_v;

	n->rect.w = box_w;
	n->rect.h = box_h;
}

// ---- box ----------------------------------------------------------------

static bool is_main_flex(const UiNode *parent, const UiNode *child)
{
	UiLen l = (parent->dir == UI_ROW) ? child->w : child->h;
	return l.kind == UI_LEN_FLEX;
}

// Stretch only applies to children that did not ask for a cross-axis size of
// their own, which is what lets a fixed-width button sit in a stretched column.
static bool cross_is_auto(const UiNode *parent, const UiNode *child)
{
	UiLen l = (parent->dir == UI_ROW) ? child->h : child->w;
	return l.kind == UI_LEN_AUTO;
}

// Resolve a child's cross-axis size. Percentages are relative to the parent's
// whole content box, never to whatever space happens to be left.
static int child_cross_size(const UiNode *parent, const UiNode *child,
			    bool cross_definite, int cont_cross)
{
	UiLen l = (parent->dir == UI_ROW) ? child->h : child->w;
	switch (l.kind) {
	case UI_LEN_PX:  return l.val < 0 ? 0 : l.val;
	case UI_LEN_PCT: return cross_definite ? cont_cross * l.val / 100 : -1;
	case UI_LEN_FLEX:
		// Flex has no meaning across the axis; treat it as "fill".
		return cross_definite ? cont_cross : -1;
	default:
		return (parent->cross == UI_CROSS_STRETCH && cross_definite)
			       ? cont_cross : -1;
	}
}

// Same for the main axis. FLEX is left to the second pass, which knows how
// much room is actually spare.
static int child_main_size(const UiNode *parent, const UiNode *child,
			   bool main_definite, int cont_main)
{
	UiLen l = (parent->dir == UI_ROW) ? child->w : child->h;
	switch (l.kind) {
	case UI_LEN_PX:  return l.val < 0 ? 0 : l.val;
	case UI_LEN_PCT: return main_definite ? cont_main * l.val / 100 : -1;
	default:         return -1;
	}
}

static int child_main(const UiNode *parent, const UiNode *child)
{
	return (parent->dir == UI_ROW) ? child->rect.w : child->rect.h;
}
static int child_cross(const UiNode *parent, const UiNode *child)
{
	return (parent->dir == UI_ROW) ? child->rect.h : child->rect.w;
}

// Measure one child, mapping main/cross back onto width/height.
static void measure_child(Ctx *c, UiNode *parent, UiNode *child,
			  int max_main, int max_cross,
			  int fix_main, int fix_cross)
{
	if (parent->dir == UI_ROW)
		measure_node(c, child, max_main, max_cross, fix_main, fix_cross);
	else
		measure_node(c, child, max_cross, max_main, fix_cross, fix_main);
}

// Point size for a container's overflow indicator: the last text size used in
// the subtree, so the indicator matches the content it stands in for.
static int overflow_size_pt(const UiNode *n)
{
	int size_pt = 9;
	for (const UiNode *ch = n->first; ch; ch = ch->next) {
		if (ch->kind == UI_TEXT && ch->size_pt) size_pt = ch->size_pt;
		else if (ch->kind == UI_BOX && ch->first) size_pt = overflow_size_pt(ch);
	}
	return size_pt;
}

static UiNode *overflow_node(Ctx *c, UiNode *n, int dropped)
{
	if (!n->overflow_text || dropped < 1) return NULL;

	// "{n}" -> the number of children that were dropped.
	char buf[256];
	const char *src = n->overflow_text;
	const char *ph = strstr(src, "{n}");
	if (ph) {
		int head = (int) (ph - src);
		snprintf(buf, sizeof(buf), "%.*s%d%s", head, src, dropped, ph + 3);
	} else {
		snprintf(buf, sizeof(buf), "%s", src);
	}

	int size_pt = overflow_size_pt(n);

	return ui_new(c->arena, &(UiNode){
		.kind = UI_TEXT, .key = UI_OVERFLOW_KEY,
		.text = buf, .size_pt = (uint16_t) size_pt, .max_lines = 1,
	});
}

static void measure_box(Ctx *c, UiNode *n, int max_w, int max_h,
			int fix_w, int fix_h)
{
	int box_w = fix_w >= 0 ? fix_w : resolve_len(n->w, max_w);
	int box_h = fix_h >= 0 ? fix_h : resolve_len(n->h, max_h);

	bool row = (n->dir == UI_ROW);
	int  own_main   = row ? box_w : box_h;
	int  own_cross  = row ? box_h : box_w;
	int  avail_main = row ? max_w : max_h;
	int  avail_cross = row ? max_h : max_w;

	int lead_m = pad_main_lead(n),  trail_m = pad_main_trail(n);
	int lead_c = pad_cross_lead(n), trail_c = pad_cross_trail(n);

	// The content box we can offer children.
	int cont_main = (own_main >= 0 ? own_main : avail_main) - lead_m - trail_m;
	int cont_cross = (own_cross >= 0 ? own_cross : avail_cross) - lead_c - trail_c;
	if (cont_main < 0)  cont_main = 0;
	if (cont_cross < 0) cont_cross = 0;
	bool main_definite  = own_main >= 0;
	bool cross_definite = own_cross >= 0;

	int nkids = 0;
	for (UiNode *ch = n->first; ch; ch = ch->next) { ch->dropped = false; nkids++; }
	int gaps = nkids > 1 ? (nkids - 1) * n->gap : 0;

	// --- pass 1: non-flex children ---
	int fixed_total = 0, flex_total = 0;
	for (UiNode *ch = n->first; ch; ch = ch->next) {
		bool flex = is_main_flex(n, ch) && main_definite;
		if (flex) {
			flex_total += ((row ? ch->w : ch->h).val > 0)
					? (row ? ch->w : ch->h).val : 1;
			continue;
		}
		int fix_cross = child_cross_size(n, ch, cross_definite, cont_cross);
		int fix_main  = child_main_size(n, ch, main_definite, cont_main);
		int room = cont_main - fixed_total - gaps;
		if (room < 0) room = 0;
		measure_child(c, n, ch, main_definite ? room : cont_main,
			      cont_cross, fix_main, fix_cross);
		fixed_total += child_main(n, ch);
	}

	// --- pass 2: distribute leftover main-axis space among flex children ---
	if (flex_total > 0) {
		int leftover = cont_main - fixed_total - gaps;
		if (leftover < 0) leftover = 0;
		int given = 0, seen = 0;
		for (UiNode *ch = n->first; ch; ch = ch->next) {
			if (!(is_main_flex(n, ch) && main_definite)) continue;
			int weight = (row ? ch->w : ch->h).val;
			if (weight <= 0) weight = 1;
			seen += weight;
			// Hand the last one the remainder so the total is exact.
			int share = (seen == flex_total) ? leftover - given
							 : leftover * weight / flex_total;
			given += share;
			int fix_cross = child_cross_size(n, ch, cross_definite, cont_cross);
			measure_child(c, n, ch, share, cont_cross, share, fix_cross);
		}
	}

	// --- cross size, and a stretch pass once it is known ---
	int max_child_cross = 0;
	for (UiNode *ch = n->first; ch; ch = ch->next) {
		int cc = child_cross(n, ch);
		if (cc > max_child_cross) max_child_cross = cc;
	}
	if (!cross_definite) {
		cont_cross = max_child_cross;
		if (n->cross == UI_CROSS_STRETCH) {
			// Now that the cross size is known, stretch children to it.
			for (UiNode *ch = n->first; ch; ch = ch->next) {
				if (!cross_is_auto(n, ch)) continue;
				if (child_cross(n, ch) == cont_cross) continue;
				// Pin the main-axis size we already settled on: this pass
				// exists only to grow the cross axis, and re-deriving the
				// main size here would resolve percentages against the
				// child's own width instead of the parent's content box.
				measure_child(c, n, ch, child_main(n, ch), cont_cross,
					      child_main(n, ch), cont_cross);
			}
		}
	}

	// --- clipping ---
	int used_main = gaps;
	for (UiNode *ch = n->first; ch; ch = ch->next)
		used_main += child_main(n, ch);

	if (n->clip && main_definite && used_main > cont_main) {
		int dropped = 0, budget = cont_main;

		// Two attempts: the first tells us whether anything overflows, the
		// second re-runs with room reserved for the overflow indicator.
		for (int attempt = 0; attempt < 2; attempt++) {
			dropped = 0;
			int acc = 0;
			bool cutting = false;
			for (UiNode *ch = n->first; ch; ch = ch->next) {
				if (ch->key && strcmp(ch->key, UI_OVERFLOW_KEY) == 0) continue;
				int need = acc ? n->gap : 0;
				if (cutting || acc + need + child_main(n, ch) > budget) {
					cutting = true;
					ch->dropped = true;
					dropped++;
					continue;
				}
				ch->dropped = false;
				acc += need + child_main(n, ch);
			}
			if (attempt == 0 && dropped > 0 && n->overflow_text) {
				int row_h = ui_text_line_height(
					c->tc, overflow_size_pt(n), UI_FNT_REGULAR);
				budget = cont_main - row_h - n->gap;
				if (budget < 0) budget = 0;
				continue;
			}
			break;
		}

		if (dropped > 0 && n->overflow_text) {
			UiNode *last = n->last;
			bool have = last && last->key &&
				    strcmp(last->key, UI_OVERFLOW_KEY) == 0;
			if (!have) {
				UiNode *ov = overflow_node(c, n, dropped);
				if (ov) {
					ui_add(n, ov);
					int fix_cross = child_cross_size(n, ov, true, cont_cross);
					measure_child(c, n, ov, cont_main, cont_cross,
						      -1, fix_cross);
				}
			}
		}

		used_main = 0;
		for (UiNode *ch = n->first; ch; ch = ch->next) {
			if (ch->dropped) continue;
			if (used_main) used_main += n->gap;
			used_main += child_main(n, ch);
		}
	}

	// A definite length already includes padding and border; only a size we
	// derived from the children needs them added.
	int final_main  = main_definite  ? own_main
					 : used_main + lead_m + trail_m;
	int final_cross = cross_definite ? own_cross
					 : max_child_cross + lead_c + trail_c;
	n->rect.w = row ? final_main : final_cross;
	n->rect.h = row ? final_cross : final_main;
}

static void measure_node(Ctx *c, UiNode *n, int max_w, int max_h,
			 int fix_w, int fix_h)
{
	if (max_w < 0) max_w = 0;
	if (max_h < 0) max_h = 0;
	if (n->kind == UI_TEXT) measure_text(c, n, max_w, max_h, fix_w, fix_h);
	else                    measure_box(c, n, max_w, max_h, fix_w, fix_h);
}

// ---- placement ----------------------------------------------------------

static void place(UiNode *n, int x, int y, uint8_t inherited_fg, uint8_t inherited_bg)
{
	n->rect.x = x;
	n->rect.y = y;
	n->resolved_fg = UI_HAS_COLOR(n->fg) ? UI_GRAY_OF(n->fg) : inherited_fg;
	n->resolved_bg = UI_HAS_COLOR(n->bg) ? UI_GRAY_OF(n->bg) : inherited_bg;

	n->content.x = x + n->border + n->pad[3];
	n->content.y = y + n->border + n->pad[0];
	n->content.w = n->rect.w - 2 * n->border - n->pad[1] - n->pad[3];
	n->content.h = n->rect.h - 2 * n->border - n->pad[0] - n->pad[2];
	if (n->content.w < 0) n->content.w = 0;
	if (n->content.h < 0) n->content.h = 0;

	if (!n->first) return;

	bool row = (n->dir == UI_ROW);
	int cont_main  = row ? n->content.w : n->content.h;
	int cont_cross = row ? n->content.h : n->content.w;

	int live = 0, used = 0;
	for (UiNode *ch = n->first; ch; ch = ch->next) {
		if (ch->dropped) continue;
		if (live) used += n->gap;
		used += row ? ch->rect.w : ch->rect.h;
		live++;
	}

	int slack = cont_main - used;
	if (slack < 0) slack = 0;
	int cursor = 0, between = n->gap;
	switch (n->main) {
	case UI_MAIN_CENTER:  cursor = slack / 2; break;
	case UI_MAIN_END:     cursor = slack; break;
	case UI_MAIN_BETWEEN: if (live > 1) between = n->gap + slack / (live - 1); break;
	default: break;
	}

	for (UiNode *ch = n->first; ch; ch = ch->next) {
		if (ch->dropped) {
			ch->rect = (UiRect){ 0, 0, 0, 0 };
			continue;
		}
		int ch_cross = row ? ch->rect.h : ch->rect.w;
		int off_cross = 0;
		switch (n->cross) {
		case UI_CROSS_CENTER: off_cross = (cont_cross - ch_cross) / 2; break;
		case UI_CROSS_END:    off_cross = cont_cross - ch_cross; break;
		default: break;
		}
		if (off_cross < 0) off_cross = 0;

		int cx = row ? n->content.x + cursor : n->content.x + off_cross;
		int cy = row ? n->content.y + off_cross : n->content.y + cursor;
		place(ch, cx, cy, n->resolved_fg, n->resolved_bg);

		cursor += (row ? ch->rect.w : ch->rect.h) + between;
	}
}

void ui_layout(UiNode *root, const UiTextCtx *tc, UiArena *arena, UiRect viewport)
{
	Ctx c = { .tc = tc, .arena = arena };
	// The root always fills the viewport: a frame describes a whole screen.
	measure_node(&c, root, viewport.w, viewport.h, viewport.w, viewport.h);
	place(root, viewport.x, viewport.y, 0, 0xFFu);
}
