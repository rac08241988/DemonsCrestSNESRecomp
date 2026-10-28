/* Demon's Crest: in-game options menu (F12 or Escape / gamepad Guide).
 *
 * Hosts the RetroArch .glslp shader-preset picker and the 120 Hz motion
 * mode. Presets are scanned from shaders/presets/ beside the executable; the
 * choice persists in dc_options.ini and is selectable ONLY from this menu
 * (config.ini's Shader key is ignored). Opening the menu freezes the game
 * (the host's modal hook) and draws over the last picture.
 *
 * Ported from the SRW1 recomp add-on's srw1_options.c. */
#ifndef DC_OPTIONS_H
#define DC_OPTIONS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void DcOptions_Init(void);
/* after_config: decides the startup shader (g_config.shader). */
void DcOptions_AfterConfig(void);
/* SnesDesktopHostGame hooks. */
int DcOptions_OnKey(int key, int mod, int pressed, int repeat);
int DcOptions_OnGamepadButton(int sdl_button, int pressed);
int DcOptions_ModalActive(void);
void DcOptions_DrawModal(uint8_t *dst, size_t pitch, int w, int h);
/* Once per simulated frame (test hooks). */
void DcOptions_OnSimFrame(unsigned number);
/* src/dc_default_config.c */
extern const char kDcDefaultConfigIni[];
#ifdef __cplusplus
}
#endif
#endif
