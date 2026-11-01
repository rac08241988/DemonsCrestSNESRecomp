/* See rpanel.h. */
#include "rpanel.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../recomp-ui/src/third_party/stb_truetype.h"
#include "inter_font_data.h"

static stbtt_fontinfo g_font[2];
static int g_font_ok[2], g_font_tried;
static float g_ascent[2], g_descent[2], g_linegap[2]; /* in font units */

static void fonts(void) {
  if (g_font_tried) return;
  g_font_tried = 1;
  const unsigned char *d[2] = {inter_regular_ttf, inter_bold_ttf};
  for (int i = 0; i < 2; i++) {
    g_font_ok[i] = stbtt_InitFont(&g_font[i], d[i], stbtt_GetFontOffsetForIndex(d[i], 0));
    if (g_font_ok[i]) {
      int a, de, lg;
      stbtt_GetFontVMetrics(&g_font[i], &a, &de, &lg);
      g_ascent[i] = (float)a; g_descent[i] = (float)de; g_linegap[i] = (float)lg;
    }
  }
}

/* ---- glyph cache --------------------------------------------------------- */
typedef struct Glyph {
  int cp;
  unsigned char *bmp;
  int w, h, xoff, yoff;
  float adv;
} Glyph;
#define RP_CACHE_SLOTS 12
#define RP_GLYPHS 384
typedef struct Cache {
  int font, size_q; /* size in 1/4 px */
  unsigned used;
  Glyph g[RP_GLYPHS];
} Cache;
static Cache g_cache[RP_CACHE_SLOTS];
static unsigned g_tick;

static Cache *cache_for(int font, float size) {
  int q = (int)lrintf(size * 4.f);
  Cache *lru = &g_cache[0];
  for (int i = 0; i < RP_CACHE_SLOTS; i++) {
    Cache *c = &g_cache[i];
    if (c->used && c->font == font && c->size_q == q) { c->used = ++g_tick; return c; }
    if (c->used < lru->used) lru = c;
  }
  for (int i = 0; i < RP_GLYPHS; i++) {
    free(lru->g[i].bmp);
    memset(&lru->g[i], 0, sizeof(Glyph));
    lru->g[i].cp = -1;
  }
  lru->font = font;
  lru->size_q = q;
  lru->used = ++g_tick;
  return lru;
}

static Glyph *glyph(Cache *c, int cp) {
  unsigned h = ((unsigned)cp * 2654435761u) % RP_GLYPHS;
  for (int n = 0; n < RP_GLYPHS; n++) {
    Glyph *g = &c->g[(h + n) % RP_GLYPHS];
    if (g->cp == cp) return g;
    if (g->cp == -1 || g->cp == 0) {
      stbtt_fontinfo *f = &g_font[c->font];
      float sc = stbtt_ScaleForPixelHeight(f, c->size_q / 4.f);
      /* em-based: scale so ascent-descent == size */
      sc = (c->size_q / 4.f) / (g_ascent[c->font] - g_descent[c->font]);
      int adv, lsb;
      stbtt_GetCodepointHMetrics(f, cp, &adv, &lsb);
      g->cp = cp;
      g->adv = adv * sc;
      g->bmp = stbtt_GetCodepointBitmapSubpixel(f, sc, sc, 0.f, 0.f, cp, &g->w, &g->h,
                                                &g->xoff, &g->yoff);
      return g;
    }
  }
  return NULL;
}

static int utf8(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  int c = s[0];
  if (c < 0x80) { *p += 1; return c; }
  if ((c & 0xE0) == 0xC0 && s[1]) { *p += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
  if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
    *p += 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
  }
  *p += 1;
  return '?';
}

/* ---- pixels -------------------------------------------------------------- */
static inline void blend(RpCanvas *c, int x, int y, uint32_t col, float cov) {
  if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1) return;
  float a = ((col >> 24) & 255) / 255.f * cov;
  if (a <= 0.f) return;
  if (a > 1.f) a = 1.f;
  uint32_t *d = &c->px[(size_t)y * c->w + x];
  uint32_t dv = *d;
  float ia = 1.f - a;
  float sr = ((col >> 16) & 255) * a, sg = ((col >> 8) & 255) * a, sb = (col & 255) * a;
  int r = (int)(sr + ((dv >> 16) & 255) * ia + .5f);
  int g = (int)(sg + ((dv >> 8) & 255) * ia + .5f);
  int b = (int)(sb + (dv & 255) * ia + .5f);
  int A = (int)(a * 255.f + ((dv >> 24) & 255) * ia + .5f);
  if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255; if (A > 255) A = 255;
  *d = ((uint32_t)A << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void rp_canvas_clip(RpCanvas *c, int x0, int y0, int x1, int y1) {
  c->cx0 = x0 < 0 ? 0 : x0; c->cy0 = y0 < 0 ? 0 : y0;
  c->cx1 = x1 > c->w ? c->w : x1; c->cy1 = y1 > c->h ? c->h : y1;
}
void rp_canvas_noclip(RpCanvas *c) { rp_canvas_clip(c, 0, 0, c->w, c->h); }
void rp_clear(RpCanvas *c) { memset(c->px, 0, (size_t)c->w * c->h * 4); }

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

static float sd_rrect(float px, float py, float cx, float cy, float hw, float hh, float r) {
  float qx = fabsf(px - cx) - (hw - r), qy = fabsf(py - cy) - (hh - r);
  float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
  float in = qx > qy ? qx : qy;
  if (in > 0) in = 0;
  return sqrtf(ox * ox + oy * oy) + in - r;
}

static void rrect(RpCanvas *c, float x, float y, float w, float h, float r, float t,
                  uint32_t col) {
  if (w <= 0 || h <= 0) return;
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  float cx = x + w / 2, cy = y + h / 2;
  int x0 = (int)floorf(x) - 1, y0 = (int)floorf(y) - 1;
  int x1 = (int)ceilf(x + w) + 1, y1 = (int)ceilf(y + h) + 1;
  if (x0 < c->cx0) x0 = c->cx0; if (y0 < c->cy0) y0 = c->cy0;
  if (x1 > c->cx1) x1 = c->cx1; if (y1 > c->cy1) y1 = c->cy1;
  float inner_r = r + 1.5f; /* rows far from the corners are flat */
  for (int py = y0; py < y1; py++) {
    float fy = py + .5f;
    int flat_row = fy > y + inner_r && fy < y + h - inner_r;
    for (int px = x0; px < x1; px++) {
      float fx = px + .5f;
      float d;
      if (flat_row && t <= 0) {
        float dl = x - fx, dr = fx - (x + w);
        d = dl > dr ? dl : dr;
        if (d < -1.f) { blend(c, px, py, col, 1.f); continue; }
      } else {
        d = sd_rrect(fx, fy, cx, cy, w / 2, h / 2, r);
      }
      float cov;
      if (t > 0) cov = clampf(.5f - (fabsf(d + t / 2) - t / 2), 0, 1);
      else cov = clampf(.5f - d, 0, 1);
      if (cov > 0) blend(c, px, py, col, cov);
    }
  }
}
void rp_fill_rrect(RpCanvas *c, float x, float y, float w, float h, float r, uint32_t col) {
  rrect(c, x, y, w, h, r, 0, col);
}
void rp_stroke_rrect(RpCanvas *c, float x, float y, float w, float h, float r, float t,
                     uint32_t col) {
  rrect(c, x, y, w, h, r, t, col);
}
void rp_fill_circle(RpCanvas *c, float cx, float cy, float r, uint32_t col) {
  rrect(c, cx - r, cy - r, 2 * r, 2 * r, r, 0, col);
}
void rp_stroke_circle(RpCanvas *c, float cx, float cy, float r, float t, uint32_t col) {
  rrect(c, cx - r, cy - r, 2 * r, 2 * r, r, t, col);
}
void rp_line(RpCanvas *c, float x0, float y0, float x1, float y1, float t, uint32_t col) {
  float hr = t / 2;
  int bx0 = (int)floorf(fminf(x0, x1) - hr - 1), by0 = (int)floorf(fminf(y0, y1) - hr - 1);
  int bx1 = (int)ceilf(fmaxf(x0, x1) + hr + 1), by1 = (int)ceilf(fmaxf(y0, y1) + hr + 1);
  float dx = x1 - x0, dy = y1 - y0, ll = dx * dx + dy * dy;
  for (int py = by0; py < by1; py++)
    for (int px = bx0; px < bx1; px++) {
      float fx = px + .5f - x0, fy = py + .5f - y0;
      float k = ll > 0 ? clampf((fx * dx + fy * dy) / ll, 0, 1) : 0;
      float ex = fx - k * dx, ey = fy - k * dy;
      float d = sqrtf(ex * ex + ey * ey) - hr;
      float cov = clampf(.5f - d, 0, 1);
      if (cov > 0) blend(c, px, py, col, cov);
    }
}
static float edge(float ax, float ay, float bx, float by, float px, float py) {
  float ex = bx - ax, ey = by - ay, l = sqrtf(ex * ex + ey * ey);
  return l > 0 ? ((px - ax) * ey - (py - ay) * ex) / l : 0;
}
void rp_tri(RpCanvas *c, float x0, float y0, float x1, float y1, float x2, float y2,
            uint32_t col) {
  float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
  if (area < 0) { float t; t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
  int bx0 = (int)floorf(fminf(x0, fminf(x1, x2))) - 1, by0 = (int)floorf(fminf(y0, fminf(y1, y2))) - 1;
  int bx1 = (int)ceilf(fmaxf(x0, fmaxf(x1, x2))) + 1, by1 = (int)ceilf(fmaxf(y0, fmaxf(y1, y2))) + 1;
  for (int py = by0; py < by1; py++)
    for (int px = bx0; px < bx1; px++) {
      float fx = px + .5f, fy = py + .5f;
      float d = edge(x0, y0, x1, y1, fx, fy);
      float d2 = edge(x1, y1, x2, y2, fx, fy), d3 = edge(x2, y2, x0, y0, fx, fy);
      if (d2 > d) d = d2;
      if (d3 > d) d = d3;
      float cov = clampf(.5f - d, 0, 1);
      if (cov > 0) blend(c, px, py, col, cov);
    }
}

/* ---- text ---------------------------------------------------------------- */
float rp_line_height(int font, float size) { (void)font; return size; }

static float measure(Cache *ch, const char *s, const char *end) {
  float w = 0;
  while (*s && (!end || s < end)) {
    int cp = utf8(&s);
    Glyph *g = glyph(ch, cp);
    if (g) w += g->adv;
  }
  return w;
}

float rp_text_width(int font, float size, const char *s) {
  fonts();
  if (!g_font_ok[font] || !s) return 0;
  return measure(cache_for(font, size), s, NULL);
}

static float draw_run(RpCanvas *c, Cache *ch, float x, float y, const char *s,
                      const char *end, uint32_t col) {
  float sc = (ch->size_q / 4.f) / (g_ascent[ch->font] - g_descent[ch->font]);
  float base = y + g_ascent[ch->font] * sc;
  float pen = x;
  while (*s && (!end || s < end)) {
    int cp = utf8(&s);
    Glyph *g = glyph(ch, cp);
    if (!g) continue;
    if (g->bmp) {
      int gx = (int)lrintf(pen) + g->xoff, gy = (int)lrintf(base) + g->yoff;
      for (int yy = 0; yy < g->h; yy++)
        for (int xx = 0; xx < g->w; xx++) {
          unsigned char v = g->bmp[yy * g->w + xx];
          if (v) blend(c, gx + xx, gy + yy, col, v / 255.f);
        }
    }
    pen += g->adv;
  }
  return pen - x;
}

float rp_text(RpCanvas *c, int font, float size, float x, float y, const char *s,
              uint32_t col, float max_w) {
  fonts();
  if (!g_font_ok[font] || !s) return 0;
  Cache *ch = cache_for(font, size);
  float w = measure(ch, s, NULL);
  if (max_w <= 0 || w <= max_w) return draw_run(c, ch, x, y, s, NULL, col);
  const char *ell = "\xE2\x80\xA6";
  float ew = measure(ch, ell, NULL);
  const char *p = s, *last = s;
  float acc = 0;
  while (*p) {
    const char *q = p;
    int cp = utf8(&q);
    Glyph *g = glyph(ch, cp);
    float a = g ? g->adv : 0;
    if (acc + a + ew > max_w) break;
    acc += a;
    p = q;
    last = p;
  }
  float dw = draw_run(c, ch, x, y, s, last, col);
  return dw + draw_run(c, ch, x + dw, y, ell, NULL, col);
}

void rp_text_center(RpCanvas *c, int font, float size, float x, float y, float w, float h,
                    const char *s, uint32_t col) {
  float tw = rp_text_width(font, size, s);
  if (tw > w) tw = w;
  rp_text(c, font, size, x + (w - tw) / 2, y + (h - size) / 2, s, col, w);
}

void rp_finish(RpCanvas *c) {
  size_t n = (size_t)c->w * c->h;
  for (size_t i = 0; i < n; i++) {
    uint32_t v = c->px[i];
    unsigned a = v >> 24;
    if (a == 0) { c->px[i] = 0; continue; }
    if (a == 255) continue;
    unsigned r = ((v >> 16) & 255) * 255u / a, g = ((v >> 8) & 255) * 255u / a,
             b = (v & 255) * 255u / a;
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    c->px[i] = (a << 24) | (r << 16) | (g << 8) | b;
  }
}

uint32_t rp_mix(uint32_t a, uint32_t b, float t) {
  uint32_t o = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    float va = (a >> sh) & 255, vb = (b >> sh) & 255;
    o |= (uint32_t)(int)(va + (vb - va) * t + .5f) << sh;
  }
  return o;
}
uint32_t rp_alpha(uint32_t col, float a) {
  int A = (int)(((col >> 24) & 255) * a + .5f);
  if (A > 255) A = 255; if (A < 0) A = 0;
  return (col & 0xFFFFFF) | ((uint32_t)A << 24);
}
