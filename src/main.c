#include "http.h"
#include "json.h"
#include "layout.h"

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
#include <sys/stat.h>

static volatile sig_atomic_t g_running = 1;
static FBInkConfig g_cfg_draw  = { 0 };
static FBInkConfig g_cfg_flash = { 0 };
static FBInkConfig g_cfg_du    = { 0 };

static int g_touch_switch_xy  = 1;
static int g_touch_mirrored_x = 1;

static void sig_handler(int sig) { (void) sig; g_running = 0; }

#define LUM_BLACK    0
#define LUM_DARK    64
#define LUM_MED     128
#define LUM_LIGHT   192
#define LUM_WHITE   255

void layout_compute(struct Layout *l, int sw, int sh)
{
	(void) sh;
	memset(l, 0, sizeof(*l));
	l->margin  = sw / 30;              if (l->margin  < 10) l->margin = 10;
	l->gap     = l->margin;
	l->col_half = (sw - 2 * l->margin - l->gap) / 2;
	l->col_left = l->margin;
	l->col_right = l->margin + l->col_half + l->gap;

	l->font_date   = 14;
	l->font_temp   = 30;
	l->font_detail = 11;
	l->font_fcast  = 10;
	l->font_header = 14;
	l->font_title  = 13;
	l->font_meta   = 9;
	l->font_btn    = 9;
	l->font_evt    = 11;
	l->font_evt_sub = 8;
	l->font_err    = 14;
}

const char *weather_icon(int code)
{
	// WMO weather interpretation codes
	// https://open-meteo.com/en/docs
	if (code == 0)   return "\u2600";       // ☀ clear sky
	if (code == 1)   return "\U0001F324";   // 🌤 mainly clear
	if (code == 2)   return "\u26C5";       // ⛅ partly cloudy
	if (code == 3)   return "\u2601";       // ☁ overcast
	if (code <= 48)  return "\U0001F32B";   // 🌫 fog / rime fog
	if (code <= 57)  return "\U0001F327";   // 🌧 drizzle / freezing drizzle
	if (code <= 67)  return "\U0001F327";   // 🌧 rain / freezing rain
	if (code <= 77)  return "\u2744";       // ❄ snow fall / snow grains
	if (code <= 82)  return "\U0001F326";   // 🌦 rain showers
	if (code <= 86)  return "\U0001F328";   // 🌨 snow showers
	if (code <= 99)  return "\u26C8";       // ⛈ thunderstorm / thunderstorm with hail
	return "?";                             // Unknown
}

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
	g_cfg_draw.is_quiet  = true;
	g_cfg_draw.bg_color  = BG_WHITE;
	g_cfg_draw.no_refresh = true;

	g_cfg_flash.is_quiet   = true;
	g_cfg_flash.bg_color   = BG_WHITE;
	g_cfg_flash.wfm_mode   = WFM_GL16;
	g_cfg_flash.is_flashing = false;

	g_cfg_du.is_quiet   = true;
	g_cfg_du.bg_color   = BG_WHITE;
	g_cfg_du.wfm_mode   = WFM_DU;

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
	fprintf(stderr, "homesrv: screen %dx%d dpi %d device %s\n",
		s->screen_w, s->screen_h, st.screen_dpi, st.device_name);
	layout_compute(&s->layout, s->screen_w, s->screen_h);

	int device_id = (int) st.device_id;
	if (device_id == 10 || device_id == 70 || device_id == 72 || device_id == 74) {
		g_touch_switch_xy  = 1;
		g_touch_mirrored_x = 0;
	} else {
		g_touch_switch_xy  = 1;
		g_touch_mirrored_x = 1;
	}

	snprintf(s->font_path, sizeof(s->font_path), "%s/fonts/DejaVuSans.ttf", s->base_dir);
	if (fbink_add_ot_font(s->font_path, FNT_REGULAR) < 0) {
		fprintf(stderr, "homesrv: fbink_add_ot_font failed\n"); return -1;
	}
	s->font_loaded = 1;
	return 0;
}

void ot_setup(FBInkOTConfig *ot, int top, int left, int right,
	      unsigned int size_pt, int centered)
{
	memset(ot, 0, sizeof(*ot));
	ot->margins.top    = (short) top;
	ot->margins.left   = (short) left;
	ot->margins.right  = (short) right;
	ot->size_pt        = size_pt;
	ot->is_centered    = (bool) centered;
	ot->is_formatted   = false;
}

int ot_print(int fbfd, const char *text, FBInkOTConfig *ot)
{
	// FBInk reads is_centered from fbink_cfg when non-NULL,
	// not from the OT config. Mirror it so centering works.
	bool was = g_cfg_draw.is_centered;
	g_cfg_draw.is_centered = ot->is_centered;
	int rv = fbink_print_ot(fbfd, text, ot, &g_cfg_draw, NULL);
	g_cfg_draw.is_centered = was;
	return rv;
}

int ot_print_top(int fbfd, const char *text, FBInkOTConfig *ot, int *top)
{
	bool was = g_cfg_draw.is_centered;
	g_cfg_draw.is_centered = ot->is_centered;
	int new_top = fbink_print_ot(fbfd, text, ot, &g_cfg_draw, NULL);
	g_cfg_draw.is_centered = was;
	// fbink_print_ot returns 0 when there is no room left to print
	// (and a negative value on error); never treat that as a valid cursor.
	if (new_top <= 0) return -1;
	if (top) *top = new_top;
	return 0;
}

static void pen(uint8_t gray)
{
	fbink_set_bg_pen_gray(gray, false, false);
}

static void fpen(uint8_t gray)
{
	fbink_set_fg_pen_gray(gray, false, false);
}

// Truncate src to at most max_bytes, avoiding splitting a UTF-8 sequence,
// and append an ellipsis if anything was cut off.
static void truncate_ellipsis(char *dst, size_t dstsize, const char *src,
			      size_t max_bytes)
{
	size_t len = strlen(src);
	if (len <= max_bytes) {
		snprintf(dst, dstsize, "%s", src);
		return;
	}
	size_t cut = max_bytes;
	while (cut > 0 && ((unsigned char) src[cut] & 0xC0) == 0x80)
		cut--;
	snprintf(dst, dstsize, "%.*s\u2026", (int) cut, src);
}

void draw_divider(int fbfd, int y, int x1, int x2, uint8_t gray)
{
	pen(gray);
	FBInkRect r = { (unsigned short) x1, (unsigned short) y,
			(unsigned short) (x2 - x1), 4 };
	fprintf(stderr, "homesrv: div top=%d left=%d w=%d h=%d gray=%d\n",
		r.top, r.left, r.width, r.height, gray);
	fbink_cls(fbfd, &g_cfg_draw, &r, false);
	pen(LUM_WHITE);
}

void draw_filled_box(int fbfd, int x, int y, int w, int h, uint8_t gray)
{
	pen(gray);
	FBInkRect r = { (unsigned short) x, (unsigned short) y,
			(unsigned short) w, (unsigned short) h };
	fprintf(stderr, "homesrv: box top=%d left=%d w=%d h=%d gray=%d\n",
		r.top, r.left, r.width, r.height, gray);
	fbink_cls(fbfd, &g_cfg_draw, &r, false);
	pen(LUM_WHITE);
}

// ---- rendering ----

int render_clock_row(const struct AppState *s, int top)
{
	const struct Layout *l = &s->layout;
	FBInkOTConfig ot;
	time_t now = time(NULL);
	struct tm tm_buf;
	struct tm *tm = localtime_r(&now, &tm_buf);
	char buf[64];
	int mid = s->screen_w / 2;
	int y_t, y_d;

	// Date — left half
	strftime(buf, sizeof(buf), "%a, %b %e", tm);
	ot_setup(&ot, top, l->margin, s->screen_w - mid + l->gap,
		 (unsigned) l->font_date, false);
	ot_print_top(s->fbfd, buf, &ot, &y_d);

	// Time — right edge, 12-hour
	strftime(buf, sizeof(buf), "%I:%M", tm);
	ot_setup(&ot, top, s->screen_w - 180, l->margin,
		 (unsigned) (l->font_date - 2), false);
	ot_print_top(s->fbfd, buf, &ot, &y_t);

	int bottom = (y_t > y_d) ? y_t : y_d;
	fprintf(stderr, "homesrv: clock_row top=%d bottom=%d\n", top, bottom);
	return bottom + 2;
}

int render_weather(const struct AppState *s, int top)
{
	const struct Layout *l = &s->layout;
	FBInkOTConfig ot;
	char buf[128];
	if (!s->data_loaded || !s->data.has_weather) return top;
	const struct Weather *w = &s->data.weather;

	int temp_end, detail_end;

	// Icon + temp — centred, full width
	ot_setup(&ot, top, l->margin, l->margin,
		 (unsigned) l->font_temp, true);
	snprintf(buf, sizeof(buf), "%s  %d\u00B0",
		 weather_icon(w->code), w->temperature);
	ot_print_top(s->fbfd, buf, &ot, &temp_end);

	// Single detail line: desc · hi/lo · rain% · humidity%
	snprintf(buf, sizeof(buf), "%s", w->description);
	{
		int blen = (int) strlen(buf);
		snprintf(buf + blen, sizeof(buf) - (size_t) blen,
			 " \u00B7 %d\u00B0/%d\u00B0",
			 w->today_high, w->today_low);
	}
	{
		int blen = (int) strlen(buf);
		snprintf(buf + blen, sizeof(buf) - (size_t) blen,
			 " \u00B7 \u2602%d%%", w->rain_chance);
	}
	{
		int blen = (int) strlen(buf);
		snprintf(buf + blen, sizeof(buf) - (size_t) blen,
			 " \u00B7 \U0001F4A7%d%%", w->humidity);
	}
	ot_setup(&ot, temp_end + 2, l->margin, l->margin,
		 (unsigned) l->font_detail, true);
	ot_print_top(s->fbfd, buf, &ot, &detail_end);

	fprintf(stderr, "homesrv: weather top=%d bottom=%d\n", top, detail_end);
	return detail_end + 8;
}

int render_forecast(const struct AppState *s, int top)
{
	const struct Layout *l = &s->layout;
	FBInkOTConfig ot;
	char buf[64];
	if (!s->data_loaded || !s->data.has_weather) return top;
	const struct Weather *w = &s->data.weather;

	int n = w->forecast_count;
	if (n < 1) return top;

	int col_w = (s->screen_w - 2 * l->margin - l->gap * (n - 1)) / n;
	if (col_w < 60) col_w = 60;

	int max_end = top;
	for (int i = 0; i < n; i++) {
		const struct Forecast *f = &w->forecast[i];
		int cx = l->margin + i * (col_w + l->gap);
		int col_r = s->screen_w - cx - col_w;
		int y = top;

		ot_setup(&ot, y, cx, col_r, (unsigned) l->font_fcast, true);
		ot_print_top(s->fbfd, f->name, &ot, &y);

		ot_setup(&ot, y + 2, cx, col_r,
			 (unsigned) (l->font_fcast + 4), true);
		ot_print_top(s->fbfd, weather_icon(f->code), &ot, &y);

		snprintf(buf, sizeof(buf), "%d\u00B0/%d\u00B0", f->high, f->low);
		ot_setup(&ot, y + 2, cx, col_r, (unsigned) l->font_fcast, true);
		ot_print_top(s->fbfd, buf, &ot, &y);

		if (y > max_end) max_end = y;
	}

	fprintf(stderr, "homesrv: forecast top=%d bottom=%d\n", top, max_end);
	return max_end + 8;
}

// Days from "today" (date_iso, YYYY-MM-DD) to next_due. Positive = future,
// negative = past, 0 = today.
static int days_from_today(const char *date_iso, const char *next_due)
{
	struct tm t0 = { 0 }, t1 = { 0 };
	int y, m, d;
	if (sscanf(date_iso, "%d-%d-%d", &y, &m, &d) != 3) return 0;
	t0.tm_year = y - 1900; t0.tm_mon = m - 1; t0.tm_mday = d; t0.tm_hour = 12;
	if (sscanf(next_due, "%d-%d-%d", &y, &m, &d) != 3) return 0;
	t1.tm_year = y - 1900; t1.tm_mon = m - 1; t1.tm_mday = d; t1.tm_hour = 12;
	time_t a = mktime(&t0), b = mktime(&t1);
	if (a == (time_t) -1 || b == (time_t) -1) return 0;
	return (int) (difftime(b, a) / 86400.0);
}

// Full weekday name ("Tuesday") for a YYYY-MM-DD date, or -1 on failure.
static int weekday_name(const char *date_iso, char *buf, size_t bufsz)
{
	struct tm tm_buf = { 0 };
	int y, m, d;
	if (sscanf(date_iso, "%d-%d-%d", &y, &m, &d) != 3) return -1;
	tm_buf.tm_year = y - 1900; tm_buf.tm_mon = m - 1; tm_buf.tm_mday = d;
	tm_buf.tm_hour = 12;
	if (mktime(&tm_buf) == (time_t) -1) return -1;
	strftime(buf, bufsz, "%A", &tm_buf);
	return 0;
}

// Build the human-friendly due-date label ("Due today", "Due on Tuesday",
// "Was due on 05/09", ...).
static void format_due_label(const struct AppState *s, const struct Task *t,
			     char *buf, size_t bufsz)
{
	int diff = days_from_today(s->data.date_iso, t->next_due);
	int yr, mo, dy;
	int has_date = sscanf(t->next_due, "%d-%d-%d", &yr, &mo, &dy) == 3;

	if (diff == 0) {
		snprintf(buf, bufsz, "Due today");
		return;
	}
	if (diff == -1) {
		snprintf(buf, bufsz, "Was due yesterday");
		return;
	}
	if (diff > 0 && diff < 4) {
		char wd[16];
		if (weekday_name(t->next_due, wd, sizeof(wd)) == 0)
			snprintf(buf, bufsz, "Due on %s", wd);
		else
			snprintf(buf, bufsz, "Due on %02d/%02d", dy, mo);
		return;
	}
	if (diff < 0 && diff > -4) {
		char wd[16];
		if (weekday_name(t->next_due, wd, sizeof(wd)) == 0)
			snprintf(buf, bufsz, "Was due on %s", wd);
		else
			snprintf(buf, bufsz, "Was due on %02d/%02d", dy, mo);
		return;
	}
	if (has_date)
		snprintf(buf, bufsz, diff > 0 ? "Due on %02d/%02d"
					     : "Was due on %02d/%02d", dy, mo);
	else
		snprintf(buf, bufsz, diff > 0 ? "Due on %s" : "Was due on %s",
			 t->next_due);
}

int render_tasks(struct AppState *s, int top, int col_x, int col_w)
{
	const struct Layout *l = &s->layout;
	FBInkOTConfig ot;
	char buf[256];
	int y = top;
	int col_rm = s->screen_w - (col_x + col_w);

	// Section header
	ot_setup(&ot, y, col_x, col_rm, (unsigned) l->font_header, false);
	snprintf(buf, sizeof(buf), "TASKS");
	if (ot_print_top(s->fbfd, buf, &ot, &y) < 0) return y;
	y += 6;

	s->task_count = 0;
	int max_y = s->screen_h - 55;

	for (int i = 0; i < s->data.task_count && y < max_y; i++) {
		const struct Task *t = &s->data.tasks[i];
		int text_x = col_x;

		// Title
		ot_setup(&ot, y, text_x, col_rm, (unsigned) l->font_title, false);
		if (ot_print_top(s->fbfd, t->title, &ot, &y) < 0) return y;

		// Meta
		format_due_label(s, t, buf, sizeof(buf));
		if (t->due_time[0]) {
			int bl = (int) strlen(buf);
			snprintf(buf + bl, sizeof(buf) - (size_t) bl,
				 " \u00B7 %s", t->due_time);
		}
		if (t->assigned_name[0]) {
			int bl = (int) strlen(buf);
			snprintf(buf + bl, sizeof(buf) - (size_t) bl,
				 " \u00B7 %s", t->assigned_name);
		}
		if (t->recur_type[0]) {
			int bl = (int) strlen(buf);
			snprintf(buf + bl, sizeof(buf) - (size_t) bl,
				 " \u00B7 %s", t->recur_type);
		}
		ot_setup(&ot, y + 2, text_x, col_rm, (unsigned) l->font_meta, false);
		if (ot_print_top(s->fbfd, buf, &ot, &y) < 0) return y;

		// Done button
		y += 4;
		s->task_button_y[s->task_count] = y;

		// Button background: dark filled box
		int btn_w = col_w * 2 / 3;
		if (btn_w > 200) btn_w = 200;
		int btn_x = text_x;
		int btn_h = l->font_btn * 4 + 4;
		pen(LUM_DARK);
		FBInkRect br = { (unsigned short) btn_x, (unsigned short) y,
				 (unsigned short) btn_w, (unsigned short) btn_h };
		fbink_cls(s->fbfd, &g_cfg_draw, &br, false);

		// White text on dark background (match text bg to button)
		fpen(LUM_WHITE);
		ot_setup(&ot, y + 2, btn_x,
			 s->screen_w - (btn_x + btn_w),
			 (unsigned) l->font_btn, true);
		ot_print(s->fbfd, "Done", &ot);
		fpen(LUM_BLACK);
		pen(LUM_WHITE);

		y += l->gap + 20;

		// Record card bottom for hit-testing
		s->task_card_bottom[s->task_count++] = y;
	}

	fprintf(stderr, "homesrv: tasks top=%d count=%d bottom=%d\n",
		top, s->task_count, y);
	return y;
}

// Render a single calendar event. Returns 0 on success, -1 if there is no
// room left. Events already over are drawn dimmed and truncated to one line;
// other events are truncated to three lines. All truncation adds an ellipsis.
static int render_event(const struct AppState *s, const struct CalendarEvent *e,
			int col_x, int col_w, int *y)
{
	const struct Layout *l = &s->layout;
	FBInkOTConfig ot;
	char buf[256];
	int col_rm = s->screen_w - (col_x + col_w);
	int row_y = *y;
	int time_x = col_x;
	// Enough for "12:00pm" at the normal event font size (11pt).
	int time_w = col_w * 3 / 8;
	const char *time_text = e->all_day ? "All day" : e->time_str;
	int font_main = e->is_over ? l->font_evt_sub : l->font_evt;
	int max_lines = e->is_over ? 1 : 3;

	int yt = row_y, ys = row_y;

	ot_setup(&ot, row_y, time_x, s->screen_w - time_x - time_w,
		 (unsigned) font_main, false);
	if (ot_print_top(s->fbfd, time_text, &ot, &yt) < 0) goto fail;

	int summary_x = time_x + time_w + l->gap;
	int summary_w = s->screen_w - summary_x - col_rm;
	if (summary_w < 60) summary_w = 60;

	// Estimate characters per line (average ~2.3px per pt per char),
	// then truncate the summary to the allowed number of lines.
	int chars_per_line = summary_w / (font_main * 2.3);
	if (chars_per_line < 8) chars_per_line = 8;
	char summary_buf[128];
	truncate_ellipsis(summary_buf, sizeof(summary_buf), e->summary,
			  (size_t) (max_lines * chars_per_line));

	ot_setup(&ot, row_y, summary_x, col_rm, (unsigned) font_main, false);
	if (ot_print_top(s->fbfd, summary_buf, &ot, &ys) < 0) goto fail;

	if (e->is_over) {
		*y = (yt > ys) ? yt : ys;
		*y += l->gap / 2;
		return 0;
	}

	int row_bottom = (yt > ys) ? yt : ys;
	snprintf(buf, sizeof(buf), "\u2014 %s", e->feed_name);
	ot_setup(&ot, ys + 2, summary_x + l->margin, col_rm,
		 (unsigned) l->font_evt_sub, false);
	if (ot_print_top(s->fbfd, buf, &ot, y) < 0) return -1;

	if (row_bottom > *y) *y = row_bottom;
	*y += l->gap;
	return 0;

fail:
	if (e->is_over) fpen(LUM_BLACK);
	return -1;
}

int render_calendar(const struct AppState *s, int top, int col_x, int col_w)
{
	const struct Layout *l = &s->layout;
	FBInkOTConfig ot;
	char buf[256];
	int y = top;
	int col_rm = s->screen_w - (col_x + col_w);
	int has_today = 0, has_tomorrow = 0;
	int max_y = s->screen_h - 55;

	for (int i = 0; i < s->data.event_count; i++) {
		if (s->data.events[i].is_today) has_today = 1;
		else has_tomorrow = 1;
	}

	ot_setup(&ot, y, col_x, col_rm, (unsigned) l->font_header, false);
	if (ot_print_top(s->fbfd, "TODAY", &ot, &y) < 0) return y;
	y += 6;

	if (!has_today) {
		ot_setup(&ot, y, col_x, col_rm, (unsigned) l->font_meta, false);
		if (ot_print_top(s->fbfd, "Nothing scheduled", &ot, &y) < 0)
			return y;
		y += l->margin;
	}

	int shown = 0;
	for (int i = 0; i < s->data.event_count && y < max_y; i++) {
		const struct CalendarEvent *e = &s->data.events[i];
		if (!e->is_today) continue;
		if (render_event(s, e, col_x, col_w, &y) < 0) return y;
		shown++;
	}
	if (shown < has_today) {
		snprintf(buf, sizeof(buf), "\u2026 +%d more",
			 has_today - shown);
		ot_setup(&ot, y, col_x, col_rm, (unsigned) l->font_meta, false);
		if (ot_print_top(s->fbfd, buf, &ot, &y) < 0) return y;
		y += l->gap;
	}

	if (y < max_y) {
		y += l->gap;
		ot_setup(&ot, y, col_x, col_rm, (unsigned) l->font_header, false);
		if (ot_print_top(s->fbfd, "TOMORROW", &ot, &y) < 0) return y;
		y += 6;

		if (!has_tomorrow) {
			ot_setup(&ot, y, col_x, col_rm, (unsigned) l->font_meta, false);
			if (ot_print_top(s->fbfd, "Nothing scheduled", &ot, &y) < 0)
				return y;
		}

		int shown_t = 0;
		for (int i = 0; i < s->data.event_count && y < max_y; i++) {
			const struct CalendarEvent *e = &s->data.events[i];
			if (e->is_today) continue;
			if (render_event(s, e, col_x, col_w, &y) < 0) return y;
			shown_t++;
		}
		if (shown_t < has_tomorrow) {
			snprintf(buf, sizeof(buf), "\u2026 +%d more",
				 has_tomorrow - shown_t);
			ot_setup(&ot, y, col_x, col_rm, (unsigned) l->font_meta, false);
			if (ot_print_top(s->fbfd, buf, &ot, &y) < 0) return y;
		}
	}

	fprintf(stderr, "homesrv: calendar top=%d bottom=%d\n", top, y);
	return y;
}

void render_message(const struct AppState *s, const char *msg)
{
	const struct Layout *l = &s->layout;

	pen(LUM_WHITE);
	fbink_cls(s->fbfd, &g_cfg_draw, NULL, false);
	pen(LUM_WHITE);

	FBInkOTConfig ot;
	ot_setup(&ot, s->screen_h / 2 - 20, l->margin, l->margin,
		 (unsigned) l->font_err, true);
	FBInkConfig mc = g_cfg_draw;
	mc.no_refresh = false;
	fbink_print_ot(s->fbfd, msg, &ot, &mc, NULL);
}

void render_full_screen(struct AppState *s)
{
	const struct Layout *l = &s->layout;
	if (!s->data_loaded) return;

	pen(LUM_WHITE);
	fbink_cls(s->fbfd, &g_cfg_draw, NULL, false);
	pen(LUM_WHITE);

	int y = 40;
	y = render_clock_row(s, y);
	y += 20;

	y = render_weather(s, y);
	y += 40;

	y = render_forecast(s, y);
	y += 30;

	// Horizontal divider: full width, between forecast and columns
	int div_y = y;
	draw_divider(s->fbfd, div_y, 0, s->screen_w, LUM_DARK);
	y = div_y + 30;

	s->column_y = y;

	render_tasks(s, y, l->col_left, l->col_half);
	render_calendar(s, y, l->col_right, l->col_half);

	// Vertical divider: screen center, full height to bottom (minus quit area)
	int div_x = s->screen_w / 2;
	draw_filled_box(s->fbfd, div_x - 2, div_y, 4, s->screen_h - div_y - 40, LUM_DARK);

	fprintf(stderr, "homesrv: layout div_y=%d div_x=%d screen=%dx%d\n",
		div_y, div_x, s->screen_w, s->screen_h);

	// Bottom status: last refresh time + offline indicator
	char status[128];
	struct tm tm_buf;
	struct tm *ltm = localtime_r(&s->last_refresh_success, &tm_buf);
	char time_str[16];
	strftime(time_str, sizeof(time_str), "%H:%M", ltm);
	if (s->offline)
		snprintf(status, sizeof(status),
			 "Last refresh: %s  \u00B7 Offline", time_str);
	else
		snprintf(status, sizeof(status),
			 "Last refresh: %s  \u00B7 Online", time_str);
	FBInkOTConfig ots;
	ot_setup(&ots, s->screen_h - 30, l->margin, 200,
		 (unsigned) (l->font_meta - 2), false);
	FBInkConfig sc = g_cfg_draw;
	sc.fg_color = FG_GRAY6;
	fbink_print_ot(s->fbfd, status, &ots, &sc, NULL);

	fbink_refresh(s->fbfd, 0, 0, 0, 0, &g_cfg_flash);
}

// ---- touch ----

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

// ---- hit testing ----

int task_index_from_y(int touch_y, const int *card_bottoms,
		      int tasks_top, int task_count)
{
	if (task_count < 1) return -1;
	if (touch_y < tasks_top) return -1;

	// Find the card whose bottom is >= touch_y
	for (int i = 0; i < task_count; i++) {
		if (touch_y < card_bottoms[i]) return i;
	}
	return -1;
}

int col_from_x(int touch_x, int screen_w)
{
	return (touch_x < screen_w / 2) ? 0 : 1;
}

int button_hit(int touch_x, int touch_y, int button_y,
		int col_x)
{
	int btn_top = button_y;
	if (touch_y < btn_top || touch_y > btn_top + 28) return 0;
	if (touch_x > col_x && touch_x < col_x + 160) return 1;   // Done
	return 0;
}

int quit_hit(int touch_x, int touch_y, int screen_w, int screen_h)
{
	return (touch_x > screen_w - 70 && touch_y > screen_h - 70);
}

// ---- network ----

static int fetch_dashboard(struct AppState *s)
{
	char path[128];
	snprintf(path, sizeof(path), "/api/dashboard.php");
	struct HttpResponse resp;
	if (http_get(s->server_url, path, s->api_token, &resp) < 0) {
		fprintf(stderr, "homesrv: fetch failed (network error), "
			"attempting wifi reconnect\n");
		int rc = system(
			"wpa_cli -i wlan0 reassociate 2>/dev/null &");
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

static int api_complete(struct AppState *s, int task_id)
{
	char path[128]; snprintf(path, sizeof(path), "/api/tasks/complete.php");
	char post[64];  snprintf(post, sizeof(post), "task_id=%d", task_id);
	struct HttpResponse resp;
	if (http_post(s->server_url, path, s->api_token, post, &resp) < 0) {
		fprintf(stderr, "homesrv: complete id=%d failed (network error)\n", task_id);
		return -1;
	}
	if (resp.status != 200) {
		fprintf(stderr, "homesrv: complete id=%d failed (HTTP %d)\n", task_id, resp.status);
		http_free_response(&resp); return -1;
	}
	fprintf(stderr, "homesrv: complete id=%d ok\n", task_id);
	http_free_response(&resp);
	return 0;
}

static void render_task_completed(struct AppState *s, int idx)
{
	const struct Layout *l = &s->layout;
	int button_y = s->task_button_y[idx];
	int card_bottom = s->task_card_bottom[idx];

	// Clear from button area down to card bottom
	pen(LUM_WHITE);
	FBInkRect cr = { (unsigned short) l->col_left,
			 (unsigned short) button_y,
			 (unsigned short) l->col_half,
			 (unsigned short) (card_bottom - button_y) };
	FBInkConfig mc = g_cfg_draw;
	mc.wfm_mode = WFM_DU;
	mc.no_refresh = false;
	fbink_cls(s->fbfd, &mc, &cr, false);
	pen(LUM_WHITE);

	// "TASK COMPLETED!" at the same vertical position as the Done button
	FBInkOTConfig ot;
	ot_setup(&ot, button_y + 2, l->col_left,
		 l->margin,
		 (unsigned) l->font_btn, true);
	fbink_print_ot(s->fbfd, "TASK COMPLETED!", &ot, &mc, NULL);

	fbink_refresh(s->fbfd, (unsigned short) button_y,
		      (unsigned short) l->col_left,
		      (unsigned short) l->col_half,
		      (unsigned short) (card_bottom - button_y), &mc);
}

// ---- main ----

int main(int argc, char *argv[])
{
	if (argc > 1 && strcmp(argv[1], "--version") == 0) {
		printf("homesrv-kobo %s (FBInk %s)\n", HOMESRV_VERSION, fbink_version());
		return 0;
	}

	struct AppState s;
	memset(&s, 0, sizeof(s));
	s.last_refresh_success = time(NULL);

	const char *base_dir = (argc > 1) ? argv[1] : "/mnt/onboard/.adds/homesrv";
	snprintf(s.base_dir, sizeof(s.base_dir), "%s", base_dir);

	if (load_config(&s) < 0) {
		fprintf(stderr, "homesrv: failed to load config from %s/config\n", s.base_dir);
		return 1;
	}

	struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = sig_handler;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);

	if (init_display(&s) < 0) { fprintf(stderr, "homesrv: display init failed\n"); return 1; }

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

	// Clear screen and show startup status
	pen(LUM_WHITE);
	fbink_cls(s.fbfd, &g_cfg_draw, NULL, false);
	pen(LUM_WHITE);
	render_message(&s, "Starting HomeSrv...");
	fbink_refresh(s.fbfd, 0, 0, 0, 0, &g_cfg_flash);

	// Ensure Wi-Fi is connected before first fetch
	fprintf(stderr, "homesrv: checking wifi...\n");
	for (int attempt = 0; attempt < 5; attempt++) {
		struct HttpResponse pr;
		if (http_get(s.server_url, "/api/ping.php", s.api_token, &pr) == 0
		    && pr.status == 200) {
			http_free_response(&pr);
			fprintf(stderr, "homesrv: wifi ok (attempt %d)\n",
				attempt + 1);
			break;
		}
		fprintf(stderr, "homesrv: wifi not ready (attempt %d), "
			"reconnecting...\n", attempt + 1);
		{
			char buf[128];
			snprintf(buf, sizeof(buf),
				 "Connecting to Wi-Fi... (attempt %d)",
				 attempt + 1);
			render_message(&s, buf);
			fbink_refresh(s.fbfd, 0, 0, 0, 0, &g_cfg_flash);
		}
		system("wpa_cli -i wlan0 reassociate 2>/dev/null &");
		sleep(5);
	}

	int conn_ok = (fetch_dashboard(&s) == 0);
	if (!conn_ok) {
		char buf[128];
		snprintf(buf, sizeof(buf), "Connecting to %s...", s.server_url);
		render_message(&s, buf);
		fbink_refresh(s.fbfd, 0, 0, 0, 0, &g_cfg_flash);
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
			fbink_refresh(s.fbfd, 0, 0, 0, 0, &g_cfg_flash);
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
				render_full_screen(&s);
				fbink_wait_for_complete(s.fbfd, LAST_MARKER);
				s.last_refresh = now;
				s.refresh_pending = 0;
			} else {
				s.offline = 1;
				render_full_screen(&s);
				fbink_wait_for_complete(s.fbfd, LAST_MARKER);
			}
			continue;
		}

		// Full-screen flash every 2 hours to clear ghosting
		if (s.data_loaded && now - s.last_screen_refresh >= 7200) {
			fprintf(stderr, "homesrv: screen refresh (clear ghosting)\n");
			FBInkConfig cc = g_cfg_flash;
			cc.wfm_mode = WFM_GC16;
			cc.is_flashing = true;
			fbink_refresh(s.fbfd, 0, 0, 0, 0, &cc);
			fbink_wait_for_complete(s.fbfd, LAST_MARKER);
			s.last_screen_refresh = now;
		}

		// Clock partial refresh on minute boundaries
		static int last_minute = -1;
		if (s.data_loaded) {
			struct tm tm_buf;
			struct tm *tm = localtime_r(&now, &tm_buf);
			int min = tm->tm_min;
			if (last_minute != -1 && last_minute != min) {
				int ctop = 40;
				pen(LUM_WHITE);
				FBInkRect cr = { 0, 0,
						 (unsigned short) s.screen_w, 100 };
				fbink_cls(s.fbfd, &g_cfg_draw, &cr, false);
				pen(LUM_WHITE);
				render_clock_row(&s, ctop);
				FBInkConfig duc = g_cfg_du;
				duc.no_refresh = false;
				fbink_refresh(s.fbfd, 0, 0, (unsigned) s.screen_w, 100, &duc);
			}
			last_minute = min;
		}

		// Periodic connectivity check every 10s
		static time_t last_conn_check = 0;
		if (s.data_loaded && now - last_conn_check >= 10) {
			struct HttpResponse pr;
			int was_offline = s.offline;
			if (http_get(s.server_url, "/api/ping.php", s.api_token,
				     &pr) == 0 && pr.status == 200) {
				s.offline = 0;
				http_free_response(&pr);
			} else {
				s.offline = 1;
			}
			last_conn_check = now;
			// Status changed — partial refresh bottom area
			if (was_offline != s.offline) {
				pen(LUM_WHITE);
				FBInkRect cr = { 0,
						 (unsigned short) (s.screen_h - 40),
						 (unsigned short) s.screen_w, 40 };
				fbink_cls(s.fbfd, &g_cfg_draw, &cr, false);
				pen(LUM_WHITE);
				// Re-render just the status line
				char status[128];
				struct tm ltm_buf;
				struct tm *ltm = localtime_r(
					&s.last_refresh_success, &ltm_buf);
				char time_str[16];
				strftime(time_str, sizeof(time_str),
					 "%H:%M", ltm);
				if (s.offline)
					snprintf(status, sizeof(status),
						 "Last refresh: %s  \u00B7 "
						 "Offline",
						 time_str);
				else
					snprintf(status, sizeof(status),
						 "Last refresh: %s  \u00B7 "
						 "Online",
						 time_str);
				FBInkOTConfig ots;
				const struct Layout *l = &s.layout;
				ot_setup(&ots, s.screen_h - 30,
					 l->margin, 200,
					 (unsigned) (l->font_meta - 2), false);
				FBInkConfig sc = g_cfg_draw;
				sc.fg_color = FG_GRAY6;
				fbink_print_ot(s.fbfd, status, &ots, &sc,
					       NULL);
				FBInkConfig duc = g_cfg_du;
				duc.no_refresh = false;
				fbink_refresh(s.fbfd,
					      (unsigned) (s.screen_h - 40), 0,
					      (unsigned) s.screen_w, 40, &duc);
			}
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
					rc = libevdev_next_event(evdev, LIBEVDEV_READ_FLAG_SYNC, &ev);
			}
		} while (rc == LIBEVDEV_READ_STATUS_SYNC || rc == LIBEVDEV_READ_STATUS_SUCCESS);

		if (!got_touch) continue;

		fprintf(stderr, "homesrv: touch raw=%d,%d down=%d\n", touch_x, touch_y, touch_down);
		if (!touch_down) continue;

		int sx, sy;
		transform_touch(touch_x, touch_y, &sx, &sy, s.screen_w, s.screen_h);

		if (quit_hit(sx, sy, s.screen_w, s.screen_h)) {
			fprintf(stderr, "homesrv: quit triggered\n");
			g_running = 0;
			continue;
		}

		int col = col_from_x(sx, s.screen_w);
		if (col == 0) {
			int idx = task_index_from_y(sy, s.task_card_bottom,
						   s.column_y, s.task_count);
			if (idx < 0) {
				fprintf(stderr, "homesrv: touch missed tasks y=%d\n", sy);
				continue;
			}

			const struct Task *t = &s.data.tasks[idx];

			int btn = button_hit(sx, sy,
					    s.task_button_y[idx],
					    s.layout.col_left);
			if (btn == 1) {
				fprintf(stderr, "homesrv: done id=%d\n", t->id);
				render_task_completed(&s, idx);
				sleep(2);
				if (api_complete(&s, t->id) == 0)
					s.refresh_pending = 1;
			}
		}
	}

	fprintf(stderr, "homesrv: clearing screen on exit\n");
	pen(LUM_WHITE);
	fbink_cls(s.fbfd, &g_cfg_draw, NULL, false);
	fbink_refresh(s.fbfd, 0, 0, 0, 0, &g_cfg_flash);
	fbink_wait_for_complete(s.fbfd, LAST_MARKER);

	if (evdev) libevdev_free(evdev);
	if (s.touch_fd >= 0) close(s.touch_fd);
	if (s.font_loaded) fbink_free_ot_fonts();
	fbink_close(s.fbfd);
	return 0;
}
