// Host-side painter: rasterises a laid-out tree into an 8-bit grayscale bitmap
// and writes it out as a PNG. Used by the preview binary; never built for the
// device.

#ifndef HOMESRV_PAINT_STB_H
#define HOMESRV_PAINT_STB_H

#include "ui_paint.h"
#include "ui_text.h"

typedef struct {
	uint8_t         *pix;    // w*h, 8-bit gray
	int              w, h;
	const UiTextCtx *tc;
} StbCanvas;

int  stb_canvas_init(StbCanvas *cv, int w, int h, const UiTextCtx *tc);
void stb_canvas_free(StbCanvas *cv);
void stb_canvas_clear(StbCanvas *cv, uint8_t gray);
int  stb_canvas_write_png(const StbCanvas *cv, const char *path);

UiPainter stb_painter(StbCanvas *cv);

#endif
