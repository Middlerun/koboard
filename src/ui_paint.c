#include "ui_paint.h"

void ui_paint(const UiPainter *p, const UiNode *n)
{
	if (!n || n->dropped || n->rect.w <= 0 || n->rect.h <= 0) return;

	if (UI_HAS_COLOR(n->bg) && p->fill_rect)
		p->fill_rect(p->ctx, n->rect, UI_GRAY_OF(n->bg));

	if (n->border > 0 && p->stroke_rect)
		p->stroke_rect(p->ctx, n->rect, n->border,
			       UI_HAS_COLOR(n->border_color)
				       ? UI_GRAY_OF(n->border_color)
				       : n->resolved_fg);

	if (n->kind == UI_TEXT && p->draw_text) {
		const char *text = n->draw_text ? n->draw_text : n->text;
		if (text && *text) {
			UiTextRun run = {
				.size_pt     = n->size_pt,
				.style       = n->style,
				.align       = n->align,
				.gray        = n->resolved_fg,
				.bg_gray     = n->resolved_bg,
				.max_lines   = n->lines,
				.no_ellipsis = n->no_ellipsis,
			};
			p->draw_text(p->ctx, n->content, text, &run);
		}
	}

	for (const UiNode *c = n->first; c; c = c->next)
		ui_paint(p, c);
}
