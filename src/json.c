#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void safe_str(char *dst, size_t dstsize, const char *src)
{
	if (!src) {
		dst[0] = '\0';
		return;
	}
	strncpy(dst, src, dstsize - 1);
	dst[dstsize - 1] = '\0';
}

static int safe_int(const cJSON *item)
{
	if (!item) return 0;
	if (cJSON_IsNumber(item)) return item->valueint;
	if (cJSON_IsString(item)) return atoi(item->valuestring);
	return 0;
}

int json_parse_dashboard(const char *json, struct DashboardData *data)
{
	memset(data, 0, sizeof(*data));

	cJSON *root = cJSON_Parse(json);
	if (!root) return -1;

	safe_str(data->date, sizeof(data->date),
		 cJSON_GetStringValue(cJSON_GetObjectItem(root, "date")));
	safe_str(data->date_iso, sizeof(data->date_iso),
		 cJSON_GetStringValue(cJSON_GetObjectItem(root, "date_iso")));

	cJSON *tasks = cJSON_GetObjectItem(root, "tasks");
	if (tasks && cJSON_IsArray(tasks)) {
		cJSON *item;
		int i = 0;
		cJSON_ArrayForEach(item, tasks) {
			if (i >= MAX_TASKS) break;
			struct Task *t = &data->tasks[i];
			safe_str(t->title, sizeof(t->title),
				 cJSON_GetStringValue(cJSON_GetObjectItem(item, "title")));
			safe_str(t->next_due, sizeof(t->next_due),
				 cJSON_GetStringValue(cJSON_GetObjectItem(item, "next_due")));
			safe_str(t->due_time, sizeof(t->due_time),
				 cJSON_GetStringValue(cJSON_GetObjectItem(item, "due_time")));
			safe_str(t->recur_type, sizeof(t->recur_type),
				 cJSON_GetStringValue(cJSON_GetObjectItem(item, "recur_type")));
			safe_str(t->assigned_name, sizeof(t->assigned_name),
				 cJSON_GetStringValue(cJSON_GetObjectItem(item, "assigned_name")));
			safe_str(t->status, sizeof(t->status),
				 cJSON_GetStringValue(cJSON_GetObjectItem(item, "status")));
			t->id = safe_int(cJSON_GetObjectItem(item, "id"));
			i++;
		}
		data->task_count = i;
	}

	cJSON *events = cJSON_GetObjectItem(root, "calendar_events");
	if (events && cJSON_IsArray(events)) {
		cJSON *item;
		int i = 0;
		cJSON_ArrayForEach(item, events) {
			if (i >= MAX_EVENTS) break;
			struct CalendarEvent *e = &data->events[i];
			safe_str(e->summary, sizeof(e->summary),
				 cJSON_GetStringValue(cJSON_GetObjectItem(item, "summary")));
			safe_str(e->time_str, sizeof(e->time_str),
				 cJSON_GetStringValue(cJSON_GetObjectItem(item, "time")));
			safe_str(e->color, sizeof(e->color),
				 cJSON_GetStringValue(cJSON_GetObjectItem(item, "color")));
			safe_str(e->feed_name, sizeof(e->feed_name),
				 cJSON_GetStringValue(cJSON_GetObjectItem(item, "feed_name")));
			e->all_day  = safe_int(cJSON_GetObjectItem(item, "all_day"));
			e->is_today = safe_int(cJSON_GetObjectItem(item, "is_today"));
			i++;
		}
		data->event_count = i;
	}

	cJSON *weather = cJSON_GetObjectItem(root, "weather");
	if (weather && !cJSON_IsNull(weather)) {
		data->has_weather = 1;
		struct Weather *w = &data->weather;
		w->temperature = safe_int(cJSON_GetObjectItem(weather, "temperature"));
		w->code        = safe_int(cJSON_GetObjectItem(weather, "code"));
		w->humidity    = safe_int(cJSON_GetObjectItem(weather, "humidity"));
		w->rain_chance = safe_int(cJSON_GetObjectItem(weather, "rain_chance"));
		safe_str(w->description, sizeof(w->description),
			 cJSON_GetStringValue(cJSON_GetObjectItem(weather, "description")));

		cJSON *forecast = cJSON_GetObjectItem(weather, "forecast");
		if (forecast && cJSON_IsArray(forecast)) {
			cJSON *fitem;
			int j = 0;
			cJSON_ArrayForEach(fitem, forecast) {
				if (j >= MAX_FORECAST) break;
				struct Forecast *f = &w->forecast[j];
				safe_str(f->name, sizeof(f->name),
					 cJSON_GetStringValue(cJSON_GetObjectItem(fitem, "name")));
				f->high = safe_int(cJSON_GetObjectItem(fitem, "high"));
				f->low  = safe_int(cJSON_GetObjectItem(fitem, "low"));
				f->code = safe_int(cJSON_GetObjectItem(fitem, "code"));
				safe_str(f->description, sizeof(f->description),
					 cJSON_GetStringValue(cJSON_GetObjectItem(fitem, "description")));
				j++;
			}
			w->forecast_count = j;
		}
	}

	cJSON_Delete(root);
	return 0;
}

void json_free_dashboard(struct DashboardData *data)
{
	(void) data;
}
