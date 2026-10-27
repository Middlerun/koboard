#include "ui_json.h"

#include "cJSON.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

// A malformed tree from the server must not take the client down with it, so
// both recursion and size are bounded. The dashboard is ~5 levels deep and a
// couple of hundred nodes; these leave room to spare without leaving room for
// a runaway document. The node ceiling is set against UI_ARENA_BYTES: a tree
// this big (160 bytes a node, plus strings) still has to fit in one frame.
#define UI_JSON_MAX_DEPTH 24
#define UI_JSON_MAX_NODES 1500

struct ParseCtx {
	UiArena       *arena;
	const UiVars  *vars;
	int            nodes;
	char          *err;
	size_t         errsz;
};

static void fail(struct ParseCtx *ctx, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void fail(struct ParseCtx *ctx, const char *fmt, ...)
{
	if (!ctx->err || ctx->err[0]) return;   // keep the first, deepest reason
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(ctx->err, ctx->errsz, fmt, ap);
	va_end(ap);
}

// ---- scalars -------------------------------------------------------------

static int get_int(const cJSON *obj, const char *name, int fallback)
{
	const cJSON *it = cJSON_GetObjectItem(obj, name);
	if (cJSON_IsNumber(it)) return (int) it->valuedouble;
	if (cJSON_IsBool(it))   return cJSON_IsTrue(it) ? 1 : 0;
	return fallback;
}

static bool get_bool(const cJSON *obj, const char *name)
{
	const cJSON *it = cJSON_GetObjectItem(obj, name);
	if (cJSON_IsBool(it))   return cJSON_IsTrue(it);
	if (cJSON_IsNumber(it)) return it->valuedouble != 0;
	return false;
}

static const char *get_str(const cJSON *obj, const char *name)
{
	return cJSON_GetStringValue(cJSON_GetObjectItem(obj, name));
}

// Grays arrive as 0-255 and are stored biased by one (see UI_COLOR_NONE), so
// an absent property stays "unset" rather than becoming black.
static UiColor get_color(const cJSON *obj, const char *name)
{
	const cJSON *it = cJSON_GetObjectItem(obj, name);
	if (!cJSON_IsNumber(it)) return UI_COLOR_NONE;
	int g = (int) it->valuedouble;
	if (g < 0)   g = 0;
	if (g > 255) g = 255;
	return GRAY(g);
}

// "auto" | "200" | "37%" | "flex:2", or a bare number meaning pixels.
static UiLen get_len(const cJSON *obj, const char *name)
{
	const cJSON *it = cJSON_GetObjectItem(obj, name);
	if (cJSON_IsNumber(it)) return PX((int) it->valuedouble);

	const char *s = cJSON_GetStringValue(it);
	if (!s || !*s || strcmp(s, "auto") == 0) return AUTO;

	if (strncmp(s, "flex:", 5) == 0) return FLEX(atoi(s + 5));

	char *end = NULL;
	long v = strtol(s, &end, 10);
	if (end == s) return AUTO;
	if (*end == '%') return PCT((int) v);
	return PX((int) v);
}

// [top, right, bottom, left], or one number for all four sides.
static void get_pad(const cJSON *obj, int16_t pad[4])
{
	const cJSON *it = cJSON_GetObjectItem(obj, "pad");
	if (cJSON_IsNumber(it)) {
		for (int i = 0; i < 4; i++) pad[i] = (int16_t) it->valuedouble;
		return;
	}
	if (!cJSON_IsArray(it)) return;
	int i = 0;
	const cJSON *v;
	cJSON_ArrayForEach(v, it) {
		if (i >= 4) break;
		if (cJSON_IsNumber(v)) pad[i] = (int16_t) v->valuedouble;
		i++;
	}
}

static uint8_t get_enum(const cJSON *obj, const char *name, uint8_t max)
{
	int v = get_int(obj, name, 0);
	if (v < 0 || v > (int) max) return 0;
	return (uint8_t) v;
}

static const UiAction *parse_action(struct ParseCtx *ctx, const cJSON *obj)
{
	const cJSON *it = cJSON_GetObjectItem(obj, "action");
	if (!cJSON_IsObject(it)) return NULL;

	// An allow-list, not a range: the two actions that are the device's own
	// business stay out of the server's reach. Quitting belongs to the menu,
	// and which servers exist is not something a server should get a say in.
	// Neither loses the server anything — it can still send the user to the
	// menu with UI_ACT_OPEN, and the menu offers both.
	int kind = get_int(it, "kind", UI_ACT_NONE);
	switch (kind) {
	case UI_ACT_POST:
	case UI_ACT_REFRESH:
	case UI_ACT_OPEN:
		break;
	default:   // including UI_ACT_QUIT and UI_ACT_SERVER
		return NULL;
	}

	return ui_action(ctx->arena, &(UiAction){
		.kind   = (uint8_t) kind,
		.path   = get_str(it, "path"),
		.body   = get_str(it, "body"),
		.target = get_str(it, "target"),
		.toast  = get_str(it, "toast"),
	});
}

// ---- placeholders --------------------------------------------------------

// Render one "{name}" or "{name:format}" into `out`. Returns 0 if the name was
// recognised, -1 if it was not — in which case the caller copies the
// placeholder through verbatim rather than swallowing it.
static int expand_one(const struct ParseCtx *ctx, const char *name, size_t namelen,
		      const char *fmt, char *out, size_t outsz)
{
	if (namelen == 5 && strncmp(name, "clock", 5) == 0) {
		struct tm tm_buf;
		struct tm *tm = localtime_r(&ctx->vars->now, &tm_buf);
		if (!tm) return -1;
		// strftime writes nothing and returns 0 both when the result is
		// empty and when it does not fit; an empty clock is better than a
		// buffer's worth of leftovers either way.
		if (strftime(out, outsz, fmt && *fmt ? fmt : "%H:%M", tm) == 0)
			out[0] = '\0';
		return 0;
	}
	return -1;
}

// Substitute every known placeholder in `s`. Returns `s` itself when there was
// nothing to do, so the common case allocates nothing.
static const char *expand_vars(struct ParseCtx *ctx, const char *s)
{
	if (!s || !ctx->vars || !strchr(s, '{')) return s;

	char out[512];
	size_t n = 0;
	for (const char *p = s; *p; ) {
		if (*p != '{') {
			if (n + 1 < sizeof(out)) out[n++] = *p;
			p++;
			continue;
		}

		const char *close = strchr(p, '}');
		if (!close) {   // unbalanced: the rest is just text
			while (*p && n + 1 < sizeof(out)) out[n++] = *p++;
			break;
		}

		const char *colon = memchr(p + 1, ':', (size_t) (close - p - 1));
		size_t namelen = (size_t) ((colon ? colon : close) - (p + 1));

		char fmt[64] = { 0 };
		if (colon) {
			size_t fmtlen = (size_t) (close - colon - 1);
			if (fmtlen >= sizeof(fmt)) fmtlen = sizeof(fmt) - 1;
			memcpy(fmt, colon + 1, fmtlen);
		}

		char value[128];
		if (expand_one(ctx, p + 1, namelen, fmt, value, sizeof(value)) == 0) {
			for (const char *v = value; *v && n + 1 < sizeof(out); v++)
				out[n++] = *v;
		} else {
			// Not ours — hand it back untouched.
			for (const char *q = p; q <= close && n + 1 < sizeof(out); q++)
				out[n++] = *q;
		}
		p = close + 1;
	}
	out[n] = '\0';

	return strcmp(out, s) == 0 ? s : ui_str(ctx->arena, out);
}

// ---- nodes ---------------------------------------------------------------

static UiNode *parse_node(struct ParseCtx *ctx, const cJSON *obj, int depth)
{
	if (!cJSON_IsObject(obj)) {
		fail(ctx, "node is not an object");
		return NULL;
	}
	if (depth > UI_JSON_MAX_DEPTH) {
		fail(ctx, "tree deeper than %d levels", UI_JSON_MAX_DEPTH);
		return NULL;
	}
	if (++ctx->nodes > UI_JSON_MAX_NODES) {
		fail(ctx, "more than %d nodes", UI_JSON_MAX_NODES);
		return NULL;
	}

	const char *kind = get_str(obj, "kind");
	bool is_text = kind && strcmp(kind, "text") == 0;
	if (kind && !is_text && strcmp(kind, "box") != 0) {
		fail(ctx, "unknown node kind '%s'", kind);
		return NULL;
	}

	UiNode proto = {
		.kind = is_text ? UI_TEXT : UI_BOX,
		.key  = get_str(obj, "key"),
		.w    = get_len(obj, "w"),
		.h    = get_len(obj, "h"),
		.border       = (int16_t) get_int(obj, "border", 0),
		.border_color = get_color(obj, "borderColor"),
		.bg           = get_color(obj, "bg"),
		.fg           = get_color(obj, "fg"),
	};
	get_pad(obj, proto.pad);

	if (is_text) {
		proto.text        = expand_vars(ctx, get_str(obj, "text"));
		proto.size_pt     = (uint16_t) get_int(obj, "size", 0);
		proto.style       = get_enum(obj, "style", UI_FNT_BOLD_ITALIC);
		proto.align       = get_enum(obj, "align", UI_ALIGN_END);
		proto.max_lines   = (uint8_t) get_int(obj, "maxLines", 0);
		proto.no_ellipsis = get_bool(obj, "noEllipsis");
	} else {
		const char *dir = get_str(obj, "dir");
		proto.dir   = dir ? (strcmp(dir, "row") == 0 ? UI_ROW : UI_COL)
				  : (uint8_t) (get_int(obj, "dir", 0) ? UI_ROW : UI_COL);
		proto.gap   = (int16_t) get_int(obj, "gap", 0);
		proto.main  = get_enum(obj, "main", UI_MAIN_BETWEEN);
		proto.cross = get_enum(obj, "cross", UI_CROSS_END);
		proto.clip  = get_bool(obj, "clip");
		proto.overflow_text = get_str(obj, "overflowText");
	}

	// ui_new copies every borrowed string into the arena, so the cJSON
	// document can be freed as soon as this returns.
	UiNode *node = ui_new(ctx->arena, &proto);
	node->action = parse_action(ctx, obj);

	const cJSON *children = cJSON_GetObjectItem(obj, "children");
	if (cJSON_IsArray(children)) {
		const cJSON *item;
		cJSON_ArrayForEach(item, children) {
			UiNode *child = parse_node(ctx, item, depth + 1);
			if (!child) return NULL;
			ui_add(node, child);
		}
	}
	return node;
}

// A whole number of seconds, or UI_ENV_UNSET if absent or not a number.
static int get_seconds(const cJSON *obj, const char *name)
{
	const cJSON *it = cJSON_GetObjectItem(obj, name);
	if (!cJSON_IsNumber(it) || it->valuedouble < 0) return UI_ENV_UNSET;
	if (it->valuedouble > 86400 * 7) return 86400 * 7;
	return (int) it->valuedouble;
}

static void parse_envelope(const cJSON *doc, UiEnvelope *env)
{
	const cJSON *refresh = cJSON_GetObjectItem(doc, "refresh");
	if (cJSON_IsObject(refresh)) {
		env->refresh_seconds = get_seconds(refresh, "seconds");
		env->ping_seconds    = get_seconds(refresh, "pingSeconds");
		env->flash_seconds   = get_seconds(refresh, "flashSeconds");
		const char *ping = get_str(refresh, "ping");
		if (ping) snprintf(env->ping, sizeof(env->ping), "%s", ping);
	}
}

UiNode *ui_from_json(UiArena *a, const char *json, const UiVars *vars,
		     UiEnvelope *env, char *err, size_t errsz)
{
	if (err && errsz) err[0] = '\0';
	if (env)
		*env = (UiEnvelope){
			.refresh_seconds = UI_ENV_UNSET,
			.ping_seconds    = UI_ENV_UNSET,
			.flash_seconds   = UI_ENV_UNSET,
		};

	cJSON *doc = cJSON_Parse(json);
	if (!doc) {
		if (err) snprintf(err, errsz, "invalid JSON");
		return NULL;
	}

	struct ParseCtx ctx = { .arena = a, .vars = vars, .err = err,
				.errsz = errsz };

	// Either an envelope or a bare node. A bare node is what a --dump-rects
	// file is, which is how the golden dumps double as parser input.
	const cJSON *root = doc;
	if (!cJSON_GetObjectItem(doc, "kind")) {
		const cJSON *schema = cJSON_GetObjectItem(doc, "schema");
		if (cJSON_IsNumber(schema) && (int) schema->valuedouble > UI_SCHEMA_VERSION) {
			if (err)
				snprintf(err, errsz, "server speaks UI schema %d, "
					 "this client understands %d",
					 (int) schema->valuedouble, UI_SCHEMA_VERSION);
			cJSON_Delete(doc);
			return NULL;
		}
		root = cJSON_GetObjectItem(doc, "root");
		if (!root) {
			if (err) snprintf(err, errsz, "no 'root' node in response");
			cJSON_Delete(doc);
			return NULL;
		}
		if (env) parse_envelope(doc, env);
	}

	UiNode *node = parse_node(&ctx, root, 0);
	cJSON_Delete(doc);
	if (!node && err && errsz && !err[0])
		snprintf(err, errsz, "unparseable tree");
	return node;
}
