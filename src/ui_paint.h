// Painter interface.
//
// The paint walk is shared; only these three primitives differ between the
// device (FBInk) and the host preview (stb_truetype into a bitmap). That is
// what lets `make preview` exercise the real layout code.

#ifndef KOBOARD_UI_PAINT_H
#define KOBOARD_UI_PAINT_H

#include "ui.h"

typedef struct {
	int         size_pt;
	uint8_t     style;    // UiFontStyle
	uint8_t     align;    // UiAlign
	uint8_t     gray;     // resolved foreground
	uint8_t     bg_gray;  // background this text is drawn onto
	int         max_lines;
	bool        no_ellipsis;
} UiTextRun;

typedef struct UiPainter {
	void *ctx;
	void (*fill_rect)(void *ctx, UiRect r, uint8_t gray);
	void (*stroke_rect)(void *ctx, UiRect r, int width, uint8_t gray);
	void (*draw_text)(void *ctx, UiRect r, const char *text, const UiTextRun *run);
} UiPainter;

// Paint a subtree. Nodes clipped away by an ancestor are skipped.
void ui_paint(const UiPainter *p, const UiNode *node);

#endif
