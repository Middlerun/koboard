#ifndef HOMESRV_LAYOUT_H
#define HOMESRV_LAYOUT_H

#include <time.h>
#include "json.h"
#include "fbink.h"

struct Layout {
	int margin;
	int gap;
	int col_half;
	int col_left;
	int col_right;
	int modal_w;
	int modal_h;
	int modal_opt_h;
	int font_date;
	int font_temp;
	int font_detail;
	int font_fcast;
	int font_header;
	int font_title;
	int font_meta;
	int font_btn;
	int font_evt;
	int font_evt_sub;
	int font_modal;
	int font_err;
};

void layout_compute(struct Layout *l, int screen_w, int screen_h);

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
	int    column_y;
	int    task_card_bottom[MAX_TASKS];
	int    task_count;
	struct Layout layout;

	struct DashboardData data;
	int    data_loaded;

	int    modal_open;
	int    modal_task_id;
	char   modal_title[128];

	time_t last_refresh;
	time_t last_refresh_success;
	int    offline;
	int    refresh_pending;
};

int load_config(struct AppState *state);

int  render_clock_row(const struct AppState *s, int top);
int  render_weather(const struct AppState *s, int top);
int  render_forecast(const struct AppState *s, int top);
int  render_tasks(struct AppState *s, int top, int col_x, int col_w);
int  render_calendar(const struct AppState *s, int top, int col_x, int col_w);
void render_modal(const struct AppState *s);
void render_message(const struct AppState *s, const char *msg);
void render_full_screen(struct AppState *s);

int  init_display(struct AppState *s);
void ot_setup(FBInkOTConfig *ot, int top, int left, int right,
	      unsigned int size_pt, int centered);
int  ot_print(int fbfd, const char *text, FBInkOTConfig *ot);
int  ot_print_top(int fbfd, const char *text, FBInkOTConfig *ot, int *top);
void draw_divider(int fbfd, int y, int x1, int x2, uint8_t gray);
void draw_filled_box(int fbfd, int x, int y, int w, int h, uint8_t gray);

int  init_touch(const char *dev_path);

int  task_index_from_y(int touch_y, const int *card_bottoms,
		       int tasks_top, int task_count);
int  modal_option_from_y(int touch_y, int screen_h, int modal_h);
int  col_from_x(int touch_x, int screen_w);
int  button_hit(int touch_x, int touch_y, int card_bottom, int col_x);
int  quit_hit(int touch_x, int touch_y, int screen_w, int screen_h);

#endif