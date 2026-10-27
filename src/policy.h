// How often the client does things: refetch, ping, flash the panel clear.
//
// A server says what it wants in its envelope (see UiEnvelope); this turns
// that into what the client will actually do — the client's default for
// anything left unsaid, and a floor under every interval so a typo in someone's
// envelope cannot make the device hammer their server or wear on the panel.
//
// Kept apart from main.c so the rules build, and are tested, on the host.

#ifndef KOBOARD_POLICY_H
#define KOBOARD_POLICY_H

#include "ui_json.h"

#define DEFAULT_REFRESH_SECONDS 1800
#define DEFAULT_PING_SECONDS      10
#define DEFAULT_FLASH_SECONDS   7200
#define MIN_REFRESH_SECONDS       15
#define MIN_PING_SECONDS           5
#define MIN_FLASH_SECONDS        300

struct Policy {
	int  refresh_seconds;
	int  ping_seconds;        // 0 = no ping: online/offline follows fetches
	int  flash_seconds;       // 0 = never
	char ping_url[384];       // resolved against the screen URL; "" = none
};

// The policy before a server has said anything.
void policy_defaults(struct Policy *p);

// What `env` asks for, made safe. `screen_url` is what relative URLs in the
// envelope resolve against. A ping URL that cannot be used (https, malformed,
// too long) is dropped, and the ping with it. `env` may be NULL, which is the
// same as policy_defaults().
void policy_from_envelope(struct Policy *p, const UiEnvelope *env,
			  const char *screen_url);

#endif
