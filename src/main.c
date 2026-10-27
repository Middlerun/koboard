#include "app.h"
#include "config.h"
#include "http.h"
#include "touch.h"
#include "ui_json.h"
#include "ui_layout.h"
#include "ui_paint.h"

#include "fbink.h"
#include "libevdev/libevdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>

static volatile sig_atomic_t g_running = 1;
static FBInkConfig g_cfg_draw  = { 0 };
static FBInkConfig g_cfg_flash = { 0 };
static FBInkConfig g_cfg_du    = { 0 };

static int g_touch_switch_xy  = 1;
static int g_touch_mirrored_x = 1;

static void sig_handler(int sig) { (void) sig; g_running = 0; }

// The server currently selected in the menu.
static const struct Server *cur(const struct AppState *s)
{
	return &s->config.servers[s->server_index];
}

int init_display(struct AppState *s)
{
	g_cfg_draw.is_quiet   = true;
	g_cfg_draw.bg_color   = BG_WHITE;
	g_cfg_draw.no_refresh = true;

	g_cfg_flash.is_quiet    = true;
	g_cfg_flash.bg_color    = BG_WHITE;
	g_cfg_flash.wfm_mode    = WFM_GL16;
	g_cfg_flash.is_flashing  = false;

	g_cfg_du.is_quiet = true;
	g_cfg_du.bg_color = BG_WHITE;
	g_cfg_du.wfm_mode = WFM_DU;

	s->fbfd = fbink_open();
	if (s->fbfd < 0) { fprintf(stderr, "koboard: fbink_open failed\n"); return -1; }

	FBInkConfig init_cfg = { 0 };
	init_cfg.is_quiet = true;
	if (fbink_init(s->fbfd, &init_cfg) < 0) {
		fprintf(stderr, "koboard: fbink_init failed\n"); return -1;
	}

	FBInkState st = { 0 };
	fbink_get_state(&init_cfg, &st);
	s->screen_w = (int) st.view_width;
	s->screen_h = (int) st.view_height;
	s->dpi      = (int) st.screen_dpi;
	fprintf(stderr, "koboard: screen %dx%d dpi %d device %s\n",
		s->screen_w, s->screen_h, s->dpi, st.device_name);

	theme_init(&s->theme, s->screen_w);
	ui_arena_init(&s->arena, s->arena_buf, sizeof(s->arena_buf));

	int device_id = (int) st.device_id;
	if (device_id == 10 || device_id == 70 || device_id == 72 || device_id == 74) {
		g_touch_switch_xy  = 1;
		g_touch_mirrored_x = 0;
	} else {
		g_touch_switch_xy  = 1;
		g_touch_mirrored_x = 1;
	}

	// Two font registries have to agree: FBInk's, which does the drawing, and
	// ours, which does the measuring. They are fed the same files.
	ui_text_init(&s->text, s->dpi);

	snprintf(s->font_path, sizeof(s->font_path), "%s/fonts/DejaVuSans.ttf",
		 s->base_dir);
	if (fbink_add_ot_font(s->font_path, FNT_REGULAR) < 0 ||
	    ui_text_load(&s->text, UI_FNT_REGULAR, s->font_path) < 0) {
		fprintf(stderr, "koboard: failed to load %s\n", s->font_path);
		return -1;
	}
	s->font_loaded = 1;

	char bold[512];
	snprintf(bold, sizeof(bold), "%s/fonts/DejaVuSans-Bold.ttf", s->base_dir);
	if (fbink_add_ot_font(bold, FNT_BOLD) < 0 ||
	    ui_text_load(&s->text, UI_FNT_BOLD, bold) < 0)
		fprintf(stderr, "koboard: no bold font at %s, using regular\n", bold);

	s->painter = (FbPainter){
		.fbfd = s->fbfd,
		.screen_w = s->screen_w,
		.screen_h = s->screen_h,
		.tc = &s->text,
		.cfg = &g_cfg_draw,
	};
	return 0;
}

// ---- rendering -----------------------------------------------------------

// Rebuild this frame's tree and lay it out. The body is re-parsed from the
// server's JSON every time, which costs a few milliseconds and keeps one code
// path: there is no half-built tree to keep in sync with the arena.
static UiNode *build_frame(struct AppState *s, const char *message)
{
	ui_arena_reset(&s->arena);

	struct ScreenCtx ctx = {
		.now = time(NULL),
		.last_refresh_success = s->last_refresh_success,
		.offline = s->offline,
	};

	if (s->menu_open) {
		s->root = screen_menu(&s->arena, &s->theme, &s->config,
				      s->server_index, KOBOARD_VERSION);
		ui_layout(s->root, &s->text, &s->arena,
			  (UiRect){ 0, 0, s->screen_w, s->screen_h });
		return s->root;
	}

	UiNode *body = NULL;
	char err[160] = { 0 };
	if (!message && s->ui_json) {
		UiVars vars = { .now = ctx.now };
		body = ui_from_json(&s->arena, s->ui_json, &vars, NULL, err, sizeof(err));
		if (!body) fprintf(stderr, "koboard: bad UI from server: %s\n", err);
	}

	// A message goes in the frame too, not instead of it: the corner targets
	// are the only way to reach the menu, and "cannot talk to this server" is
	// exactly when you want to pick a different one.
	if (!body) {
		const char *msg = message ? message : (err[0] ? err : "Loading…");
		body = screen_message(&s->arena, &s->theme, msg);
	}
	s->root = screen_frame(&s->arena, &s->theme, body, &ctx);

	ui_layout(s->root, &s->text, &s->arena,
		  (UiRect){ 0, 0, s->screen_w, s->screen_h });

	if (s->arena.oom)
		fprintf(stderr, "koboard: UI arena exhausted (%zu bytes) — "
			"frame may be incomplete\n", s->arena.cap);
	return s->root;
}

static void paint_and_flash(struct AppState *s)
{
	fbink_set_bg_pen_gray(0xFFu, false, false);
	fbink_cls(s->fbfd, &g_cfg_draw, NULL, false);

	UiPainter p = fbink_painter(&s->painter);
	ui_paint(&p, s->root);

	fbink_refresh(s->fbfd, 0, 0, 0, 0, &g_cfg_flash);
}

static void render_full_screen(struct AppState *s)
{
	build_frame(s, NULL);
	paint_and_flash(s);
}

static void render_message(struct AppState *s, const char *msg)
{
	build_frame(s, msg);
	paint_and_flash(s);
}

// Repaint one keyed subtree and refresh only its rect. Used for the clock and
// the connectivity line, which change far more often than the rest.
static void render_subtree(struct AppState *s, const char *key)
{
	build_frame(s, NULL);
	UiNode *n = ui_find(s->root, key);
	if (!n || n->rect.w <= 0 || n->rect.h <= 0) return;

	fbink_set_bg_pen_gray(0xFFu, false, false);
	FBInkRect r = {
		(unsigned short) n->rect.x, (unsigned short) n->rect.y,
		(unsigned short) n->rect.w, (unsigned short) n->rect.h,
	};
	fbink_cls(s->fbfd, &g_cfg_draw, &r, false);

	UiPainter p = fbink_painter(&s->painter);
	ui_paint(&p, n);

	FBInkConfig du = g_cfg_du;
	du.no_refresh = false;
	fbink_refresh(s->fbfd, (unsigned short) n->rect.y,
		      (unsigned short) n->rect.x, (unsigned short) n->rect.w,
		      (unsigned short) n->rect.h, &du);
}

// Optimistic feedback: replace a tapped node with its action's toast text
// while the request is in flight.
//
// This goes through the layout engine rather than printing straight to the
// framebuffer, so the text is measured, fitted to one line and ellipsized the
// same way every other string is. Printing it raw let FBInk wrap it inside the
// button and spill past the refreshed area.
static void render_toast(struct AppState *s, const UiNode *n, const char *toast)
{
	UiRect rect = n->rect;   // copy: the node lives in the frame arena

	ui_arena_init(&s->overlay, s->overlay_buf, sizeof(s->overlay_buf));
	UiNode *root = CHILDREN(BOX(&s->overlay, .dir = UI_ROW, .bg = UI_WHITE,
				.main = UI_MAIN_CENTER, .cross = UI_CROSS_CENTER),
		TEXT(&s->overlay, toast, .size_pt = screen_toast_font(n),
		     .align = UI_ALIGN_CENTER, .max_lines = 1));
	ui_layout(root, &s->text, &s->overlay, rect);

	FBInkConfig du = g_cfg_du;
	du.no_refresh = true;

	FbPainter fp = s->painter;
	fp.cfg = &du;
	UiPainter p = fbink_painter(&fp);
	ui_paint(&p, root);

	du.no_refresh = false;
	fbink_refresh(s->fbfd, (unsigned short) rect.y, (unsigned short) rect.x,
		      (unsigned short) rect.w, (unsigned short) rect.h, &du);
}

// ---- touch ---------------------------------------------------------------

static void transform_touch(int raw_x, int raw_y, int *out_x, int *out_y,
			    int sw, int sh)
{
	int can_x, can_y;
	if (g_touch_switch_xy && g_touch_mirrored_x) {
		can_x = sw - raw_y; can_y = raw_x;
	} else if (g_touch_switch_xy) {
		can_x = raw_y; can_y = raw_x;
	} else {
		can_x = raw_x; can_y = raw_y;
	}
	*out_x = can_x; *out_y = can_y;
	if (*out_x < 0) *out_x = 0;
	if (*out_y < 0) *out_y = 0;
	if (*out_x >= sw) *out_x = sw - 1;
	if (*out_y >= sh) *out_y = sh - 1;
}

int init_touch(const char *dev_path)
{
	return open(dev_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
}

// ---- network -------------------------------------------------------------

// Adopt what the current server's envelope asks for (see policy.h). `env` may
// be NULL: the defaults, before a server has said anything.
static void apply_policy(struct AppState *s, const UiEnvelope *env)
{
	struct Policy p;
	policy_from_envelope(&p, env, cur(s)->url);

	if (p.refresh_seconds != s->policy.refresh_seconds
	    || p.ping_seconds != s->policy.ping_seconds
	    || p.flash_seconds != s->policy.flash_seconds
	    || strcmp(p.ping_url, s->policy.ping_url) != 0)
		fprintf(stderr, "koboard: policy: refresh %ds, ping %s every %ds, "
			"flash %ds\n", p.refresh_seconds,
			p.ping_url[0] ? p.ping_url : "(none)", p.ping_seconds,
			p.flash_seconds);
	s->policy = p;
}

// Ask the server for its screen. The panel's geometry goes with the request,
// as query parameters on whatever URL the config gave: the server scales its
// layout to the width, and sends back a tree this client only has to lay out
// and paint.
static int fetch_ui(struct AppState *s)
{
	char query[64], url[512];
	snprintf(query, sizeof(query), "w=%d&h=%d&dpi=%d",
		 s->screen_w, s->screen_h, s->dpi);
	if (url_with_query(cur(s)->url, query, url, sizeof(url)) < 0) {
		snprintf(s->last_error, sizeof(s->last_error), "URL too long");
		return -1;
	}

	struct HttpResponse resp;
	if (http_get(url, cur(s)->token, &resp) < 0) {
		fprintf(stderr, "koboard: fetch %s failed (network error), "
			"attempting wifi reconnect\n", url);
		snprintf(s->last_error, sizeof(s->last_error), "no connection");
		int rc = system("wpa_cli -i wlan0 reassociate 2>/dev/null &");
		fprintf(stderr, "koboard: wifi reconnect rc=%d\n", rc);
		return -1;
	}
	if (resp.status != 200) {
		fprintf(stderr, "koboard: fetch %s failed (HTTP %d)\n", url,
			resp.status);
		snprintf(s->last_error, sizeof(s->last_error), "HTTP %d", resp.status);
		http_free_response(&resp); return -1;
	}

	// Parse once here, into an arena of its own, so a broken response is
	// rejected before it replaces the screen that is currently working — and
	// without disturbing the frame tree that hit testing is still holding.
	void *probe_buf = malloc(UI_ARENA_BYTES);
	if (!probe_buf) { http_free_response(&resp); return -1; }
	UiArena probe;
	ui_arena_init(&probe, probe_buf, UI_ARENA_BYTES);
	char err[160];
	UiEnvelope env;
	UiNode *probe_root = ui_from_json(&probe, resp.body, NULL, &env,
					  err, sizeof(err));
	free(probe_buf);
	if (!probe_root) {
		fprintf(stderr, "koboard: fetch %s failed (%s)\n", url, err);
		snprintf(s->last_error, sizeof(s->last_error), "%s", err);
		http_free_response(&resp); return -1;
	}

	char *copy = strdup(resp.body);
	http_free_response(&resp);
	if (!copy) { fprintf(stderr, "koboard: out of memory\n"); return -1; }

	free(s->ui_json);
	s->ui_json = copy;
	s->ui_loaded = 1;
	s->last_error[0] = '\0';
	apply_policy(s, &env);
	return 0;
}

// An action's path is resolved against the screen URL, so a server can say
// "/api/tasks/complete.php" without the tree knowing where it is hosted.
static int api_post(struct AppState *s, const char *path, const char *body)
{
	char url[512];
	if (url_resolve(cur(s)->url, path, url, sizeof(url)) < 0) {
		fprintf(stderr, "koboard: POST %s: URL too long\n", path);
		return -1;
	}

	struct HttpResponse resp;
	if (http_post(url, cur(s)->token, body, &resp) < 0) {
		fprintf(stderr, "koboard: POST %s (%s) failed (network error)\n",
			url, body);
		return -1;
	}
	if (resp.status != 200) {
		fprintf(stderr, "koboard: POST %s (%s) failed (HTTP %d)\n",
			url, body, resp.status);
		http_free_response(&resp); return -1;
	}
	fprintf(stderr, "koboard: POST %s (%s) ok\n", url, body);
	http_free_response(&resp);
	return 0;
}

// Point the client at another server: drop the tree the old one sent, so its
// dashboard cannot linger under the new server's name, and ask for a fresh
// one. The menu closes, because the answer to "did it work" is the dashboard.
static void select_server(struct AppState *s, int index)
{
	if (index < 0 || index >= s->config.count) return;

	fprintf(stderr, "koboard: switching to server %d: %s (%s)\n", index + 1,
		s->config.servers[index].name, s->config.servers[index].url);

	s->server_index = index;
	free(s->ui_json);
	s->ui_json = NULL;
	s->ui_loaded = 0;
	s->last_error[0] = '\0';
	apply_policy(s, NULL);
	s->offline = 0;
	s->menu_open = 0;
	s->refresh_pending = 1;

	char msg[160];
	snprintf(msg, sizeof(msg), "Connecting to %s...",
		 s->config.servers[index].name);
	render_message(s, msg);
}

// Every tap goes through here. Behaviour comes from the node's declared
// action, so nothing in the touch path re-derives where anything is drawn.
static void perform_action(struct AppState *s, const UiNode *n)
{
	const UiAction *act = n->action;

	// Any action may carry optimistic feedback, not just a POST: an invisible
	// target especially needs to show that the tap landed.
	bool toasted = act->toast && *act->toast;
	if (toasted) render_toast(s, n, act->toast);

	switch (act->kind) {
	case UI_ACT_POST:
		if (toasted) sleep(2);
		if (api_post(s, act->path, act->body) == 0)
			s->refresh_pending = 1;
		break;
	case UI_ACT_REFRESH:
		fprintf(stderr, "koboard: refresh requested\n");
		s->refresh_pending = 1;
		break;
	case UI_ACT_QUIT:
		fprintf(stderr, "koboard: quit triggered\n");
		g_running = 0;
		break;
	case UI_ACT_OPEN:
		// Only the two screens the client itself draws; the dashboard is
		// whatever the server last sent.
		s->menu_open = act->target && strcmp(act->target, SCREEN_MENU) == 0;
		fprintf(stderr, "koboard: open %s\n",
			s->menu_open ? SCREEN_MENU : SCREEN_DASHBOARD);
		render_full_screen(s);
		break;
	case UI_ACT_SERVER:
		select_server(s, act->target ? atoi(act->target) : 0);
		break;
	default:
		break;
	}
}

// ---- main ---------------------------------------------------------------

int main(int argc, char *argv[])
{
	if (argc > 1 && strcmp(argv[1], "--version") == 0) {
		printf("koboard-kobo %s (FBInk %s)\n", KOBOARD_VERSION, fbink_version());
		return 0;
	}

	static struct AppState s;
	memset(&s, 0, sizeof(s));
	s.last_refresh_success = time(NULL);

	const char *base_dir = (argc > 1) ? argv[1] : "/mnt/onboard/.adds/koboard";
	snprintf(s.base_dir, sizeof(s.base_dir), "%s", base_dir);

	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/config", s.base_dir);
	if (config_load(config_path, &s.config) < 0) {
		fprintf(stderr, "koboard: no usable server in %s\n", config_path);
		return 1;
	}
	for (int i = 0; i < s.config.count; i++)
		fprintf(stderr, "koboard: server %d: %s (%s)\n", i + 1,
			s.config.servers[i].name, s.config.servers[i].url);

	struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = sig_handler;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);

	if (init_display(&s) < 0) {
		fprintf(stderr, "koboard: display init failed\n"); return 1;
	}

	s.touch_fd = init_touch("/dev/input/event1");
	if (s.touch_fd < 0)
		s.touch_fd = init_touch("/dev/input/by-path/platform-0-0010-event");

	struct libevdev *evdev = NULL;
	int has_touch = 0;
	if (s.touch_fd >= 0) {
		evdev = libevdev_new();
		libevdev_set_fd(evdev, s.touch_fd);
		has_touch = 1;

		// Nickel is only paused, and still holds the touchscreen open:
		// see touch.h for why it has to be ours alone while we run.
		touch_grab(evdev);
	}

	apply_policy(&s, NULL);
	render_message(&s, "Starting Koboard...");

	// The first fetch doubles as the Wi-Fi check: there is no separate ping
	// to try before the server has said where its ping is. fetch_ui() asks
	// for a reassociate itself on a network error, so this only has to wait.
	for (int attempt = 0; attempt < 5; attempt++) {
		if (fetch_ui(&s) == 0) {
			fprintf(stderr, "koboard: first fetch ok (attempt %d)\n",
				attempt + 1);
			break;
		}
		char buf[160];
		snprintf(buf, sizeof(buf), "Connecting to %s... (%s)",
			 cur(&s)->name, s.last_error);
		render_message(&s, buf);
		fbink_wait_for_complete(s.fbfd, LAST_MARKER);
		sleep(5);
	}

	if (s.ui_loaded) {
		s.offline = 0;
		s.last_refresh_success = time(NULL);
		render_full_screen(&s);
		fbink_wait_for_complete(s.fbfd, LAST_MARKER);
	}
	s.last_refresh = time(NULL);
	s.last_screen_refresh = time(NULL);

	int touch_x = 0, touch_y = 0, touch_down = 0, conn_retries = 0;
	int last_minute = -1;
	time_t last_conn_check = 0;
	struct pollfd pfd = { .fd = s.touch_fd, .events = POLLIN };

	while (g_running) {
		int timeout = has_touch ? 1000 : 15000;
		int poll_rc = poll(has_touch ? &pfd : NULL, has_touch ? 1 : 0, timeout);
		time_t now = time(NULL);

		if (!s.ui_loaded && !s.refresh_pending && !s.menu_open
		    && now - s.last_refresh >= 15) {
			conn_retries++;
			char buf[160];
			snprintf(buf, sizeof(buf), "Connecting to %s... (retry %d: %s)",
				 cur(&s)->name, conn_retries,
				 s.last_error[0] ? s.last_error : "waiting");
			render_message(&s, buf);
			fbink_wait_for_complete(s.fbfd, LAST_MARKER);
			if (fetch_ui(&s) == 0) {
				render_full_screen(&s);
				fbink_wait_for_complete(s.fbfd, LAST_MARKER);
			}
			s.last_refresh = now;
			continue;
		}

		if (!s.menu_open
		    && (s.refresh_pending
			|| (s.ui_loaded
			    && now - s.last_refresh >= s.policy.refresh_seconds))) {
			if (s.refresh_pending)
				fprintf(stderr, "koboard: refresh (user action)\n");
			else
				fprintf(stderr, "koboard: refreshing... (interval=%lds)\n",
					(long) (now - s.last_refresh));
			if (fetch_ui(&s) == 0) {
				fprintf(stderr, "koboard: refresh ok\n");
				s.offline = 0;
				s.last_refresh_success = now;
				s.last_refresh = now;
				s.refresh_pending = 0;
			} else {
				s.offline = 1;
				s.refresh_pending = 0;
				s.last_refresh = now;
			}
			if (s.ui_loaded) {
				render_full_screen(&s);
			} else {
				// Nothing to fall back on — usually a server just
				// picked from the menu that is not answering. Say
				// which one, and leave the corners reachable.
				char msg[192];
				snprintf(msg, sizeof(msg), "Cannot reach %s (%s)",
					 cur(&s)->name, s.last_error);
				render_message(&s, msg);
			}
			fbink_wait_for_complete(s.fbfd, LAST_MARKER);
			continue;
		}

		// Full-screen flash to clear ghosting, as often as the server asks.
		if (s.ui_loaded && !s.menu_open && s.policy.flash_seconds > 0
		    && now - s.last_screen_refresh >= s.policy.flash_seconds) {
			fprintf(stderr, "koboard: screen refresh (clear ghosting)\n");
			FBInkConfig cc = g_cfg_flash;
			cc.wfm_mode = WFM_GC16;
			cc.is_flashing = true;
			fbink_refresh(s.fbfd, 0, 0, 0, 0, &cc);
			fbink_wait_for_complete(s.fbfd, LAST_MARKER);
			s.last_screen_refresh = now;
		}

		// Clock: repaint just that subtree on minute boundaries.
		if (s.ui_loaded && !s.menu_open) {
			struct tm tm_buf;
			struct tm *tm = localtime_r(&now, &tm_buf);
			if (last_minute != -1 && last_minute != tm->tm_min)
				render_subtree(&s, KEY_CLOCK);
			last_minute = tm->tm_min;
		}

		// Connectivity: repaint just the status line when it changes. Only
		// if the server named a ping URL; without one, online/offline just
		// follows how the last fetch went.
		if (s.ui_loaded && !s.menu_open && s.policy.ping_seconds > 0
		    && now - last_conn_check >= s.policy.ping_seconds) {
			struct HttpResponse pr;
			int was_offline = s.offline;
			int ok = http_get(s.policy.ping_url, cur(&s)->token, &pr) == 0
				 && pr.status == 200;
			// A body comes back with any status, not just 200, and
			// http_get() leaves it NULL on failure, so free either way.
			http_free_response(&pr);
			s.offline = !ok;
			last_conn_check = now;
			if (was_offline != s.offline)
				render_subtree(&s, KEY_STATUS);
		}

		if (poll_rc < 0) { if (errno == EINTR) continue; break; }
		if (poll_rc == 0) continue;
		if (!has_touch || !(pfd.revents & POLLIN)) continue;

		int got_touch = 0, rc;
		do {
			struct input_event ev;
			rc = libevdev_next_event(evdev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
			if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
				if (ev.type == EV_ABS) {
					if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X)
						touch_x = ev.value;
					else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y)
						touch_y = ev.value;
				} else if (ev.type == EV_KEY &&
					   (ev.code == BTN_TOOL_FINGER ||
					    ev.code == BTN_TOUCH ||
					    ev.code == BTN_TOOL_PEN)) {
					touch_down = (ev.value > 0) ? 1 : 0;
				} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
					got_touch = 1;
				}
			} else if (rc == LIBEVDEV_READ_STATUS_SYNC) {
				while (rc == LIBEVDEV_READ_STATUS_SYNC)
					rc = libevdev_next_event(evdev,
						LIBEVDEV_READ_FLAG_SYNC, &ev);
			}
		} while (rc == LIBEVDEV_READ_STATUS_SYNC ||
			 rc == LIBEVDEV_READ_STATUS_SUCCESS);

		if (!got_touch || !touch_down) continue;

		int sx, sy;
		transform_touch(touch_x, touch_y, &sx, &sy, s.screen_w, s.screen_h);
		fprintf(stderr, "koboard: touch raw=%d,%d -> %d,%d\n",
			touch_x, touch_y, sx, sy);

		const UiNode *hit = ui_hit_test(s.root, sx, sy);
		if (!hit) {
			fprintf(stderr, "koboard: touch hit nothing\n");
			continue;
		}
		fprintf(stderr, "koboard: hit %s\n", hit->key ? hit->key : "(unkeyed)");
		perform_action(&s, hit);
	}

	fprintf(stderr, "koboard: clearing screen on exit\n");
	fbink_set_bg_pen_gray(0xFFu, false, false);
	fbink_cls(s.fbfd, &g_cfg_draw, NULL, false);
	fbink_refresh(s.fbfd, 0, 0, 0, 0, &g_cfg_flash);
	fbink_wait_for_complete(s.fbfd, LAST_MARKER);

	// After the clear, so the screen answers the Quit tap at once; the lift
	// has usually happened by now and is just waiting to be read.
	if (evdev) {
		touch_wait_for_lift(evdev, s.touch_fd, touch_down, 3000);
		touch_release(evdev);
	}

	free(s.ui_json);
	if (evdev) libevdev_free(evdev);
	if (s.touch_fd >= 0) close(s.touch_fd);
	if (s.font_loaded) fbink_free_ot_fonts();
	ui_text_free(&s.text);
	fbink_close(s.fbfd);
	return 0;
}
