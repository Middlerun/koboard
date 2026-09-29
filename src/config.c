#include "config.h"

#include <stdio.h>
#include <string.h>

static void set_field(char *dst, size_t dstsz, const char *value)
{
	snprintf(dst, dstsz, "%s", value);
}

// "http://192.168.1.50:8082/" -> "192.168.1.50:8082", so a server with no
// name of its own still shows as something a person can tell apart.
static void name_from_url(struct Server *s)
{
	const char *host = strstr(s->url, "://");
	host = host ? host + 3 : s->url;

	size_t n = strcspn(host, "/");
	if (n >= sizeof(s->name)) n = sizeof(s->name) - 1;
	memcpy(s->name, host, n);
	s->name[n] = '\0';
}

int config_load(const char *path, struct Config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));

	FILE *f = fopen(path, "r");
	if (!f) return -1;

	struct Server slots[MAX_SERVERS];
	memset(slots, 0, sizeof(slots));

	char line[320];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '#' || line[0] == '\0') continue;

		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		const char *key = line, *value = eq + 1;

		int n = 0;
		char field[16] = { 0 };
		if (sscanf(key, "SERVER_%d_%15s", &n, field) != 2) continue;
		if (n < 1 || n > MAX_SERVERS) continue;

		struct Server *s = &slots[n - 1];
		if      (strcmp(field, "NAME")  == 0) set_field(s->name,  sizeof(s->name),  value);
		else if (strcmp(field, "URL")   == 0) set_field(s->url,   sizeof(s->url),   value);
		else if (strcmp(field, "TOKEN") == 0) set_field(s->token, sizeof(s->token), value);
	}
	fclose(f);

	// Keep the numbering's order but close the gaps, so SERVER_1 and SERVER_3
	// with no SERVER_2 is two servers rather than a hole in the menu.
	for (int i = 0; i < MAX_SERVERS; i++) {
		if (slots[i].url[0] == '\0') continue;
		struct Server *dst = &cfg->servers[cfg->count++];
		*dst = slots[i];
		if (dst->name[0] == '\0') name_from_url(dst);
	}
	return cfg->count > 0 ? 0 : -1;
}
