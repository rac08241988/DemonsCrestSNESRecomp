/* rpanel: a tiny software 2D kit for the in-game side panel.
 * Antialiased rounded rects, circles, capsules and TrueType text (stb_truetype
 * with an embedded Inter, SIL OFL 1.1 -- see fonts/Inter-OFL.txt), drawn into
 * an ARGB8888 canvas. Drawing is premultiplied; rp_finish converts the canvas
 * to straight alpha for the host's modal layer. Shared by the Demon's Crest
 * and Super Ryu World ports. */
#ifndef RPANEL_H
#define RPANEL_H
#include <stdint.h>

typedef struct RpCanvas {
  uint32_t *px;   /* premultiplied ARGB while drawing */
  int w, h;
  int cx0, cy0, cx1, cy1;   /* clip rect (inclusive-exclusive) */
} RpCanvas;

enum { RP_REGULAR = 0, RP_BOLD = 1 };

void rp_canvas_clip(RpCanvas *c, int x0, int y0, int x1, int y1);
void rp_canvas_noclip(RpCanvas *c);
void rp_clear(RpCanvas *c);
/* Colours are straight ARGB 0xAARRGGBB. */
void rp_fill_rrect(RpCanvas *c, float x, float y, float w, float h, float r, uint32_t col);
void rp_stroke_rrect(RpCanvas *c, float x, float y, float w, float h, float r,
                     float t, uint32_t col);
void rp_fill_circle(RpCanvas *c, float cx, float cy, float r, uint32_t col);
void rp_stroke_circle(RpCanvas *c, float cx, float cy, float r, float t, uint32_t col);
void rp_line(RpCanvas *c, float x0, float y0, float x1, float y1, float t, uint32_t col);
void rp_tri(RpCanvas *c, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t col);
/* Text: `size` is the pixel height of the em; `y` is the TOP of the line box
 * (ascent + descent). max_w > 0 elides with an ellipsis to fit. Returns the
 * drawn width. */
float rp_text(RpCanvas *c, int font, float size, float x, float y, const char *s,
              uint32_t col, float max_w);
float rp_text_width(int font, float size, const char *s);
float rp_line_height(int font, float size);
/* Text centred in a box. */
void rp_text_center(RpCanvas *c, int font, float size, float x, float y, float w, float h,
                    const char *s, uint32_t col);
void rp_finish(RpCanvas *c);
uint32_t rp_mix(uint32_t a, uint32_t b, float t);   /* straight colours */
uint32_t rp_alpha(uint32_t col, float a);             /* scale alpha */
#endif
