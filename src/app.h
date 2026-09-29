#ifndef HOMESRV_APP_H
#define HOMESRV_APP_H

#include <time.h>

#include "config.h"
#include "paint_fbink.h"
#include "screens.h"
#include "ui.h"
#include "ui_text.h"

// Per-frame UI tree memory. The dashboard uses ~30 KB on a busy day; the rest
// is headroom, sized so that a tree at ui_json.c's node ceiling (160 bytes a
// node, plus its strings) still fits. Exhaustion is reported, never fatal.
#define UI_ARENA_BYTES (512 * 1024)

struct AppState {
	char   base_dir[256];
	char   font_path[512];

	// Every server the config offers, and the one being talked to now. The
	// menu switches between them without a restart.
	struct Config config;
	int    server_index;

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

	// The last tree the server sent, as received. It is re-parsed into the
	// frame arena on every frame, so there is exactly one copy of the UI and
	// one code path that turns it into nodes.
	char  *ui_json;
	int    ui_loaded;

	// The menu takes the whole screen while it is up, so the frame is built
	// from it instead of from the server's tree.
	int    menu_open;

	time_t last_refresh;
	time_t last_refresh_success;
	time_t last_screen_refresh;
	int    offline;
	int    refresh_pending;
};

int  init_display(struct AppState *s);
int  init_touch(const char *dev_path);

#endif
