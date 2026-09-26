#ifndef HOMESRV_APP_H
#define HOMESRV_APP_H

#include <time.h>

#include "json.h"
#include "paint_fbink.h"
#include "screens.h"
#include "ui.h"
#include "ui_text.h"

// Per-frame UI tree memory. The dashboard uses ~15 KB; the rest is headroom
// for a busier day. Exhaustion is reported, never fatal.
#define UI_ARENA_BYTES (256 * 1024)

struct AppState {
	char   base_dir[256];
	char   server_url[256];
	char   api_token[64];
	char   font_path[512];

	int    fbfd;
	int    font_loaded;
	int    touch_fd;

	int    screen_w;
	int    screen_h;
	int    dpi;

	struct Theme theme;
	UiTextCtx    text;
	UiArena      arena;
	UiNode      *root;        // current frame's tree, for hit testing
	FbPainter    painter;
	uint8_t      arena_buf[UI_ARENA_BYTES];

	// Separate arena for transient overlays (the tap toast), so drawing one
	// does not disturb the frame tree that hit testing is still using.
	UiArena      overlay;
	uint8_t      overlay_buf[4096];

	struct DashboardData data;
	int    data_loaded;

	time_t last_refresh;
	time_t last_refresh_success;
	time_t last_screen_refresh;
	int    offline;
	int    refresh_pending;
};

int  load_config(struct AppState *state);
int  init_display(struct AppState *s);
int  init_touch(const char *dev_path);

#endif
