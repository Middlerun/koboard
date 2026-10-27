// The chrome the client still draws itself.
//
// The dashboard body comes from the server as a UI tree (see ui_json.h); this
// file wraps it in the one row that has to keep working when the server does
// not: the status line, which reports the connection and holds the two
// invisible corner targets.
//
// The clock is *not* here. It lives in the server's layout, as a placeholder
// the client substitutes on every frame (see UiVars), so its wording and
// placement can change without a new build.
//
// The menu is here for the same reason. Switching servers is exactly what you
// want to do when the current one is unreachable, so it cannot be something
// the server draws.
//
// Nothing here touches the framebuffer, so the host preview runs exactly this
// code.

#ifndef HOMESRV_SCREENS_H
#define HOMESRV_SCREENS_H

#include <time.h>

#include "config.h"
#include "ui.h"

// Spacing and point sizes for the chrome, fixed by the client and scaled to the
// panel. A server that wants its body to line up with the status row inset its
// own content by the same width / 30.
struct Theme {
	int margin;
	int font_status, font_msg;
	int font_head, font_item, font_sub, font_btn;
};

void theme_init(struct Theme *t, int screen_w);

struct ScreenCtx {
	time_t now;
	time_t last_refresh_success;
	int    offline;
};

// Keys for the subtrees that get repainted on their own, without a full
// redraw. KEY_CLOCK is the one the server is expected to put on whatever shows
// the time; the rest belong to the chrome, so they exist whatever it sends.
#define KEY_CLOCK   "clock"
#define KEY_STATUS  "status"
#define KEY_MENU    "menu"
#define KEY_REFRESH "refresh"

// Screen names for UI_ACT_OPEN.
#define SCREEN_DASHBOARD "dashboard"
#define SCREEN_MENU      "menu"

// Wrap a server-sent body in the chrome. `body` is adopted, not copied, and is
// given the screen's leftover height: the frame is always body / status, top
// to bottom.
UiNode *screen_frame(UiArena *a, const struct Theme *t, UiNode *body,
		     const struct ScreenCtx *ctx);

// The menu, drawn in place of the dashboard: pick a server, or quit.
UiNode *screen_menu(UiArena *a, const struct Theme *t, const struct Config *cfg,
		    int current, const char *version);

// Point size for the toast drawn over a tapped node: the node's own label
// size, so the feedback matches what it replaced.
int screen_toast_font(const UiNode *n);

// A centred message, used as the body when there is no tree from the server:
// starting up, connecting, a refused response. It goes inside screen_frame()
// like any other body, so the corner targets stay reachable.
UiNode *screen_message(UiArena *a, const struct Theme *t, const char *msg);

#endif
