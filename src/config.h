// The client's config file: which servers it can talk to.
//
// More than one, so the device can be pointed at a test server and back
// without an edit-and-reboot. The menu (see screens.h) switches between them
// at runtime; this file is only about reading the list.
//
// Parsing lives apart from main.c so the host preview can render the menu from
// a real config file instead of imitating one.

#ifndef HOMESRV_CONFIG_H
#define HOMESRV_CONFIG_H

// Six is what fits on the menu screen without scrolling, which is well past
// the two or three anyone actually has.
#define MAX_SERVERS 6

struct Server {
	char name[32];    // for the menu; defaults to the host:port
	char url[160];
	char token[64];
};

struct Config {
	struct Server servers[MAX_SERVERS];
	int           count;
};

// Read `path`. Returns 0 if at least one usable server was found, -1 if not
// (in which case `cfg->count` is 0 and the caller should say so and stop).
//
// The file is `KEY=value` lines, `#` comments, in any order:
//
//   SERVER_1_NAME=Prod
//   SERVER_1_URL=http://192.168.1.50:8082
//   SERVER_1_TOKEN=abc123
//   SERVER_2_NAME=Test
//   ...
//
// NAME is optional and defaults to the host:port. Gaps in the numbering are
// closed up, so SERVER_1 and SERVER_3 with no SERVER_2 is two servers rather
// than a hole in the menu.
int config_load(const char *path, struct Config *cfg);

#endif
