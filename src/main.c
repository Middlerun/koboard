#include "app.h"
#include "http.h"
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

static volatile sig_atomic_t g_running = 1;
static FBInkConfig g_cfg_draw  = { 0 };
static FBInkConfig g_cfg_flash = { 0 };
static FBInkConfig g_cfg_du    = { 0 };

static int g_touch_switch_xy  = 1;
static int g_touch_mirrored_x = 1;

static void sig_handler(int sig) { (void) sig; g_running = 0; }

int load_config(struct AppState *state)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/config", state->base_dir);
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
		if (line[0] == '#' || line[0] == '\0') continue;
		char *eq = strchr(line, '='); if (!eq) continue;
		*eq = '\0';
		if (strcmp(line, "SERVER_URL") == 0)
			snprintf(state->server_url, sizeof(state->server_url), "%s", eq + 1);
		else if (strcmp(line, "API_TOKEN") == 0)
			snprintf(state->api_token, sizeof(state->api_token), "%s", eq + 1);
	}
	fclose(f);
	if (state->server_url[0] == '\0' || state->api_token[0] == '\0') return -1;
	return 0;
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
	if (s->fbfd < 0) { fprintf(stderr, "homesrv: fbink_open failed\n"); return -1; }

	FBInkConfig init_cfg = { 0 };
	init_cfg.is_quiet = true;
	if (fbink_init(s->fbfd, &init_cfg) < 0) {
		fprintf(stderr, "homesrv: fbink_init failed\n"); return -1;
	}

	FBInkState st = { 0 };
	fbink_get_state(&init_cfg, &st);
	s->screen_w = (int) st.view_width;
	s->screen_h = (int) st.view_height;
	s->dpi      = (int) st.screen_dpi;
	fprintf(stderr, "homesrv: screen %dx%d dpi %d device %s\n",
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
		fprintf(stderr, "homesrv: failed to load %s\n", s->font_path);
		return -1;
	}
	s->font_loaded = 1;

	char bold[512];
	snprintf(bold, sizeof(bold), "%s/fonts/DejaVuSans-Bold.ttf", s->base_dir);
	if (fbink_add_ot_font(bold, FNT_BOLD) < 0 ||
	    ui_text_load(&s->text, UI_FNT_BOLD, bold) < 0)
		fprintf(stderr, "homesrv: no bold font at %s, using regular\n", bold);

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

// Rebuild this frame's tree and lay it out. Cheap enough (tens of
// microseconds, one arena reset) to redo whenever anything might have moved.
static UiNode *build_frame(struct AppState *s, const char *message)
{
	ui_arena_reset(&s->arena);

	struct ScreenCtx ctx = {
		.now = time(NULL),
		.last_refresh_success = s->last_refresh_success,
		.offline = s->offline,
	};

	if (message || !s->data_loaded)
		s->root = screen_message(&s->arena, &s->theme,
					 message ? message : "Loading…");
	else
		s->root = screen_dashboard(&s->arena, &s->theme, &s->data, &ctx);

	ui_layout(s->root, &s->text, &s->arena,
		  (UiRect){ 0, 0, s->screen_w, s->screen_h });

	if (s->arena.oom)
		fprintf(stderr, "homesrv: UI arena exhausted (%zu bytes) — "
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
	UiNode *root = KIDS(BOX(&s->overlay, .dir = UI_ROW, .bg = UI_WHITE,
				.main = UI_MAIN_CENTER, .cross = UI_CROSS_CENTER),
		TEXT(&s->overlay, toast, .size_pt = s->theme.font_btn,
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

static int fetch_dashboard(struct AppState *s)
{
	struct HttpResponse resp;
	if (http_get(s->server_url, "/api/dashboard.php", s->api_token, &resp) < 0) {
		fprintf(stderr, "homesrv: fetch failed (network error), "
			"attempting wifi reconnect\n");
		int rc = system("wpa_cli -i wlan0 reassociate 2>/dev/null &");
		fprintf(stderr, "homesrv: wifi reconnect rc=%d\n", rc);
		return -1;
	}
	if (resp.status != 200) {
		fprintf(stderr, "homesrv: fetch failed (HTTP %d)\n", resp.status);
		http_free_response(&resp); return -1;
	}
	if (json_parse_dashboard(resp.body, &s->data) < 0) {
		fprintf(stderr, "homesrv: fetch failed (JSON parse error)\n");
		http_free_response(&resp); return -1;
	}
	http_free_response(&resp);
	s->data_loaded = 1;
	return 0;
}

static int api_post(struct AppState *s, const char *path, const char *body)
{
	struct HttpResponse resp;
	if (http_post(s->server_url, path, s->api_token, body, &resp) < 0) {
		fprintf(stderr, "homesrv: POST %s (%s) failed (network error)\n",
			path, body);
		return -1;
	}
	if (resp.status != 200) {
		fprintf(stderr, "homesrv: POST %s (%s) failed (HTTP %d)\n",
			path, body, resp.status);
		http_free_response(&resp); return -1;
	}
	fprintf(stderr, "homesrv: POST %s (%s) ok\n", path, body);
	http_free_response(&resp);
	return 0;
}

// Every tap goes through here. Behaviour comes from the node's declared
// action, so nothing in the touch path re-derives where anything is drawn.
static void perform_action(struct AppState *s, const UiNode *n)
{
	const UiAction *act = n->action;
	switch (act->kind) {
	case UI_ACT_POST:
		if (act->toast && *act->toast) {
			render_toast(s, n, act->toast);
			sleep(2);
		}
		if (api_post(s, act->path, act->body) == 0)
			s->refresh_pending = 1;
		break;
	case UI_ACT_REFRESH:
		s->refresh_pending = 1;
		break;
	case UI_ACT_QUIT:
		fprintf(stderr, "homesrv: quit triggered\n");
		g_running = 0;
		break;
	default:
		break;
	}
}

// ---- main ---------------------------------------------------------------

int main(int argc, char *argv[])
{
	if (argc > 1 && strcmp(argv[1], "--version") == 0) {
		printf("homesrv-kobo %s (FBInk %s)\n", HOMESRV_VERSION, fbink_version());
		return 0;
	}

	static struct AppState s;
	memset(&s, 0, sizeof(s));
	s.last_refresh_success = time(NULL);

	const char *base_dir = (argc > 1) ? argv[1] : "/mnt/onboard/.adds/homesrv";
	snprintf(s.base_dir, sizeof(s.base_dir), "%s", base_dir);

	if (load_config(&s) < 0) {
		fprintf(stderr, "homesrv: failed to load config from %s/config\n",
			s.base_dir);
		return 1;
	}

	struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = sig_handler;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);

	if (init_display(&s) < 0) {
		fprintf(stderr, "homesrv: display init failed\n"); return 1;
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
	}

	render_message(&s, "Starting HomeSrv...");

	// Ensure Wi-Fi is connected before the first fetch.
	fprintf(stderr, "homesrv: checking wifi...\n");
	for (int attempt = 0; attempt < 5; attempt++) {
		struct HttpResponse pr;
		if (http_get(s.server_url, "/api/ping.php", s.api_token, &pr) == 0
		    && pr.status == 200) {
			http_free_response(&pr);
			fprintf(stderr, "homesrv: wifi ok (attempt %d)\n", attempt + 1);
			break;
		}
		fprintf(stderr, "homesrv: wifi not ready (attempt %d), "
			"reconnecting...\n", attempt + 1);
		char buf[128];
		snprintf(buf, sizeof(buf), "Connecting to Wi-Fi... (attempt %d)",
			 attempt + 1);
		render_message(&s, buf);
		system("wpa_cli -i wlan0 reassociate 2>/dev/null &");
		sleep(5);
	}

	if (fetch_dashboard(&s) != 0) {
		char buf[128];
		snprintf(buf, sizeof(buf), "Connecting to %s...", s.server_url);
		render_message(&s, buf);
		fbink_wait_for_complete(s.fbfd, LAST_MARKER);
	}

	if (s.data_loaded) {
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

		if (!s.data_loaded && now - s.last_refresh >= 15) {
			conn_retries++;
			char buf[128];
			snprintf(buf, sizeof(buf), "Connecting to %s... (retry %d)",
				 s.server_url, conn_retries);
			render_message(&s, buf);
			fbink_wait_for_complete(s.fbfd, LAST_MARKER);
			if (fetch_dashboard(&s) == 0) {
				render_full_screen(&s);
				fbink_wait_for_complete(s.fbfd, LAST_MARKER);
			}
			s.last_refresh = now;
			continue;
		}

		if (s.data_loaded && (now - s.last_refresh >= 1800 || s.refresh_pending)) {
			if (s.refresh_pending)
				fprintf(stderr, "homesrv: refresh (user action)\n");
			else
				fprintf(stderr, "homesrv: refreshing... (interval=%lds)\n",
					(long) (now - s.last_refresh));
			if (fetch_dashboard(&s) == 0) {
				fprintf(stderr, "homesrv: refresh ok\n");
				s.offline = 0;
				s.last_refresh_success = now;
				s.last_refresh = now;
				s.refresh_pending = 0;
			} else {
				s.offline = 1;
			}
			render_full_screen(&s);
			fbink_wait_for_complete(s.fbfd, LAST_MARKER);
			continue;
		}

		// Full-screen flash every 2 hours to clear ghosting.
		if (s.data_loaded && now - s.last_screen_refresh >= 7200) {
			fprintf(stderr, "homesrv: screen refresh (clear ghosting)\n");
			FBInkConfig cc = g_cfg_flash;
			cc.wfm_mode = WFM_GC16;
			cc.is_flashing = true;
			fbink_refresh(s.fbfd, 0, 0, 0, 0, &cc);
			fbink_wait_for_complete(s.fbfd, LAST_MARKER);
			s.last_screen_refresh = now;
		}

		// Clock: repaint just that subtree on minute boundaries.
		if (s.data_loaded) {
			struct tm tm_buf;
			struct tm *tm = localtime_r(&now, &tm_buf);
			if (last_minute != -1 && last_minute != tm->tm_min)
				render_subtree(&s, KEY_CLOCK);
			last_minute = tm->tm_min;
		}

		// Connectivity: repaint just the status line when it changes.
		if (s.data_loaded && now - last_conn_check >= 10) {
			struct HttpResponse pr;
			int was_offline = s.offline;
			if (http_get(s.server_url, "/api/ping.php", s.api_token, &pr) == 0
			    && pr.status == 200) {
				s.offline = 0;
				http_free_response(&pr);
			} else {
				s.offline = 1;
			}
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
		fprintf(stderr, "homesrv: touch raw=%d,%d -> %d,%d\n",
			touch_x, touch_y, sx, sy);

		const UiNode *hit = ui_hit_test(s.root, sx, sy);
		if (!hit) {
			fprintf(stderr, "homesrv: touch hit nothing\n");
			continue;
		}
		fprintf(stderr, "homesrv: hit %s\n", hit->key ? hit->key : "(unkeyed)");
		perform_action(&s, hit);
	}

	fprintf(stderr, "homesrv: clearing screen on exit\n");
	fbink_set_bg_pen_gray(0xFFu, false, false);
	fbink_cls(s.fbfd, &g_cfg_draw, NULL, false);
	fbink_refresh(s.fbfd, 0, 0, 0, 0, &g_cfg_flash);
	fbink_wait_for_complete(s.fbfd, LAST_MARKER);

	if (evdev) libevdev_free(evdev);
	if (s.touch_fd >= 0) close(s.touch_fd);
	if (s.font_loaded) fbink_free_ot_fonts();
	ui_text_free(&s.text);
	fbink_close(s.fbfd);
	return 0;
}
