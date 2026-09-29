// Host-side preview: renders a real screen to a PNG without a Kobo.
//
// It takes the same UI JSON the device does, and runs the same ui_json.c /
// screens.c / ui_layout.c / ui_text.c, so a layout that looks right here looks
// right there. Only the painter differs.
//
//   php bin/render-ui.php fixtures/typical.json | homesrv-preview --ui -
//   homesrv-preview --url http://localhost:8082 --device glohd
//   homesrv-preview --ui tree.json --dump-rects -   # layout as JSON, no pixels
//   homesrv-preview --screen menu --config fixtures/servers.config

#include "config.h"
#include "http.h"
#include "paint_stb.h"
#include "screens.h"
#include "ui.h"
#include "ui_json.h"
#include "ui_layout.h"
#include "ui_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t g_arena[1024 * 1024];
static uint8_t g_overlay[4096];

// First node carrying an action toast, so the preview can show the tap
// feedback the device draws. Without this the toast is the one screen that
// only ever gets exercised on hardware.
static const UiNode *find_toast_node(const UiNode *n)
{
	if (!n || n->dropped) return NULL;
	if (n->action && n->action->toast && *n->action->toast) return n;
	for (const UiNode *c = n->first; c; c = c->next) {
		const UiNode *f = find_toast_node(c);
		if (f) return f;
	}
	return NULL;
}

struct Device {
	const char *name;
	int w, h, dpi;
};

// Screen geometry per Kobo model. FBInk reports these at runtime on the
// device; here we have to be told which one to imitate.
static const struct Device devices[] = {
	{ "glo",      758,  1024, 213 },
	{ "glohd",    1072, 1448, 300 },
	{ "clarahd",  1072, 1448, 300 },
	{ "libra2",   1264, 1680, 300 },
	{ "auranone", 1404, 1872, 300 },
};

static const struct Device *find_device(const char *name)
{
	for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++)
		if (strcmp(devices[i].name, name) == 0) return &devices[i];
	return NULL;
}

static char *read_file(const char *path, size_t *len)
{
	FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
	if (!f) return NULL;
	size_t cap = 65536, used = 0;
	char *buf = malloc(cap);
	if (!buf) { if (f != stdin) fclose(f); return NULL; }
	for (;;) {
		if (used + 4096 > cap) {
			cap *= 2;
			char *p = realloc(buf, cap);
			if (!p) { free(buf); if (f != stdin) fclose(f); return NULL; }
			buf = p;
		}
		size_t n = fread(buf + used, 1, 4096, f);
		used += n;
		if (n < 4096) break;
	}
	buf[used] = '\0';
	if (len) *len = used;
	if (f != stdin) fclose(f);
	return buf;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: homesrv-preview [options]\n"
		"  --ui FILE         UI JSON to render ('-' for stdin)\n"
		"  --screen NAME     dashboard (default) | menu\n"
		"  --config FILE     server list for --screen menu\n"
		"  --server N        which server the menu shows as current (1-based)\n"
		"  --url URL         fetch /api/ui.php from this server instead\n"
		"  --token TOKEN     API token for --url\n"
		"  --device NAME     glo | glohd | clarahd | libra2 | auranone\n"
		"  --width N --height N --dpi N   override the device preset\n"
		"  --now EPOCH       fix the clock, for reproducible output\n"
		"  --out FILE        PNG to write (default preview.png)\n"
		"  --dump-rects FILE laid-out tree as JSON ('-' for stdout)\n"
		"  --toast           draw the tap feedback over the first action node\n"
		"  --hit X,Y         report which node a touch there would hit, then exit\n"
		"  --font PATH       regular font (default ../fonts/DejaVuSans.ttf)\n"
		"  --font-bold PATH  bold font\n"
		"  --print-device NAME   print 'WIDTH HEIGHT DPI' for a device, then exit\n");
}

int main(int argc, char **argv)
{
	const char *json_path = NULL, *url = NULL, *token = "";
	const char *out = "preview.png", *dump = NULL;
	const char *font = "../fonts/DejaVuSans.ttf";
	const char *font_bold = "../fonts/DejaVuSans-Bold.ttf";
	int show_toast = 0;
	const char *screen = "dashboard", *config_path = NULL, *hit = NULL;
	int server_no = 1;
	const struct Device *dev = &devices[0];
	int w = 0, h = 0, dpi = 0;
	time_t now = time(NULL);

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
		if      (!strcmp(arg, "--ui")     && val) { json_path = val; i++; }
		else if (!strcmp(arg, "--url")    && val) { url = val; i++; }
		else if (!strcmp(arg, "--token")  && val) { token = val; i++; }
		else if (!strcmp(arg, "--out")    && val) { out = val; i++; }
		else if (!strcmp(arg, "--dump-rects") && val) { dump = val; i++; }
		else if (!strcmp(arg, "--font")   && val) { font = val; i++; }
		else if (!strcmp(arg, "--font-bold") && val) { font_bold = val; i++; }
		else if (!strcmp(arg, "--width")  && val) { w = atoi(val); i++; }
		else if (!strcmp(arg, "--height") && val) { h = atoi(val); i++; }
		else if (!strcmp(arg, "--dpi")    && val) { dpi = atoi(val); i++; }
		else if (!strcmp(arg, "--screen") && val) { screen = val; i++; }
		else if (!strcmp(arg, "--config") && val) { config_path = val; i++; }
		else if (!strcmp(arg, "--server") && val) { server_no = atoi(val); i++; }
		else if (!strcmp(arg, "--hit") && val) { hit = val; i++; }
		else if (!strcmp(arg, "--toast")) { show_toast = 1; }
		else if (!strcmp(arg, "--now")    && val) { now = (time_t) atoll(val); i++; }
		else if (!strcmp(arg, "--print-device") && val) {
			// One table of screen geometry, shared with the shell: the
			// Makefile and the test runner ask for it here instead of
			// keeping a second copy that can drift.
			const struct Device *d = find_device(val);
			if (!d) { fprintf(stderr, "unknown device: %s\n", val); return 2; }
			printf("%d %d %d\n", d->w, d->h, d->dpi);
			return 0;
		}
		else if (!strcmp(arg, "--device") && val) {
			dev = find_device(val);
			if (!dev) { fprintf(stderr, "unknown device: %s\n", val); return 2; }
			i++;
		} else { usage(); return 2; }
	}

	if (!w)   w = dev->w;
	if (!h)   h = dev->h;
	if (!dpi) dpi = dev->dpi;

	bool want_menu = strcmp(screen, "menu") == 0;

	char *body = NULL;
	if (want_menu) {
		body = NULL;   // the menu is drawn by the client, from the config
	} else if (url) {
		char path[128];
		snprintf(path, sizeof(path), "/api/ui.php?w=%d&h=%d&dpi=%d", w, h, dpi);
		struct HttpResponse resp;
		if (http_get(url, path, token, &resp) < 0) {
			fprintf(stderr, "preview: fetch from %s failed\n", url);
			return 1;
		}
		if (resp.status != 200) {
			fprintf(stderr, "preview: fetch failed (HTTP %d)\n", resp.status);
			http_free_response(&resp);
			return 1;
		}
		body = strdup(resp.body);
		http_free_response(&resp);
	} else if (json_path) {
		body = read_file(json_path, NULL);
		if (!body) { fprintf(stderr, "preview: cannot read %s\n", json_path); return 1; }
	} else {
		usage();
		return 2;
	}

	UiTextCtx tc;
	ui_text_init(&tc, dpi);
	if (ui_text_load(&tc, UI_FNT_REGULAR, font) < 0) {
		fprintf(stderr, "preview: cannot load font %s\n", font);
		return 1;
	}
	ui_text_load(&tc, UI_FNT_BOLD, font_bold);   // optional

	UiArena arena;
	ui_arena_init(&arena, g_arena, sizeof(g_arena));

	struct Theme theme;
	theme_init(&theme, w);

	struct ScreenCtx ctx = {
		.now = now,
		.last_refresh_success = now,
		.offline = 0,
	};

	UiNode *root;
	if (want_menu) {
		// The same config parser the device uses, so what is previewed is
		// what a real config file produces.
		struct Config cfg;
		if (config_path) {
			if (config_load(config_path, &cfg) < 0) {
				fprintf(stderr, "preview: no usable server in %s\n",
					config_path);
				return 1;
			}
		} else {
			cfg = (struct Config){ .count = 2, .servers = {
				{ "Home", "http://192.168.1.50:8082", "" },
				{ "Test", "http://192.168.1.50:8083", "" },
			} };
		}
		int current = server_no - 1;
		if (current < 0 || current >= cfg.count) current = 0;
		root = screen_menu(&arena, &theme, &cfg, current, "preview");
	} else {
		char err[160];
		UiVars vars = { .now = now };
		UiNode *body_tree = ui_from_json(&arena, body, &vars, err, sizeof(err));
		free(body);
		if (!body_tree) {
			fprintf(stderr, "preview: %s\n", err);
			return 1;
		}
		root = screen_frame(&arena, &theme, body_tree, &ctx);
	}
	ui_layout(root, &tc, &arena, (UiRect){ 0, 0, w, h });

	if (arena.oom)
		fprintf(stderr, "preview: warning: UI arena exhausted (%zu bytes)\n",
			arena.cap);

	// Invisible tap targets cannot be checked by looking at a PNG, so the
	// hit test is askable directly: the same ui_hit_test() the device runs.
	if (hit) {
		int hx = 0, hy = 0;
		if (sscanf(hit, "%d,%d", &hx, &hy) != 2) {
			fprintf(stderr, "preview: --hit wants X,Y\n");
			return 2;
		}
		const UiNode *n = ui_hit_test(root, hx, hy);
		if (!n) {
			printf("%d,%d: nothing\n", hx, hy);
			return 1;
		}
		printf("%d,%d: %s action=%u", hx, hy, n->key ? n->key : "(unkeyed)",
		       n->action->kind);
		if (n->action->path)   printf(" path=%s", n->action->path);
		if (n->action->body)   printf(" body=%s", n->action->body);
		if (n->action->target) printf(" target=%s", n->action->target);
		printf(" rect=[%d,%d,%d,%d]\n", n->rect.x, n->rect.y,
		       n->rect.w, n->rect.h);
		return 0;
	}

	if (dump) {
		char *js = NULL;
		if (ui_write_json(root, &js, NULL) == 0) {
			if (strcmp(dump, "-") == 0) {
				fputs(js, stdout);
			} else {
				FILE *f = fopen(dump, "w");
				if (f) { fputs(js, f); fclose(f); }
				else { fprintf(stderr, "preview: cannot write %s\n", dump); }
			}
			free(js);
		}
	}

	StbCanvas cv;
	if (stb_canvas_init(&cv, w, h, &tc) < 0) return 1;
	stb_canvas_clear(&cv, 255);
	UiPainter p = stb_painter(&cv);
	ui_paint(&p, root);

	if (show_toast) {
		const UiNode *target = find_toast_node(root);
		if (!target) {
			fprintf(stderr, "preview: no node with a toast to draw\n");
		} else {
			// Mirrors render_toast() in main.c: same tree, same engine.
			UiArena ov;
			ui_arena_init(&ov, g_overlay, sizeof(g_overlay));
			UiNode *tn = CHILDREN(BOX(&ov, .dir = UI_ROW, .bg = UI_WHITE,
					      .main = UI_MAIN_CENTER,
					      .cross = UI_CROSS_CENTER),
				TEXT(&ov, target->action->toast,
				     .size_pt = screen_toast_font(target),
				     .align = UI_ALIGN_CENTER, .max_lines = 1));
			ui_layout(tn, &tc, &ov, target->rect);
			ui_paint(&p, tn);
		}
	}

	if (stb_canvas_write_png(&cv, out) < 0) {
		fprintf(stderr, "preview: cannot write %s\n", out);
		return 1;
	}
	fprintf(stderr, "preview: %s  %dx%d @ %d dpi  (arena %zu/%zu bytes)\n",
		out, w, h, dpi, arena.used, arena.cap);

	stb_canvas_free(&cv);
	ui_text_free(&tc);
	return 0;
}
