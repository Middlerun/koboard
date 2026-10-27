#include "policy.h"

#include "http.h"

#include <stdio.h>
#include <string.h>

static int at_least(int v, int floor) { return v < floor ? floor : v; }

// An interval where 0 means "never": unset takes the default, 0 stays 0, and
// anything else is held to the floor.
static int optional_interval(int v, int def, int floor)
{
	if (v == UI_ENV_UNSET) return def;
	if (v == 0) return 0;
	return at_least(v, floor);
}

void policy_defaults(struct Policy *p)
{
	memset(p, 0, sizeof(*p));
	p->refresh_seconds = DEFAULT_REFRESH_SECONDS;
	p->ping_seconds    = 0;
	p->flash_seconds   = DEFAULT_FLASH_SECONDS;
}

void policy_from_envelope(struct Policy *p, const UiEnvelope *env,
			  const char *screen_url)
{
	policy_defaults(p);
	if (!env) return;

	if (env->refresh_seconds != UI_ENV_UNSET)
		p->refresh_seconds = at_least(env->refresh_seconds,
					      MIN_REFRESH_SECONDS);
	p->flash_seconds = optional_interval(env->flash_seconds,
					     DEFAULT_FLASH_SECONDS,
					     MIN_FLASH_SECONDS);

	// Checked once, here, so an unusable ping URL costs one log line rather
	// than a failed request every few seconds.
	if (!env->ping[0]) return;
	struct Url u;
	if (url_resolve(screen_url, env->ping, p->ping_url, sizeof(p->ping_url)) < 0
	    || url_parse(p->ping_url, &u) < 0) {
		fprintf(stderr, "koboard: ignoring unusable ping URL %s\n", env->ping);
		p->ping_url[0] = '\0';
		return;
	}
	p->ping_seconds = optional_interval(env->ping_seconds,
					    DEFAULT_PING_SECONDS,
					    MIN_PING_SECONDS);
}
