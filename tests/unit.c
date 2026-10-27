// Unit tests for the parts of the client a server's choices flow through:
// URL handling, the envelope, and the refresh policy built from it.
//
// The golden tests cover layout; these cover the protocol, where an odd value
// in someone else's server is most likely to land. Built and run on the host
// by `make test`.

#include "http.h"
#include "policy.h"
#include "ui.h"
#include "ui_json.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		       printf(__VA_ARGS__); printf("\n"); } \
} while (0)

// ---- URLs ----------------------------------------------------------------

static void parses(const char *url, const char *host, int port, const char *path)
{
	struct Url u;
	int rc = url_parse(url, &u);
	CHECK(rc == 0 && !strcmp(u.host, host) && u.port == port && !strcmp(u.path, path),
	      "url_parse(%s) = %d %s:%d%s, want %s:%d%s",
	      url, rc, u.host, u.port, u.path, host, port, path);
}

static void rejects(const char *url)
{
	struct Url u;
	CHECK(url_parse(url, &u) < 0, "url_parse(%s) should fail", url);
}

static void resolves(const char *base, const char *ref, const char *want)
{
	char out[256];
	int rc = url_resolve(base, ref, out, sizeof(out));
	CHECK(rc == 0 && !strcmp(out, want), "url_resolve(%s, %s) = %s, want %s",
	      base, ref, rc ? "(error)" : out, want);
}

static void appends(const char *url, const char *query, const char *want)
{
	char out[256];
	int rc = url_with_query(url, query, out, sizeof(out));
	CHECK(rc == 0 && !strcmp(out, want), "url_with_query(%s, %s) = %s, want %s",
	      url, query, rc ? "(error)" : out, want);
}

static void test_urls(void)
{
	parses("http://192.168.1.50:8082/api/ui.php", "192.168.1.50", 8082, "/api/ui.php");
	parses("http://h/api/ui.php?w=758&h=1024", "h", 80, "/api/ui.php?w=758&h=1024");
	parses("http://h:8082", "h", 8082, "/");
	parses("http://h?x=1", "h", 80, "/?x=1");
	parses("http://h/a:b/c", "h", 80, "/a:b/c");     // a colon in the path is not a port
	parses("http://h/p#frag", "h", 80, "/p");        // the fragment never reaches the server
	rejects("https://h/x");                          // no TLS, and it says so
	rejects("ftp://h/x");
	rejects("http://:80/x");
	rejects("http://h:99999/x");
	rejects("h/x");

	const char *screen = "http://h:8082/api/ui.php?w=758";
	resolves(screen, "/api/tasks/complete.php", "http://h:8082/api/tasks/complete.php");
	resolves(screen, "ping.php", "http://h:8082/api/ping.php");
	resolves(screen, "http://other/x", "http://other/x");
	resolves("http://h:8082", "ping", "http://h:8082/ping");
	resolves("http://h/dash", "/x", "http://h/x");

	appends("http://h/ui", "w=1", "http://h/ui?w=1");
	appends("http://h/ui?screen=2", "w=1", "http://h/ui?screen=2&w=1");
	appends("http://h/ui#top", "w=1", "http://h/ui?w=1");

	char tiny[16];
	CHECK(url_resolve("http://h/ui", "a-much-longer-path", tiny, sizeof(tiny)) < 0,
	      "url_resolve should refuse a result that does not fit");
}

// ---- envelope ------------------------------------------------------------

static uint8_t arena_buf[64 * 1024];

static UiEnvelope envelope_of(const char *json)
{
	UiArena a;
	ui_arena_init(&a, arena_buf, sizeof(arena_buf));
	UiEnvelope env;
	char err[160];
	UiNode *root = ui_from_json(&a, json, NULL, &env, err, sizeof(err));
	CHECK(root != NULL, "ui_from_json(%s): %s", json, err);
	return env;
}

static void test_envelope(void)
{
	UiEnvelope e = envelope_of(
		"{\"schema\":1,\"refresh\":{\"seconds\":60,\"ping\":\"ping.php\","
		"\"pingSeconds\":7,\"flashSeconds\":0},"
		"\"root\":{\"kind\":\"box\"}}");
	CHECK(e.refresh_seconds == 60, "refresh %d", e.refresh_seconds);
	CHECK(e.ping_seconds == 7, "ping %d", e.ping_seconds);
	CHECK(e.flash_seconds == 0, "flash %d", e.flash_seconds);
	CHECK(!strcmp(e.ping, "ping.php"), "ping url %s", e.ping);

	// Nothing said: every field unset, so the client's defaults apply.
	e = envelope_of("{\"schema\":1,\"root\":{\"kind\":\"box\"}}");
	CHECK(e.refresh_seconds == UI_ENV_UNSET && e.ping_seconds == UI_ENV_UNSET
	      && e.flash_seconds == UI_ENV_UNSET
	      && e.ping[0] == '\0', "bare envelope should leave everything unset");

	// A bare node (a --dump-rects file) has no envelope at all.
	e = envelope_of("{\"kind\":\"box\"}");
	CHECK(e.refresh_seconds == UI_ENV_UNSET, "bare node should leave refresh unset");

	// Nonsense is ignored rather than trusted.
	e = envelope_of("{\"schema\":1,\"refresh\":{\"seconds\":-5,\"pingSeconds\":\"x\"},"
			"\"root\":{\"kind\":\"box\"}}");
	CHECK(e.refresh_seconds == UI_ENV_UNSET, "negative refresh should be unset");
	CHECK(e.ping_seconds == UI_ENV_UNSET, "string ping should be unset");
}

// ---- policy --------------------------------------------------------------

static UiEnvelope unset_env(void)
{
	return (UiEnvelope){
		.refresh_seconds = UI_ENV_UNSET, .ping_seconds = UI_ENV_UNSET,
		.flash_seconds = UI_ENV_UNSET,
	};
}

static void test_policy(void)
{
	const char *screen = "http://h:8082/api/ui.php";
	struct Policy p;

	policy_from_envelope(&p, NULL, screen);
	CHECK(p.refresh_seconds == DEFAULT_REFRESH_SECONDS && p.ping_seconds == 0
	      && p.flash_seconds == DEFAULT_FLASH_SECONDS && p.ping_url[0] == '\0',
	      "NULL envelope should give the defaults");

	UiEnvelope e = unset_env();
	policy_from_envelope(&p, &e, screen);
	CHECK(p.refresh_seconds == DEFAULT_REFRESH_SECONDS && p.ping_seconds == 0,
	      "an envelope that says nothing should give the defaults");

	// Floors: a server cannot make the device hammer it, or the panel.
	e = unset_env();
	e.refresh_seconds = 1; e.flash_seconds = 1;
	strcpy(e.ping, "ping.php"); e.ping_seconds = 1;
	policy_from_envelope(&p, &e, screen);
	CHECK(p.refresh_seconds == MIN_REFRESH_SECONDS, "refresh floor: %d", p.refresh_seconds);
	CHECK(p.flash_seconds == MIN_FLASH_SECONDS, "flash floor: %d", p.flash_seconds);
	CHECK(p.ping_seconds == MIN_PING_SECONDS, "ping floor: %d", p.ping_seconds);
	CHECK(!strcmp(p.ping_url, "http://h:8082/api/ping.php"), "ping resolved to %s", p.ping_url);

	// 0 means never — for the two intervals where never makes sense.
	e = unset_env();
	e.flash_seconds = 0; strcpy(e.ping, "/up"); e.ping_seconds = 0;
	policy_from_envelope(&p, &e, screen);
	CHECK(p.flash_seconds == 0, "flash 0 should mean never, got %d", p.flash_seconds);
	CHECK(p.ping_seconds == 0, "ping 0 should mean never, got %d", p.ping_seconds);

	// A ping URL alone gets the default interval.
	e = unset_env();
	strcpy(e.ping, "/up");
	policy_from_envelope(&p, &e, screen);
	CHECK(p.ping_seconds == DEFAULT_PING_SECONDS && !strcmp(p.ping_url, "http://h:8082/up"),
	      "ping URL alone: %ds %s", p.ping_seconds, p.ping_url);

	// An unusable ping URL drops the ping, not the rest of the policy.
	e = unset_env();
	e.refresh_seconds = 120;
	strcpy(e.ping, "https://h/up"); e.ping_seconds = 30;
	policy_from_envelope(&p, &e, screen);
	CHECK(p.ping_seconds == 0 && p.ping_url[0] == '\0', "https ping should be dropped");
	CHECK(p.refresh_seconds == 120, "refresh should survive a bad ping: %d", p.refresh_seconds);

}

int main(void)
{
	// url_parse and the policy log what they reject, which is right on the
	// device and noise here.
	freopen("/dev/null", "w", stderr);

	test_urls();
	test_envelope();
	test_policy();

	if (failures) {
		printf("%d of %d checks failed\n", failures, checks);
		return 1;
	}
	printf("all %d unit checks passed\n", checks);
	return 0;
}
