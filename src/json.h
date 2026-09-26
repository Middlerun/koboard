#ifndef HOMESRV_JSON_H
#define HOMESRV_JSON_H

#include "cJSON.h"

#define MAX_TASKS    50
#define MAX_EVENTS   40
#define MAX_FORECAST 4

struct Task {
	int    id;
	char   title[128];
	char   next_due[16];
	char   due_time[8];
	char   recur_type[16];
	char   assigned_name[64];
	char   status[12];     // "overdue", "today", "future"
};

struct CalendarEvent {
	char   summary[128];
	char   time_str[16];    // "2:30pm" or empty for all-day
	char   color[16];       // "#3b82f6"
	char   feed_name[64];
	int    all_day;
	int    is_today;
	int    is_over;
};

struct Forecast {
	char   name[8];          // "Mon"
	int    high;
	int    low;
	int    code;
	char   description[32];
};

struct Weather {
	int    temperature;
	int    today_high;
	int    today_low;
	int    code;
	char   description[32];
	int    humidity;
	double rain_mm;
	struct Forecast forecast[MAX_FORECAST];
	int    forecast_count;
};

struct DashboardData {
	char   date[64];
	char   date_iso[16];
	struct Task tasks[MAX_TASKS];
	int    task_count;
	struct CalendarEvent events[MAX_EVENTS];
	int    event_count;
	struct Weather weather;
	int    has_weather;
};

int  json_parse_dashboard(const char *json, struct DashboardData *data);
void json_free_dashboard(struct DashboardData *data);

#endif
