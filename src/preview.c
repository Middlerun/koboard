// Host-side preview: renders the real dashboard tree to a PNG without a Kobo.
//
// This runs the same screens.c / ui_layout.c / ui_text.c as the device, so a
// layout that looks right here looks right there. Only the painter differs.
//
//   homesrv-preview --json ../fixtures/typical.json --out /tmp/dash.png
//   homesrv-preview --url http://localhost:8082 --device glohd
//   homesrv-preview --json f.json --dump-rects -   # layout as JSON, no pixels

#include "http.h"
#include "json.h"
#include "paint_stb.h"
#include "screens.h"
#include "ui.h"
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
		"  --json FILE       dashboard JSON to render ('-' for stdin)\n"
		"  --url URL         fetch /api/dashboard.php from this server instead\n"
		"  --token TOKEN     API token for --url\n"
		"  --device NAME     glo | glohd | clarahd | libra2 | auranone\n"
		"  --width N --height N --dpi N   override the device preset\n"
		"  --now EPOCH       fix the clock, for reproducible output\n"
		"  --out FILE        PNG to write (default preview.png)\n"
		"  --dump-rects FILE laid-out tree as JSON ('-' for stdout)\n"
		"  --toast           draw the tap feedback over the first action node\n"
		"  --font PATH       regular font (default ../fonts/DejaVuSans.ttf)\n"
		"  --font-bold PATH  bold font\n");
}

int main(int argc, char **argv)
{
	const char *json_path = NULL, *url = NULL, *token = "";
	const char *out = "preview.png", *dump = NULL;
	const char *font = "../fonts/DejaVuSans.ttf";
	const char *font_bold = "../fonts/DejaVuSans-Bold.ttf";
	int show_toast = 0;
	const struct Device *dev = &devices[0];
	int w = 0, h = 0, dpi = 0;
	time_t now = time(NULL);

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
		if      (!strcmp(arg, "--json")   && val) { json_path = val; i++; }
		else if (!strcmp(arg, "--url")    && val) { url = val; i++; }
		else if (!strcmp(arg, "--token")  && val) { token = val; i++; }
		else if (!strcmp(arg, "--out")    && val) { out = val; i++; }
		else if (!strcmp(arg, "--dump-rects") && val) { dump = val; i++; }
		else if (!strcmp(arg, "--font")   && val) { font = val; i++; }
		else if (!strcmp(arg, "--font-bold") && val) { font_bold = val; i++; }
		else if (!strcmp(arg, "--width")  && val) { w = atoi(val); i++; }
		else if (!strcmp(arg, "--height") && val) { h = atoi(val); i++; }
		else if (!strcmp(arg, "--dpi")    && val) { dpi = atoi(val); i++; }
		else if (!strcmp(arg, "--toast")) { show_toast = 1; }
		else if (!strcmp(arg, "--now")    && val) { now = (time_t) atoll(val); i++; }
		else if (!strcmp(arg, "--device") && val) {
			dev = find_device(val);
			if (!dev) { fprintf(stderr, "unknown device: %s\n", val); return 2; }
			i++;
		} else { usage(); return 2; }
	}

	if (!w)   w = dev->w;
	if (!h)   h = dev->h;
	if (!dpi) dpi = dev->dpi;

	char *body = NULL;
	if (url) {
		struct HttpResponse resp;
		if (http_get(url, "/api/dashboard.php", token, &resp) < 0) {
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

	struct DashboardData data;
	if (json_parse_dashboard(body, &data) < 0) {
		fprintf(stderr, "preview: JSON parse error\n");
		free(body);
		return 1;
	}
	free(body);

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

	UiNode *root = screen_dashboard(&arena, &theme, &data, &ctx);
	ui_layout(root, &tc, &arena, (UiRect){ 0, 0, w, h });

	if (arena.oom)
		fprintf(stderr, "preview: warning: UI arena exhausted (%zu bytes)\n",
			arena.cap);

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
			UiNode *tn = KIDS(BOX(&ov, .dir = UI_ROW, .bg = UI_WHITE,
					      .main = UI_MAIN_CENTER,
					      .cross = UI_CROSS_CENTER),
				TEXT(&ov, target->action->toast,
				     .size_pt = theme.font_btn,
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
