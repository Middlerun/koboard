// Device painter: draws a laid-out tree with FBInk.
//
// This is the only file that knows FBInk's margin-based text API. Every node
// arrives as an absolute rect; converting that into top/left/right margins
// happens once, here, instead of at every call site.

#ifndef HOMESRV_PAINT_FBINK_H
#define HOMESRV_PAINT_FBINK_H

#include "ui_paint.h"
#include "ui_text.h"

#include "fbink.h"

typedef struct {
	int              fbfd;
	int              screen_w, screen_h;
	const UiTextCtx *tc;          // only for line-height cross-checks
	const FBInkConfig *cfg;       // drawing config (no_refresh while compositing)
	int              mismatches;  // times FBInk disagreed with our measurement
} FbPainter;

UiPainter fbink_painter(FbPainter *fp);

#endif
