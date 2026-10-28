/* See dc_interp.h. */
#include "dc_interp.h"
#include "snes/ppu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define kRegBytes offsetof(Ppu, vramPointer) /* register snapshot prefix */
#define kLines 225                            /* index = beam line 1..224 */
#define kBgMaxDelta 24  /* px per tick; larger = teleport / scene cut */
#define kObjMaxDelta 24
#define kObjSearchRadius 16

typedef struct Capture {
  Ppu *start;                      /* full PPU after line 1 */
  uint8_t regs[kLines][kRegBytes]; /* per-line registers (HDMA/IRQ aware) */
  uint16_t cgram[kLines][0x100];   /* per-line CGRAM (HDMA gradients) */
  uint32_t serial;
  bool complete;
} Capture;

static DcMotionMode g_mode = kDcMotion_Off;
static Capture g_cap[2];
static int g_cur = -1;    /* newest complete capture */
static int g_filling = 0; /* capture the live raster writes */
static int g_lines_seen;
static uint32_t g_serial;
static Ppu *g_scratch;
static uint8_t *g_buf;
static size_t g_buf_size;
static double g_last_refresh;

DcMotionMode DcInterp_GetMode(void) { return g_mode; }

void DcInterp_SetMode(DcMotionMode m) {
  if (m < kDcMotion_Off || m > kDcMotion_LowLatency) m = kDcMotion_Off;
  g_mode = m;
  g_cur = -1; /* drop history; restart cleanly */
  g_cap[0].complete = g_cap[1].complete = false;
}

void DcInterp_ObserveLine(const struct Ppu *ppu, unsigned line, void *ctx) {
  (void)ctx;
  if (g_mode == kDcMotion_Off || line < 1 || line >= kLines) return;
  Capture *c = &g_cap[g_filling];
  if (line == 1) {
    if (!c->start) c->start = (Ppu *)malloc(sizeof(Ppu));
    if (!c->start) return;
    memcpy(c->start, ppu, sizeof(Ppu));
    c->complete = false;
    g_lines_seen = 0;
  } else if (!c->start) {
    return;
  }
  memcpy(c->regs[line], ppu, kRegBytes);
  memcpy(c->cgram[line], ppu->cgram, sizeof(ppu->cgram));
  g_lines_seen++;
}

void DcInterp_EndFrame(void) {
  if (g_mode == kDcMotion_Off) return;
  Capture *c = &g_cap[g_filling];
  if (!c->start || g_lines_seen != kLines - 1) return;
  c->complete = true;
  c->serial = ++g_serial;
  g_cur = g_filling;
  g_filling ^= 1;
}

/* DC_INTERP_HZ=<hz> forces the presentation rate (headless tests, or a
 * display that misreports its refresh). */
static double forced_hz(void) {
  static int init;
  static double hz;
  if (!init) {
    init = 1;
    const char *e = getenv("DC_INTERP_HZ");
    if (e && *e) hz = atof(e);
  }
  return hz;
}

double DcInterp_PresentationHz(double display_refresh) {
  if (g_mode == kDcMotion_Off) return 0;
  double f = forced_hz();
  if (f >= 100.0) return f;
  if (display_refresh > 0) g_last_refresh = display_refresh;
  double r = display_refresh > 0 ? display_refresh : g_last_refresh;
  if (display_refresh <= 0 && r <= 0) return 120.0; /* "is decoupling wanted?" */
  if (r < 100.0) return 0;
  return r > 240.0 ? 240.0 : r;
}

static inline int wrap_delta(int c, int p, int mod) {
  int d = (c - p) & (mod - 1);
  return d >= mod / 2 ? d - mod : d;
}

static inline int scale_round(int d, float f) { return (int)lrintf((float)d * f); }

typedef struct ObjInfo { int x, y; uint16_t tile_attr; uint8_t size; } ObjInfo;

static void decode_oam(const Ppu *p, ObjInfo *o) {
  for (int i = 0; i < 128; i++) {
    uint16_t w0 = p->oam[i * 2], w1 = p->oam[i * 2 + 1];
    uint8_t hi = (p->highOam[i >> 2] >> ((i & 3) * 2)) & 3;
    o[i].x = (w0 & 0xff) | ((hi & 1) << 8);
    o[i].y = w0 >> 8;
    o[i].tile_attr = w1;
    o[i].size = hi >> 1;
  }
}

static void encode_pos(Ppu *p, int i, int x, int y) {
  x &= 0x1ff;
  y &= 0xff;
  p->oam[i * 2] = (uint16_t)((x & 0xff) | (y << 8));
  uint8_t sh = (uint8_t)((i & 3) * 2);
  p->highOam[i >> 2] =
      (uint8_t)((p->highOam[i >> 2] & ~(1 << sh)) | (((x >> 8) & 1) << sh));
}

/* f: signed fraction of the last tick's motion, relative to the newest
 * field (-1..0 interpolate, 0..1 extrapolate). Renders into `out`, laid out
 * exactly like the live field (same pitch and renderBuffer offset). */
static bool render_subframe(float f, const uint8_t *field, uint8_t *out,
                            size_t out_size) {
  if (g_cur < 0) return false;
  Capture *C = &g_cap[g_cur], *P = &g_cap[g_cur ^ 1];
  if (!C->complete || !P->complete || !P->start || C->serial != P->serial + 1)
    return false;
  const Ppu *cs = C->start;
  if ((cs->inidisp & 0x80) || (cs->bgmode & 7) != (P->start->bgmode & 7))
    return false;
  /* Same frame geometry as the field being presented (a window resize or a
   * widescreen toggle changes it). */
  if (!cs->renderBuffer || cs->renderPitch != P->start->renderPitch ||
      cs->extraLeftCur != P->start->extraLeftCur)
    return false;
  ptrdiff_t off = cs->renderBuffer - field;
  if (off < 0 || (size_t)off + (size_t)cs->renderPitch * 224 > out_size)
    return false;
  if (!g_scratch) g_scratch = (Ppu *)malloc(sizeof(Ppu));
  if (!g_scratch) return false;
  Ppu *s = g_scratch;
  memcpy(s, cs, sizeof(Ppu));
  s->renderBuffer = out + off;
  memset(&s->mode7Hd, 0, sizeof(s->mode7Hd));
  for (int i = 0; i < kPpuOverlaySource_Count; i++) s->overlayRenderBuffer[i] = NULL;
  s->widescreenLineEnhancer = NULL;
  s->widescreenLineEnhancerContext = NULL;

  /* Sprites: pair each newest OAM slot with its previous position by slot +
   * tile/attributes, else by the nearest identical tile. Unmatched slots
   * (spawns, swaps, teleports) stay where the game put them. */
  ObjInfo co[128], po[128];
  decode_oam(cs, co);
  decode_oam(P->start, po);
  for (int i = 0; i < 128; i++) {
    int dx = 0, dy = 0;
    bool ok = false;
    if (po[i].tile_attr == co[i].tile_attr && po[i].size == co[i].size) {
      dx = wrap_delta(co[i].x, po[i].x, 512);
      dy = wrap_delta(co[i].y, po[i].y, 256);
      ok = abs(dx) <= kObjMaxDelta && abs(dy) <= kObjMaxDelta;
    }
    if (!ok) {
      int best = -1, bestd = kObjSearchRadius * 2 + 1;
      for (int j = 0; j < 128; j++) {
        if (po[j].tile_attr != co[i].tile_attr || po[j].size != co[i].size) continue;
        int ddx = wrap_delta(co[i].x, po[j].x, 512), ddy = wrap_delta(co[i].y, po[j].y, 256);
        if (abs(ddx) > kObjSearchRadius || abs(ddy) > kObjSearchRadius) continue;
        int d = abs(ddx) + abs(ddy);
        if (d < bestd) { bestd = d; best = j; dx = ddx; dy = ddy; }
        else if (d == bestd) best = -2; /* ambiguous */
      }
      ok = best >= 0;
    }
    if (!ok || (dx == 0 && dy == 0)) continue;
    /* Parked / off-screen sprites stay parked. */
    if (co[i].y >= 0xE0 && co[i].y < 0xF8) continue;
    encode_pos(s, i, co[i].x + scale_round(dx, f), co[i].y + scale_round(dy, f));
  }

  /* Backgrounds: each line's scroll independently, so parallax, HDMA waves
   * and IRQ splits keep their own motion. Mode 7 lines are left alone. */
  for (int line = 1; line < kLines; line++) {
    memcpy(s, C->regs[line], kRegBytes);
    memcpy(s->cgram, C->cgram[line], sizeof(s->cgram));
    const Ppu *cl = (const Ppu *)C->regs[line];
    const Ppu *pl = (const Ppu *)P->regs[line];
    if ((cl->bgmode & 7) != 7 && (cl->bgmode & 7) == (pl->bgmode & 7)) {
      for (int l = 0; l < 4; l++) {
        int dx = wrap_delta(cl->hScroll[l], pl->hScroll[l], 1024);
        int dy = wrap_delta(cl->vScroll[l], pl->vScroll[l], 1024);
        if (abs(dx) > kBgMaxDelta || abs(dy) > kBgMaxDelta) continue;
        s->hScroll[l] = (uint16_t)((cl->hScroll[l] + scale_round(dx, f)) & 0x3ff);
        s->vScroll[l] = (uint16_t)((cl->vScroll[l] + scale_round(dy, f)) & 0x3ff);
      }
    }
    ppu_runLine(s, line);
  }
  return true;
}

int DcInterp_DrawFrame(uint8_t *dst, size_t pitch, const uint8_t *field,
                       int w, int h, double alpha) {
  if (g_mode == kDcMotion_Off || !field || alpha >= 1.0 || alpha < 0.0) return 0;
  double hz = DcInterp_PresentationHz(g_last_refresh);
  if (hz < 100.0) return 0;
  int n = (int)lrint(hz / 60.0);
  if (n < 2) return 0;
  /* SRW1's sub-frame phases: k = 0..n-1 since the newest tick. */
  int k = (int)floor(alpha * n);
  if (k < 0) k = 0;
  if (k > n - 1) k = n - 1;
  float f = g_mode == kDcMotion_Interpolate ? (float)(k + 1) / n - 1.0f
                                            : (float)k / n;
  if (f == 0.0f) return 0;
  size_t row = (size_t)w * 4;
  size_t need = row * (size_t)(h + 1);
  if (need > g_buf_size) {
    free(g_buf);
    g_buf = (uint8_t *)calloc(1, need);
    g_buf_size = g_buf ? need : 0;
  }
  if (!g_buf || !render_subframe(f, field, g_buf, g_buf_size)) return 0;
  for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * pitch, g_buf + (size_t)y * row, row);
  return 1;
}
