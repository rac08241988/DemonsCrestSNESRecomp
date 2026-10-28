/* Demon's Crest: render-side motion for 120 Hz+ displays.
 *
 * Game logic stays at 60 Hz. When a mode other than Off is chosen and the
 * display refreshes at 100 Hz or more, the host presents faster than it
 * simulates and asks draw_frame for in-between pictures. Each one is built
 * by re-rasterizing the PPU state captured from the newest field (per line:
 * registers, CGRAM; once: the whole PPU incl. VRAM/OAM and the widescreen
 * margin state) with interpolated BG scroll and OAM positions. Off is the
 * untouched authentic path.
 *
 * Ported from the SRW1 (Super Ryu World 1) recomp add-on and adapted to the
 * beam-driven raster (the observer sees every line's live register state)
 * and to Demon's Crest's native widescreen frame (any width up to
 * kPpuBufWidth). */
#ifndef DC_INTERP_H
#define DC_INTERP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
  kDcMotion_Off = 0,
  kDcMotion_Interpolate = 1, /* in-betweens toward the newest tick (+1 tick latency) */
  kDcMotion_LowLatency = 2,  /* extrapolate past the newest tick */
} DcMotionMode;

struct Ppu;
DcMotionMode DcInterp_GetMode(void);
void DcInterp_SetMode(DcMotionMode mode);
/* Beam-driver observer: called after ppu_runLine(line), line 1..224. */
void DcInterp_ObserveLine(const struct Ppu *ppu, unsigned line, void *ctx);
/* After the live raster finished the field. */
void DcInterp_EndFrame(void);
/* SnesDesktopHostGame hooks. */
double DcInterp_PresentationHz(double display_refresh);
int DcInterp_DrawFrame(uint8_t *dst, size_t pitch, const uint8_t *field,
                       int w, int h, double alpha);
#ifdef __cplusplus
}
#endif
#endif
