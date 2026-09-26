// Layout engine: turns a UI tree into an absolute rect per node.
//
// One pass of constraint propagation (parent offers a content box, child
// reports its size), then one pass assigning absolute origins. No reflow
// loops, no intrinsic-sizing pre-pass.
//
// Run this exactly once per tree. Trees are rebuilt from a reset arena every
// frame, so that falls out naturally.

#ifndef HOMESRV_UI_LAYOUT_H
#define HOMESRV_UI_LAYOUT_H

#include "ui.h"
#include "ui_text.h"

void ui_layout(UiNode *root, const UiTextCtx *tc, UiArena *arena, UiRect viewport);

#endif
