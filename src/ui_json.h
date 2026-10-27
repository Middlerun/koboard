// Build a UI tree from the server's JSON.
//
// The mirror of ui_write_json(): the wire format is the one documented in
// docs/ui-schema.md, so a dumped tree can be fed straight back in. Unknown
// properties are ignored on purpose — that is what lets the server describe
// something a slightly older client has never heard of without breaking it.
//
// Everything is allocated in the frame arena, so there is nothing to free: the
// tree dies with the next ui_arena_reset().

#ifndef HOMESRV_UI_JSON_H
#define HOMESRV_UI_JSON_H

#include <time.h>

#include "ui.h"

// The wire format this client understands. The server sends its own version in
// the envelope; a newer one is refused rather than half-parsed.
#define UI_SCHEMA_VERSION 1

// Values only the device knows, substituted into text as it is parsed.
//
// The server cannot send the time: by the time a tree is fetched it is already
// stale, and it has to keep ticking between fetches and while offline. So the
// server sends a placeholder and the client fills it in on every frame — which
// is what lets the clock live in the server's layout rather than in the chrome.
//
//   "{clock:%a, %b %e}"  ->  "Fri, Sep 26"
//   "{clock}"            ->  "08:01"        (%H:%M by default)
//
// A placeholder naming something unknown is left exactly as it was, so a
// string like "… +{n} more" (expanded later, by the layout engine) is safe.
typedef struct {
	time_t now;
} UiVars;

// Everything the envelope says besides the tree itself:
//
//   { "schema": 1,
//     "refresh": { "seconds": 1800, "ping": "/api/ping.php",
//                  "pingSeconds": 10, "flashSeconds": 7200 },
//     "root":    { ... } }
//
// Every field is optional. UI_ENV_UNSET means the server did not say, and the
// caller falls back to its own default; what the values are allowed to be is
// the caller's policy too, so nothing here clamps them.
#define UI_ENV_UNSET (-1)

typedef struct {
	int  refresh_seconds;   // refetch the screen this often
	int  ping_seconds;      // check `ping` this often; 0 = never
	int  flash_seconds;     // full-screen flash to clear ghosting; 0 = never
	char ping[256];         // as sent; "" = none. Resolve against the screen URL.
} UiEnvelope;

// Parse `json` — either an envelope ({"schema":1,"root":{...}}) or a bare node
// — into `a`. `vars` may be NULL, which leaves every placeholder untouched.
// `env` may be NULL; if not, it is filled in (all UI_ENV_UNSET for a bare
// node). Returns the root, or NULL with a reason in `err`.
UiNode *ui_from_json(UiArena *a, const char *json, const UiVars *vars,
		     UiEnvelope *env, char *err, size_t errsz);

#endif
