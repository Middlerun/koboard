// Declarative UI tree for the HomeSrv Kobo client.
//
// A frame is described as a tree of nodes; the layout engine turns it into an
// absolute rect per node; a painter draws it. Nothing in this header knows
// about FBInk, so the whole pipeline also builds for the host (see preview.c).
//
// The schema is a deliberate subset of CSS flexbox: single-axis boxes with
// padding, gaps, a border, grow factors, and justify/align. It maps 1:1 onto
// JSON (see docs/ui-schema.md) so the tree can later be produced by the
// server instead of by C.

#ifndef HOMESRV_UI_H
#define HOMESRV_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- arena ----------------------------------------------------------------
//
// Trees are rebuilt from scratch every frame, so all node memory comes from a
// bump allocator that is reset per frame. Allocation never returns NULL: on
// exhaustion it hands back a scratch block and sets `oom`, so tree building
// stays branch-free and the failure surfaces once, at the end.

typedef struct {
	uint8_t *buf;
	size_t   cap;
	size_t   used;
	size_t   high_water;
	int      oom;
} UiArena;

void  ui_arena_init(UiArena *a, void *buf, size_t cap);
void  ui_arena_reset(UiArena *a);
void *ui_alloc(UiArena *a, size_t size);
char *ui_str(UiArena *a, const char *s);
char *ui_strf(UiArena *a, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

// ---- geometry -------------------------------------------------------------

typedef struct {
	int x, y, w, h;
} UiRect;

static inline bool ui_rect_contains(UiRect r, int x, int y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// ---- lengths --------------------------------------------------------------
//
// UI_LEN_AUTO must be 0 so that a zero-initialised node defaults to intrinsic
// sizing on both axes.

typedef enum {
	UI_LEN_AUTO = 0,   // intrinsic: measured text, or children's extent
	UI_LEN_PX,         // absolute pixels
	UI_LEN_PCT,        // percent of the parent's content box on this axis
	UI_LEN_FLEX,       // share of the parent's leftover main-axis space
} UiLenKind;

typedef struct {
	uint8_t kind;
	int16_t val;
} UiLen;

#define AUTO     ((UiLen){ UI_LEN_AUTO, 0 })
#define PX(n)    ((UiLen){ UI_LEN_PX,   (int16_t) (n) })
#define PCT(n)   ((UiLen){ UI_LEN_PCT,  (int16_t) (n) })
#define FLEX(n)  ((UiLen){ UI_LEN_FLEX, (int16_t) (n) })

// ---- colour -------------------------------------------------------------
//
// 8-bit grayscale, stored biased by one so that 0 means "unset". An unset
// foreground inherits from the nearest ancestor that sets one (default black);
// an unset background is transparent.

typedef uint16_t UiColor;

#define UI_COLOR_NONE  ((UiColor) 0)
#define GRAY(g)        ((UiColor) ((g) + 1))
#define UI_GRAY_OF(c)  ((uint8_t) ((c) - 1))
#define UI_HAS_COLOR(c) ((c) != UI_COLOR_NONE)

#define UI_BLACK  GRAY(0)
#define UI_DARK   GRAY(64)
#define UI_MED    GRAY(128)
#define UI_LIGHT  GRAY(192)
#define UI_WHITE  GRAY(255)

// ---- enums --------------------------------------------------------------

typedef enum { UI_BOX = 0, UI_TEXT } UiKind;

typedef enum { UI_COL = 0, UI_ROW } UiDir;

// Main-axis distribution of children.
typedef enum {
	UI_MAIN_START = 0,
	UI_MAIN_CENTER,
	UI_MAIN_END,
	UI_MAIN_BETWEEN,
} UiMain;

// Cross-axis placement of children.
typedef enum {
	UI_CROSS_STRETCH = 0,   // fill the cross axis (default, so text wraps wide)
	UI_CROSS_START,
	UI_CROSS_CENTER,
	UI_CROSS_END,
} UiCross;

// Horizontal alignment of text lines inside a text node's own rect.
typedef enum {
	UI_ALIGN_START = 0,
	UI_ALIGN_CENTER,
	UI_ALIGN_END,
} UiAlign;

typedef enum {
	UI_FNT_REGULAR = 0,
	UI_FNT_BOLD,
	UI_FNT_ITALIC,
	UI_FNT_BOLD_ITALIC,
} UiFontStyle;

// ---- actions ------------------------------------------------------------
//
// A node with an action is a tap target. Dispatch is generic (see ui_hit_test
// and ui_perform in main.c), so touch handling never re-derives geometry.

typedef enum {
	UI_ACT_NONE = 0,
	UI_ACT_POST,      // form-encoded POST to `path` with `body`
	UI_ACT_REFRESH,   // re-fetch and redraw
	UI_ACT_QUIT,      // exit the app
	UI_ACT_OPEN,      // switch to the screen named in `target`
	UI_ACT_SERVER,    // talk to the server whose index is in `target`, and refetch
} UiActionKind;

typedef struct {
	uint8_t     kind;
	const char *path;    // UI_ACT_POST: request path
	const char *body;    // UI_ACT_POST: form-encoded body
	const char *target;  // UI_ACT_OPEN: screen name. UI_ACT_SERVER: server index
	const char *toast;   // optional: text to show in place of the node while the
			     // request is in flight (optimistic feedback)
} UiAction;

// ---- node ---------------------------------------------------------------

typedef struct UiNode UiNode;

struct UiNode {
	uint8_t kind;

	// Identity. Optional, but a keyed node can be found again next frame
	// (ui_find) so it can be repainted on its own — that is how the clock
	// and the status line refresh without redrawing the screen.
	const char *key;

	// Box model. `pad` is top, right, bottom, left.
	UiLen   w, h;
	int16_t pad[4];
	int16_t gap;
	int16_t border;
	UiColor border_color;
	UiColor bg;
	UiColor fg;          // inherited by descendants

	// Box only.
	uint8_t dir;         // UiDir
	uint8_t main;        // UiMain
	uint8_t cross;       // UiCross
	bool    clip;        // drop children that do not fit instead of overflowing
	const char *overflow_text;  // shown when clip dropped children; "{n}" -> count

	// Text only.
	const char *text;
	uint16_t    size_pt;
	uint8_t     style;      // UiFontStyle
	uint8_t     align;      // UiAlign
	uint8_t     max_lines;  // 0 = unlimited
	bool        no_ellipsis;

	const UiAction *action;

	// Tree links.
	UiNode *first, *last, *next, *parent;

	// ---- filled in by ui_layout; do not set these yourself ----
	UiRect      rect;        // absolute, including padding and border
	UiRect      content;     // absolute, inside padding and border
	const char *draw_text;   // `text`, or an ellipsized copy of it
	int16_t     line_h;      // measured row height
	uint8_t     lines;       // measured line count
	uint8_t     resolved_fg; // inherited foreground, as a plain gray
	uint8_t     resolved_bg; // nearest ancestor's background, as a plain gray
	bool        truncated;
	bool        dropped;     // clipped away by an ancestor's `clip`
};

// ---- building -----------------------------------------------------------
//
//   UiNode *row = CHILDREN(BOX(a, .dir = UI_ROW, .gap = 8),
//           TEXT(a, "Mon", .size_pt = 10, .w = FLEX(1)),
//           TEXTF(a, (.size_pt = 10), "%d\u00B0", temp));
//
// ui_new copies `text`, `key` and `overflow_text` into the arena, so passing a
// stack buffer is safe.

UiNode *ui_new(UiArena *a, const UiNode *proto);
UiNode *ui_add(UiNode *parent, UiNode *child);
UiNode *ui_addv(UiNode *parent, ...);   // NULL-terminated
UiNode *ui_textf(UiArena *a, const UiNode *proto, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

const UiAction *ui_action(UiArena *a, const UiAction *proto);

#define BOX(a, ...)      ui_new((a), &(UiNode){ .kind = UI_BOX, __VA_ARGS__ })
#define TEXT(a, s, ...)  ui_new((a), &(UiNode){ .kind = UI_TEXT, .text = (s), __VA_ARGS__ })
#define CHILDREN(p, ...)     ui_addv((p), __VA_ARGS__, (UiNode *) NULL)
#define ACTION(a, ...)   ui_action((a), &(UiAction){ __VA_ARGS__ })

// TEXTF(arena, (.props...), "fmt", args...) — note the parenthesised property
// list, which keeps the format string unambiguously separate. Use () for none.
#define UI_UNPAREN(...)  __VA_ARGS__
#define TEXTF(a, props, ...) \
	ui_textf((a), &(UiNode){ .kind = UI_TEXT, UI_UNPAREN props }, __VA_ARGS__)

#define PAD_ALL(n)     { (n), (n), (n), (n) }
#define PAD_XY(v, h)   { (v), (h), (v), (h) }

// ---- queries ------------------------------------------------------------

// Deepest laid-out node containing (x, y) that has an action, or NULL.
const UiNode *ui_hit_test(const UiNode *root, int x, int y);

// First node with the given key, or NULL.
UiNode *ui_find(UiNode *root, const char *key);

// Serialise the laid-out tree. Used by `make test` for golden comparisons and
// by --dump-rects for eyeballing a layout without looking at pixels. The
// format is the same one a server-driven tree would use.
int ui_write_json(const UiNode *root, char **out, size_t *out_len);

#endif
