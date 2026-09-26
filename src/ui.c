#include "ui.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// ---- arena ---------------------------------------------------------------

// Handed out when the arena is exhausted, so callers never have to check for
// NULL. The overflow is reported once, via UiArena.oom.
static uint8_t ui_oom_scratch[512];

void ui_arena_init(UiArena *a, void *buf, size_t cap)
{
	a->buf = buf;
	a->cap = cap;
	a->used = 0;
	a->high_water = 0;
	a->oom = 0;
}

void ui_arena_reset(UiArena *a)
{
	if (a->used > a->high_water) a->high_water = a->used;
	a->used = 0;
	a->oom = 0;
}

void *ui_alloc(UiArena *a, size_t size)
{
	size = (size + 7u) & ~(size_t) 7u;   // keep 8-byte alignment
	if (a->used + size > a->cap) {
		a->oom = 1;
		if (size > sizeof(ui_oom_scratch)) size = sizeof(ui_oom_scratch);
		memset(ui_oom_scratch, 0, size);
		return ui_oom_scratch;
	}
	void *p = a->buf + a->used;
	a->used += size;
	memset(p, 0, size);
	return p;
}

char *ui_str(UiArena *a, const char *s)
{
	if (!s) return NULL;
	size_t n = strlen(s) + 1;
	char *p = ui_alloc(a, n);
	memcpy(p, s, n);
	return p;
}

char *ui_strf(UiArena *a, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0) return ui_str(a, "");
	return ui_str(a, tmp);
}

// ---- building ------------------------------------------------------------

UiNode *ui_new(UiArena *a, const UiNode *proto)
{
	UiNode *n = ui_alloc(a, sizeof(*n));
	*n = *proto;
	n->first = n->last = n->next = n->parent = NULL;
	// Copy every borrowed string, so building a tree from stack buffers is safe.
	n->text          = ui_str(a, proto->text);
	n->key           = ui_str(a, proto->key);
	n->overflow_text = ui_str(a, proto->overflow_text);
	n->draw_text     = NULL;
	return n;
}

UiNode *ui_add(UiNode *parent, UiNode *child)
{
	if (!parent || !child) return parent;
	child->parent = parent;
	child->next = NULL;
	if (parent->last) parent->last->next = child;
	else              parent->first = child;
	parent->last = child;
	return parent;
}

UiNode *ui_addv(UiNode *parent, ...)
{
	va_list ap;
	va_start(ap, parent);
	for (;;) {
		UiNode *child = va_arg(ap, UiNode *);
		if (!child) break;
		ui_add(parent, child);
	}
	va_end(ap);
	return parent;
}

UiNode *ui_textf(UiArena *a, const UiNode *proto, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);

	UiNode copy = *proto;
	copy.text = tmp;
	return ui_new(a, &copy);
}

const UiAction *ui_action(UiArena *a, const UiAction *proto)
{
	UiAction *act = ui_alloc(a, sizeof(*act));
	*act = *proto;
	act->path   = ui_str(a, proto->path);
	act->body   = ui_str(a, proto->body);
	act->target = ui_str(a, proto->target);
	act->toast  = ui_str(a, proto->toast);
	return act;
}

// ---- queries -------------------------------------------------------------

const UiNode *ui_hit_test(const UiNode *n, int x, int y)
{
	if (!n || n->dropped) return NULL;
	if (!ui_rect_contains(n->rect, x, y)) return NULL;

	// Depth first, last child first: later siblings paint on top.
	const UiNode *best = NULL;
	for (const UiNode *c = n->first; c; c = c->next) {
		const UiNode *hit = ui_hit_test(c, x, y);
		if (hit) best = hit;
	}
	if (best) return best;
	return (n->action && n->action->kind != UI_ACT_NONE) ? n : NULL;
}

UiNode *ui_find(UiNode *n, const char *key)
{
	if (!n || !key) return NULL;
	if (n->key && strcmp(n->key, key) == 0) return n;
	for (UiNode *c = n->first; c; c = c->next) {
		UiNode *found = ui_find(c, key);
		if (found) return found;
	}
	return NULL;
}

// ---- JSON output ---------------------------------------------------------
//
// Mirrors the wire format documented in docs/ui-schema.md, plus the computed
// `rect`, so a dump doubles as a golden-test fixture.

typedef struct {
	char  *buf;
	size_t len, cap;
	int    err;
} UiBuf;

static void ub_raw(UiBuf *b, const char *s, size_t n)
{
	if (b->err) return;
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 4096;
		while (cap < b->len + n + 1) cap *= 2;
		char *p = realloc(b->buf, cap);
		if (!p) { b->err = 1; return; }
		b->buf = p;
		b->cap = cap;
	}
	memcpy(b->buf + b->len, s, n);
	b->len += n;
	b->buf[b->len] = '\0';
}

static void ub_str(UiBuf *b, const char *s) { ub_raw(b, s, strlen(s)); }

static void ub_fmt(UiBuf *b, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n > 0) ub_raw(b, tmp, (size_t) n < sizeof(tmp) ? (size_t) n : sizeof(tmp) - 1);
}

static void ub_json_str(UiBuf *b, const char *s)
{
	ub_str(b, "\"");
	for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
		switch (*p) {
		case '"':  ub_str(b, "\\\""); break;
		case '\\': ub_str(b, "\\\\"); break;
		case '\n': ub_str(b, "\\n");  break;
		case '\r': ub_str(b, "\\r");  break;
		case '\t': ub_str(b, "\\t");  break;
		default:
			if (*p < 0x20) ub_fmt(b, "\\u%04x", *p);
			else           ub_raw(b, (const char *) p, 1);
		}
	}
	ub_str(b, "\"");
}

static const char *ui_len_str(UiLen l, char *tmp, size_t n)
{
	switch (l.kind) {
	case UI_LEN_PX:   snprintf(tmp, n, "%d", l.val);        break;
	case UI_LEN_PCT:  snprintf(tmp, n, "%d%%", l.val);      break;
	case UI_LEN_FLEX: snprintf(tmp, n, "flex:%d", l.val);   break;
	default:          snprintf(tmp, n, "auto");             break;
	}
	return tmp;
}

static void ui_dump(UiBuf *b, const UiNode *n, int depth)
{
	char pad[64];
	int ind = depth * 2 < 60 ? depth * 2 : 60;
	memset(pad, ' ', (size_t) ind);
	pad[ind] = '\0';

	char tmp[32];
	ub_fmt(b, "%s{ \"kind\": \"%s\"", pad, n->kind == UI_BOX ? "box" : "text");
	if (n->key) { ub_str(b, ", \"key\": "); ub_json_str(b, n->key); }
	ub_fmt(b, ", \"rect\": [%d, %d, %d, %d]",
	       n->rect.x, n->rect.y, n->rect.w, n->rect.h);

	if (n->kind == UI_TEXT) {
		ub_str(b, ", \"text\": ");
		ub_json_str(b, n->draw_text ? n->draw_text : (n->text ? n->text : ""));
		ub_fmt(b, ", \"size\": %u, \"lines\": %u", n->size_pt, n->lines);
		if (n->style)       ub_fmt(b, ", \"style\": %u", n->style);
		if (n->align)       ub_fmt(b, ", \"align\": %u", n->align);
		if (n->max_lines)   ub_fmt(b, ", \"maxLines\": %u", n->max_lines);
		if (n->truncated)   ub_str(b, ", \"truncated\": true");
	} else {
		if (n->dir)   ub_str(b, ", \"dir\": \"row\"");
		if (n->gap)   ub_fmt(b, ", \"gap\": %d", n->gap);
		if (n->main)  ub_fmt(b, ", \"main\": %u", n->main);
		if (n->cross) ub_fmt(b, ", \"cross\": %u", n->cross);
		if (n->clip)  ub_str(b, ", \"clip\": true");
	}

	if (n->w.kind) ub_fmt(b, ", \"w\": \"%s\"", ui_len_str(n->w, tmp, sizeof(tmp)));
	if (n->h.kind) ub_fmt(b, ", \"h\": \"%s\"", ui_len_str(n->h, tmp, sizeof(tmp)));
	if (n->pad[0] || n->pad[1] || n->pad[2] || n->pad[3])
		ub_fmt(b, ", \"pad\": [%d, %d, %d, %d]",
		       n->pad[0], n->pad[1], n->pad[2], n->pad[3]);
	if (n->border) ub_fmt(b, ", \"border\": %d", n->border);
	if (UI_HAS_COLOR(n->bg)) ub_fmt(b, ", \"bg\": %u", UI_GRAY_OF(n->bg));
	if (UI_HAS_COLOR(n->fg)) ub_fmt(b, ", \"fg\": %u", UI_GRAY_OF(n->fg));
	if (n->dropped) ub_str(b, ", \"dropped\": true");

	if (n->action && n->action->kind != UI_ACT_NONE) {
		ub_fmt(b, ", \"action\": { \"kind\": %u", n->action->kind);
		if (n->action->path) { ub_str(b, ", \"path\": "); ub_json_str(b, n->action->path); }
		if (n->action->body) { ub_str(b, ", \"body\": "); ub_json_str(b, n->action->body); }
		ub_str(b, " }");
	}

	if (n->first) {
		ub_str(b, ", \"kids\": [\n");
		for (const UiNode *c = n->first; c; c = c->next) {
			ui_dump(b, c, depth + 1);
			if (c->next) ub_str(b, ",");
			ub_str(b, "\n");
		}
		ub_fmt(b, "%s]", pad);
	}
	ub_str(b, " }");
}

int ui_write_json(const UiNode *root, char **out, size_t *out_len)
{
	UiBuf b = { 0 };
	if (!root) return -1;
	ui_dump(&b, root, 0);
	ub_str(&b, "\n");
	if (b.err) { free(b.buf); return -1; }
	*out = b.buf;
	if (out_len) *out_len = b.len;
	return 0;
}
