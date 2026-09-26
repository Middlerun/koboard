#include "screens.h"

#include <stdio.h>
#include <string.h>

void theme_init(struct Theme *t, int screen_w)
{
	memset(t, 0, sizeof(*t));
	t->margin = screen_w / 30;
	if (t->margin < 10) t->margin = 10;
	t->gap = t->margin;
	// Spacing between events that have already finished: they are reference
	// material rather than things to act on, so they pack together.
	t->gap_tight = t->margin / 3;
	if (t->gap_tight < 4) t->gap_tight = 4;

	t->font_date     = 14;
	t->font_time     = 12;
	t->font_temp     = 30;
	t->font_detail   = 11;
	t->font_fcast    = 10;
	t->font_header   = 14;
	t->font_title    = 13;
	t->font_meta     = 9;
	t->font_btn      = 9;
	t->font_evt      = 11;
	t->font_evt_sub  = 8;
	t->font_status   = 7;
	t->font_msg      = 14;
}

const char *weather_icon(int code)
{
	// WMO weather interpretation codes
	// https://open-meteo.com/en/docs
	if (code == 0)   return "☀";       // ☀ clear sky
	if (code == 1)   return "\U0001F324";   // 🌤 mainly clear
	if (code == 2)   return "⛅";       // ⛅ partly cloudy
	if (code == 3)   return "☁";       // ☁ overcast
	if (code <= 48)  return "\U0001F32B";   // 🌫 fog / rime fog
	if (code <= 57)  return "\U0001F327";   // 🌧 drizzle / freezing drizzle
	if (code <= 67)  return "\U0001F327";   // 🌧 rain / freezing rain
	if (code <= 77)  return "❄";       // ❄ snow fall / snow grains
	if (code <= 82)  return "\U0001F326";   // 🌦 rain showers
	if (code <= 86)  return "\U0001F328";   // 🌨 snow showers
	if (code <= 99)  return "⛈";       // ⛈ thunderstorm / thunderstorm with hail
	return "?";                             // Unknown
}

// ---- date helpers --------------------------------------------------------

// Days from `date_iso` (YYYY-MM-DD) to `next_due`. Positive = future.
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

// "Due today", "Due on Tuesday", "Was due on 05/09", ...
static void format_due_label(const struct DashboardData *data,
			     const struct Task *t, char *buf, size_t bufsz)
{
	int diff = days_from_today(data->date_iso, t->next_due);
	int yr, mo, dy;
	int has_date = sscanf(t->next_due, "%d-%d-%d", &yr, &mo, &dy) == 3;

	if (diff == 0)  { snprintf(buf, bufsz, "Due today"); return; }
	if (diff == -1) { snprintf(buf, bufsz, "Was due yesterday"); return; }

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

// ---- sections ------------------------------------------------------------

static UiNode *spacer(UiArena *a, int px)
{
	return BOX(a, .h = PX(px));
}

static UiNode *clock_row(UiArena *a, const struct Theme *t, time_t now)
{
	struct tm tm_buf;
	struct tm *tm = localtime_r(&now, &tm_buf);
	char date[64], clock[16];
	strftime(date, sizeof(date), "%a, %b %e", tm);
	strftime(clock, sizeof(clock), "%I:%M", tm);

	return KIDS(BOX(a, .key = KEY_CLOCK, .dir = UI_ROW, .gap = t->gap,
			.cross = UI_CROSS_END),
		TEXT(a, date, .size_pt = t->font_date, .w = FLEX(1), .max_lines = 1),
		TEXT(a, clock, .size_pt = t->font_time, .align = UI_ALIGN_END,
		     .max_lines = 1));
}

static UiNode *weather_block(UiArena *a, const struct Theme *t,
			     const struct Weather *w)
{
	// Append using strlen rather than snprintf's return value, which reports
	// the length it *would* have written and so can walk past the buffer.
	char detail[192];
	snprintf(detail, sizeof(detail), "%s", w->description);
	size_t n = strlen(detail);
	snprintf(detail + n, sizeof(detail) - n, " · %d°/%d°",
		      w->today_high, w->today_low);
	n = strlen(detail);
	snprintf(detail + n, sizeof(detail) - n, " · ☂%d%%",
		      w->rain_chance);
	n = strlen(detail);
	snprintf(detail + n, sizeof(detail) - n, " · \U0001F4A7%d%%",
		 w->humidity);

	return KIDS(BOX(a, .dir = UI_COL, .gap = 2),
		TEXTF(a, (.size_pt = t->font_temp, .align = UI_ALIGN_CENTER,
			  .max_lines = 1),
		      "%s  %d°", weather_icon(w->code), w->temperature),
		TEXT(a, detail, .size_pt = t->font_detail,
		     .align = UI_ALIGN_CENTER, .max_lines = 1));
}

static UiNode *forecast_row(UiArena *a, const struct Theme *t,
			    const struct Weather *w)
{
	UiNode *row = BOX(a, .dir = UI_ROW, .gap = t->gap);
	for (int i = 0; i < w->forecast_count; i++) {
		const struct Forecast *f = &w->forecast[i];
		ui_add(row, KIDS(BOX(a, .dir = UI_COL, .w = FLEX(1), .gap = 2),
			TEXT(a, f->name, .size_pt = t->font_fcast,
			     .align = UI_ALIGN_CENTER, .max_lines = 1),
			TEXT(a, weather_icon(f->code), .size_pt = t->font_fcast + 4,
			     .align = UI_ALIGN_CENTER, .max_lines = 1),
			TEXTF(a, (.size_pt = t->font_fcast, .align = UI_ALIGN_CENTER,
				  .max_lines = 1),
			      "%d°/%d°", f->high, f->low)));
	}
	return row;
}

static UiNode *task_card(UiArena *a, const struct Theme *t,
			 const struct DashboardData *d, const struct Task *task)
{
	char meta[256];
	format_due_label(d, task, meta, sizeof(meta));
	if (task->due_time[0]) {
		int n = (int) strlen(meta);
		snprintf(meta + n, sizeof(meta) - (size_t) n, " · %s", task->due_time);
	}
	if (task->assigned_name[0]) {
		int n = (int) strlen(meta);
		snprintf(meta + n, sizeof(meta) - (size_t) n, " · %s",
			 task->assigned_name);
	}
	if (task->recur_type[0]) {
		int n = (int) strlen(meta);
		snprintf(meta + n, sizeof(meta) - (size_t) n, " · %s",
			 task->recur_type);
	}

	// The Done button sizes itself from its padding, so the touch target grows
	// with the screen's DPI instead of being a hard-coded pixel height.
	UiNode *button = KIDS(BOX(a, .dir = UI_ROW, .w = PCT(60),
				  .main = UI_MAIN_CENTER, .cross = UI_CROSS_CENTER,
				  .pad = PAD_XY(8, 12), .bg = UI_DARK,
				  .action = ACTION(a, .kind = UI_ACT_POST,
						   .path = "/api/tasks/complete.php",
						   .body = ui_strf(a, "task_id=%d", task->id),
						   // Kept short so it fits the button on one line at
							   // both panel sizes (see docs/ui-schema.md).
							   .toast = "COMPLETED!")),
		TEXT(a, "Done", .size_pt = t->font_btn, .fg = UI_WHITE,
		     .align = UI_ALIGN_CENTER, .max_lines = 1));

	return KIDS(BOX(a, .dir = UI_COL, .gap = 4,
			.key = ui_strf(a, "task-%d", task->id)),
		TEXT(a, task->title, .size_pt = t->font_title, .max_lines = 2),
		TEXT(a, meta, .size_pt = t->font_meta, .max_lines = 1),
		button);
}

static UiNode *tasks_column(UiArena *a, const struct Theme *t,
			    const struct DashboardData *d)
{
	UiNode *col = BOX(a, .dir = UI_COL, .w = FLEX(1), .gap = t->gap,
			  .pad = { 0, t->gap, 0, t->margin },
			  .clip = true, .overflow_text = "… +{n} more");
	ui_add(col, TEXT(a, "TASKS", .size_pt = t->font_header, .max_lines = 1));
	if (d->task_count == 0)
		ui_add(col, TEXT(a, "Nothing to do", .size_pt = t->font_meta,
				 .max_lines = 1));
	for (int i = 0; i < d->task_count; i++)
		ui_add(col, task_card(a, t, d, &d->tasks[i]));
	return col;
}

static UiNode *event_row(UiArena *a, const struct Theme *t,
			 const struct CalendarEvent *e)
{
	int font = e->is_over ? t->font_evt_sub : t->font_evt;
	const char *when = e->all_day ? "All day" : e->time_str;

	// Summary and feed name share one column beside the time, so the feed name
	// lines up with the summary instead of floating under the time.
	UiNode *detail = KIDS(BOX(a, .dir = UI_COL, .w = FLEX(1), .gap = 2),
		TEXT(a, e->summary, .size_pt = font,
		     .max_lines = e->is_over ? 1 : 3));

	if (!e->is_over)
		ui_add(detail, TEXTF(a, (.size_pt = t->font_evt_sub, .max_lines = 1),
				     "\u2014 %s", e->feed_name));

	return KIDS(BOX(a, .dir = UI_ROW, .gap = t->gap),
		TEXT(a, when, .size_pt = font, .w = PCT(37), .max_lines = 1),
		detail);
}

// Add an item to the calendar column. The column's own gap is the tight one,
// so anything that wants normal separation asks for it here. Leading items
// never get extra space, which keeps the column top-aligned with TASKS.
static void cal_add(const struct Theme *t, UiNode *col, UiNode *item, bool tight)
{
	if (col->first && !tight)
		item->pad[0] = (int16_t) (item->pad[0] + (t->gap - t->gap_tight));
	ui_add(col, item);
}

static void add_events(UiArena *a, const struct Theme *t, UiNode *col,
		       const struct DashboardData *d, int today)
{
	int count = 0;
	for (int i = 0; i < d->event_count; i++) {
		const struct CalendarEvent *e = &d->events[i];
		if (e->is_today != today) continue;
		cal_add(t, col, event_row(a, t, e), e->is_over);
		count++;
	}
	if (count == 0)
		cal_add(t, col, TEXT(a, "Nothing scheduled", .size_pt = t->font_meta,
				     .max_lines = 1), false);
}

static UiNode *calendar_column(UiArena *a, const struct Theme *t,
			       const struct DashboardData *d)
{
	UiNode *col = BOX(a, .dir = UI_COL, .w = FLEX(1), .gap = t->gap_tight,
			  .pad = { 0, t->margin, 0, t->gap },
			  .clip = true, .overflow_text = "\u2026 +{n} more");

	cal_add(t, col, TEXT(a, "TODAY", .size_pt = t->font_header,
			     .max_lines = 1), false);
	add_events(a, t, col, d, 1);
	cal_add(t, col, TEXT(a, "TOMORROW", .size_pt = t->font_header,
			     .max_lines = 1), false);
	add_events(a, t, col, d, 0);
	return col;
}

static UiNode *status_row(UiArena *a, const struct Theme *t,
			  const struct ScreenCtx *ctx)
{
	struct tm tm_buf;
	struct tm *tm = localtime_r(&ctx->last_refresh_success, &tm_buf);
	char when[16];
	strftime(when, sizeof(when), "%H:%M", tm);

	// The quit target is a real node with a real rect, so hit testing finds it
	// the same way it finds everything else. It draws nothing.
	return KIDS(BOX(a, .key = KEY_STATUS, .dir = UI_ROW,
			.main = UI_MAIN_BETWEEN, .cross = UI_CROSS_END,
			.pad = { 0, t->margin, 12, t->margin }),
		TEXTF(a, (.size_pt = t->font_status, .fg = UI_MED, .max_lines = 1),
		      "Last refresh: %s  · %s", when,
		      ctx->offline ? "Offline" : "Online"),
		BOX(a, .key = KEY_QUIT, .w = PX(70), .h = PX(44),
		    .action = ACTION(a, .kind = UI_ACT_QUIT)));
}

// ---- screens -------------------------------------------------------------

UiNode *screen_dashboard(UiArena *a, const struct Theme *t,
			 const struct DashboardData *d,
			 const struct ScreenCtx *ctx)
{
	UiNode *root = BOX(a, .dir = UI_COL, .bg = UI_WHITE);

	UiNode *header = BOX(a, .dir = UI_COL,
			     .pad = { 40, (int16_t) t->margin, 0, (int16_t) t->margin });
	ui_add(header, clock_row(a, t, ctx->now));
	ui_add(header, spacer(a, 20));
	if (d->has_weather) {
		ui_add(header, weather_block(a, t, &d->weather));
		ui_add(header, spacer(a, 40));
		if (d->weather.forecast_count > 0) {
			ui_add(header, forecast_row(a, t, &d->weather));
			ui_add(header, spacer(a, 30));
		}
	}
	ui_add(root, header);

	// Full-bleed divider: it sits outside the header's padding.
	ui_add(root, BOX(a, .h = PX(4), .bg = UI_DARK));
	ui_add(root, spacer(a, 30));

	ui_add(root, KIDS(BOX(a, .dir = UI_ROW, .h = FLEX(1)),
		tasks_column(a, t, d),
		BOX(a, .w = PX(4), .bg = UI_DARK),
		calendar_column(a, t, d)));

	ui_add(root, status_row(a, t, ctx));
	return root;
}

UiNode *screen_message(UiArena *a, const struct Theme *t, const char *msg)
{
	return KIDS(BOX(a, .dir = UI_COL, .bg = UI_WHITE,
			.main = UI_MAIN_CENTER, .cross = UI_CROSS_CENTER,
			.pad = PAD_XY(0, (int16_t) t->margin)),
		TEXT(a, msg, .size_pt = t->font_msg, .align = UI_ALIGN_CENTER));
}
