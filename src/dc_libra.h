#pragma once
/*
 * librashader for the in-game Shader menu: libretro .slangp presets on the
 * OpenGL presenter (GL runtime) and on SDL's Vulkan renderer (Vulkan
 * runtime). librashader.dll is loaded at runtime (librashader_ld.h); it is
 * never linked into the executable, and the shader files ship as unmodified
 * source beside it.
 */
#include <stdbool.h>

typedef enum DcLibraBackend {
  DC_LIBRA_NONE = 0,   /* this renderer cannot run slang presets */
  DC_LIBRA_GL,
  DC_LIBRA_VULKAN,
} DcLibraBackend;

/* Installs the presenter hooks (idempotent). Call once the renderer exists. */
void DcLibra_Install(void);
/* Which runtime the live renderer would use. */
DcLibraBackend DcLibra_Backend(void);
/* Whether librashader.dll could be loaded. */
bool DcLibra_Available(void);
/* Select a .slangp (NULL = off). The chain is built on the next presented
 * frame; DcLibra_Poll reports the outcome. */
void DcLibra_SetPreset(const char *path);
const char *DcLibra_Preset(void);
/* 1 = the requested preset is running, -1 = it failed (msg filled),
 * 0 = still pending / nothing requested. Clears the outcome once read. */
int DcLibra_Poll(char *msg, int cap);
