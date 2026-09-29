#include "screens.h"

#include <stdio.h>
#include <string.h>

void theme_init(struct Theme *t, int screen_w)
{
	memset(t, 0, sizeof(*t));
	t->margin = screen_w / 30;
	if (t->margin < 10) t->margin = 10;

	t->font_status = 7;
	t->font_msg    = 14;

	t->font_head   = 14;
	t->font_item   = 13;
	t->font_sub    = 9;
	t->font_btn    = 11;
}

// The status line, and with it the screen's two invisible corners: refresh at
// bottom left, menu at bottom right. They are real nodes with real rects, so
// hit testing finds them the way it finds everything else, and they draw
// nothing — the left one is simply the status text with an action on it.
static UiNode *status_row(UiArena *a, const struct Theme *t,
			  const struct ScreenCtx *ctx)
{
	struct tm tm_buf;
	struct tm *tm = localtime_r(&ctx->last_refresh_success, &tm_buf);
	char when[16];
	strftime(when, sizeof(when), "%H:%M", tm);

	return CHILDREN(BOX(a, .key = KEY_STATUS, .dir = UI_ROW,
			.main = UI_MAIN_BETWEEN, .cross = UI_CROSS_END,
			.pad = { 0, t->margin, 12, t->margin }),
		CHILDREN(BOX(a, .key = KEY_REFRESH, .dir = UI_ROW, .h = PX(44),
			 .cross = UI_CROSS_END,
			 // The target draws nothing, so the toast is the only sign
			 // the tap landed at all.
			 .action = ACTION(a, .kind = UI_ACT_REFRESH,
					  .toast = "Refreshing…")),
			TEXTF(a, (.size_pt = t->font_status, .fg = UI_MED,
				  .max_lines = 1),
			      "Last refresh: %s  · %s", when,
			      ctx->offline ? "Offline" : "Online")),
		BOX(a, .key = KEY_MENU, .w = PX(70), .h = PX(44),
		    .action = ACTION(a, .kind = UI_ACT_OPEN,
				     .target = SCREEN_MENU)));
}

UiNode *screen_frame(UiArena *a, const struct Theme *t, UiNode *body,
		     const struct ScreenCtx *ctx)
{
	UiNode *root = BOX(a, .dir = UI_COL, .bg = UI_WHITE);
	if (body) {
		// The body owns everything above the status line, whether or not the
		// server thought to say so.
		body->h = FLEX(1);
		ui_add(root, body);
	}
	ui_add(root, status_row(a, t, ctx));
	return root;
}

// ---- menu ----------------------------------------------------------------

// Outlined, never filled: on the panel, light glyphs on a dark fill come out
// as a blank grey slab, because the waveform blends the two greys long before
// the thin strokes survive.
static UiNode *menu_button(UiArena *a, const struct Theme *t, const char *label,
			   const UiAction *action)
{
	return CHILDREN(BOX(a, .dir = UI_ROW, .w = FLEX(1),
			.main = UI_MAIN_CENTER, .cross = UI_CROSS_CENTER,
			.pad = PAD_XY(14, 12), .bg = UI_WHITE,
			.border = 2, .border_color = UI_BLACK,
			.action = action),
		TEXT(a, label, .size_pt = t->font_btn, .style = UI_FNT_BOLD,
		     .align = UI_ALIGN_CENTER, .max_lines = 1));
}

static UiNode *server_row(UiArena *a, const struct Theme *t,
			  const struct Server *server, int index, bool current)
{
	// The marker is a column of its own so the names line up whether or not
	// a row is the current one.
	return CHILDREN(BOX(a, .dir = UI_ROW, .gap = t->margin / 2,
			.cross = UI_CROSS_CENTER, .pad = PAD_XY(10, 0),
			.action = ACTION(a, .kind = UI_ACT_SERVER,
					 .target = ui_strf(a, "%d", index))),
		TEXT(a, current ? "▶" : " ", .size_pt = t->font_item,
		     .w = PX(30), .max_lines = 1),
		CHILDREN(BOX(a, .dir = UI_COL, .w = FLEX(1), .gap = 2),
			TEXT(a, server->name, .size_pt = t->font_item,
			     .style = current ? UI_FNT_BOLD : UI_FNT_REGULAR,
			     .max_lines = 1),
			TEXT(a, server->url, .size_pt = t->font_sub, .fg = UI_MED,
			     .max_lines = 1)));
}

UiNode *screen_menu(UiArena *a, const struct Theme *t, const struct Config *cfg,
		    int current, const char *version)
{
	UiNode *root = BOX(a, .dir = UI_COL, .bg = UI_WHITE,
			   .pad = { 40, (int16_t) t->margin, 12,
				    (int16_t) t->margin });

	ui_add(root, TEXT(a, "MENU", .size_pt = t->font_head, .max_lines = 1));
	ui_add(root, BOX(a, .h = PX(20)));
	ui_add(root, TEXT(a, "SERVER", .size_pt = t->font_sub, .fg = UI_MED,
			  .max_lines = 1));
	ui_add(root, BOX(a, .h = PX(10)));

	UiNode *list = BOX(a, .dir = UI_COL, .gap = t->margin / 2);
	for (int i = 0; i < cfg->count; i++)
		ui_add(list, server_row(a, t, &cfg->servers[i], i, i == current));
	ui_add(root, list);

	// The list is followed by all the leftover space, so the buttons sit at
	// the bottom of the screen however many servers there are.
	ui_add(root, BOX(a, .h = FLEX(1)));

	ui_add(root, CHILDREN(BOX(a, .dir = UI_ROW, .gap = t->margin),
		menu_button(a, t, "Close", ACTION(a, .kind = UI_ACT_OPEN,
						  .target = SCREEN_DASHBOARD)),
		menu_button(a, t, "Quit", ACTION(a, .kind = UI_ACT_QUIT))));

	ui_add(root, BOX(a, .h = PX(12)));
	ui_add(root, TEXTF(a, (.size_pt = t->font_status, .fg = UI_MED,
			       .align = UI_ALIGN_CENTER, .max_lines = 1),
			   "homesrv %s", version ? version : "?"));
	return root;
}

int screen_toast_font(const UiNode *n)
{
	// The toast stands in for the node that was tapped, so it borrows that
	// node's own label size rather than a size of its own. A button with no
	// text at all still gets something legible.
	if (!n) return 9;
	if (n->kind == UI_TEXT && n->size_pt) return n->size_pt;
	for (const UiNode *c = n->first; c; c = c->next) {
		int size = screen_toast_font(c);
		if (size) return size;
	}
	return 9;
}

UiNode *screen_message(UiArena *a, const struct Theme *t, const char *msg)
{
	return CHILDREN(BOX(a, .dir = UI_COL, .bg = UI_WHITE,
			.main = UI_MAIN_CENTER, .cross = UI_CROSS_CENTER,
			.pad = PAD_XY(0, (int16_t) t->margin)),
		TEXT(a, msg, .size_pt = t->font_msg, .align = UI_ALIGN_CENTER));
}
