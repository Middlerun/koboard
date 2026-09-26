// Dashboard screens, as UI trees.
//
// This is the only place that decides what the dashboard looks like. It builds
// a tree and returns it; it never touches the framebuffer, so the host preview
// runs exactly this code.

#ifndef HOMESRV_SCREENS_H
#define HOMESRV_SCREENS_H

#include <time.h>

#include "json.h"
#include "ui.h"

// Point sizes and spacing, derived from the screen width.
struct Theme {
	int margin, gap, gap_tight;
	int font_date, font_time, font_temp, font_detail, font_fcast;
	int font_header, font_title, font_meta, font_btn;
	int font_evt, font_evt_sub, font_status, font_msg;
};

void theme_init(struct Theme *t, int screen_w);

struct ScreenCtx {
	time_t now;
	time_t last_refresh_success;
	int    offline;
};

// Keys for the subtrees that get repainted on their own, without a full redraw.
#define KEY_CLOCK  "clock"
#define KEY_STATUS "status"
#define KEY_QUIT   "quit"

UiNode *screen_dashboard(UiArena *a, const struct Theme *t,
			 const struct DashboardData *d,
			 const struct ScreenCtx *ctx);

UiNode *screen_message(UiArena *a, const struct Theme *t, const char *msg);

const char *weather_icon(int code);

#endif
