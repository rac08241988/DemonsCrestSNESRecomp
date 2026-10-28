/* See dc_options.h. */
#include "dc_options.h"
#include "dc_interp.h"
#include "recomp_runtime_ui.h"
#include "desktop/config.h"
#include "desktop/host_main.h"
#include "desktop/opengl.h"
#include "desktop/sdl_compat.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DC_SHADER_DIR "shaders/presets"
#define DC_OPTIONS_FILE "dc_options.ini"
#define DC_MAX_PRESETS 512

static const char *const kMotionLabels[] = {"Off", "Interpolate", "Low-latency"};
static const char *const kMotionKeys[] = {"off", "interpolate", "lowlatency"};

static char g_root[1024];                    /* resolved preset root */
static char *g_paths[DC_MAX_PRESETS + 1];    /* [0] = Off (NULL) */
static char *g_labels[DC_MAX_PRESETS + 1];
static size_t g_count;                       /* including Off */
static int g_current;                        /* applied index */
static int g_pending = -1;                   /* chosen, not yet applied */
static uint64_t g_pending_at;
static RecompRuntimeUi *g_ui;
static char g_status[200];
static bool g_initialized;
static const RecompRuntimeUiItem *g_items;
enum { kItemShader, kItemMotion, kItemResume, kItemCount };

static int parse_motion(const char *v) {
  for (int i = 0; i < 3; i++)
    if (strcmp(v, kMotionKeys[i]) == 0) return i;
  return -1;
}

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void scan_dir(const char *dir, char **found, size_t *n) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL && *n < DC_MAX_PRESETS) {
    if (e->d_name[0] == '.') continue;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(path, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      scan_dir(path, found, n);
    } else {
      size_t len = strlen(path);
      if (len > 6 && strcmp(path + len - 6, ".glslp") == 0) found[(*n)++] = strdup(path);
    }
  }
  closedir(d);
}

/* Label = path below the preset root without ".glslp" ("crt/crt-geom"). It
 * is also what dc_options.ini stores, so the file survives moving the game. */
static char *make_label(const char *path) {
  const char *rel = path + strlen(g_root) + 1;
  size_t len = strlen(rel) - 6;
  char *s = (char *)malloc(len + 1);
  memcpy(s, rel, len);
  s[len] = 0;
  return s;
}

static bool dir_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The host chdirs to the directory holding config.ini (normally the exe's);
 * fall back to the executable's own directory. */
static void resolve_root(void) {
  snprintf(g_root, sizeof(g_root), "%s", DC_SHADER_DIR);
  if (dir_exists(g_root)) return;
#if SNESRECOMP_SDL3
  const char *base = SDL_GetBasePath();
  if (base) {
    snprintf(g_root, sizeof(g_root), "%s%s", base, DC_SHADER_DIR);
    if (!dir_exists(g_root)) snprintf(g_root, sizeof(g_root), "%s", DC_SHADER_DIR);
  }
#else
  char *base = SDL_GetBasePath();
  if (base) {
    snprintf(g_root, sizeof(g_root), "%s%s", base, DC_SHADER_DIR);
    if (!dir_exists(g_root)) snprintf(g_root, sizeof(g_root), "%s", DC_SHADER_DIR);
    SDL_free(base);
  }
#endif
}

static void save_choice(void) {
  FILE *f = fopen(DC_OPTIONS_FILE, "w");
  if (!f) return;
  fprintf(f, "# Written by the in-game options menu (F12 or Escape / gamepad Guide). Edit there.\n");
  fprintf(f, "# ShaderPreset: a preset below shaders/presets, without .glslp (empty = off)\n");
  fprintf(f, "ShaderPreset = %s\n", g_current > 0 ? g_labels[g_current] : "");
  fprintf(f, "# MotionMode: off | interpolate | lowlatency (needs a 100 Hz+ display)\n");
  fprintf(f, "MotionMode = %s\n", kMotionKeys[DcInterp_GetMode()]);
  fclose(f);
}

static bool read_key(const char *key, char *out, size_t cap) {
  FILE *f = fopen(DC_OPTIONS_FILE, "r");
  if (!f) return false;
  bool found = false;
  char line[1200];
  size_t kl = strlen(key);
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, key, kl) != 0) continue;
    char *v = strchr(line, '=');
    if (!v) continue;
    v++;
    while (*v == ' ' || *v == '\t') v++;
    size_t l = strlen(v);
    while (l && (v[l - 1] == '\n' || v[l - 1] == '\r' || v[l - 1] == ' ')) v[--l] = 0;
    snprintf(out, cap, "%s", v);
    found = true;
  }
  fclose(f);
  return found;
}

static void load_motion(void) {
  char v[64];
  int m = -1;
  if (read_key("MotionMode", v, sizeof(v))) m = parse_motion(v);
  const char *env = getenv("DC_MOTION"); /* test override */
  if (env && parse_motion(env) >= 0) m = parse_motion(env);
  if (m >= 0) DcInterp_SetMode((DcMotionMode)m);
}

static int find_label(const char *want) {
  if (!want || !want[0]) return 0;
  for (size_t i = 1; i < g_count; i++)
    if (strcmp(g_labels[i], want) == 0) return (int)i;
  return -1;
}

static int load_choice(void) {
  char want[1024] = {0};
  const char *env = getenv("DC_SHADER_PRESET"); /* test override */
  if (env) snprintf(want, sizeof(want), "%s", env);
  else read_key("ShaderPreset", want, sizeof(want));
  int i = find_label(want);
  if (i < 0) {
    fprintf(stderr, "[dc] shader preset '%s' not found under %s\n", want, g_root);
    return 0;
  }
  return i;
}

static int ui_get(void *ctx, const RecompRuntimeUiItem *item, int *out) {
  (void)ctx;
  if (item == &g_items[kItemShader]) {
    *out = g_pending >= 0 ? g_pending : g_current;
    return 1;
  }
  if (item == &g_items[kItemMotion]) {
    *out = (int)DcInterp_GetMode();
    return 1;
  }
  return 0;
}

static int ui_set(void *ctx, const RecompRuntimeUiItem *item, int value) {
  (void)ctx;
  if (item == &g_items[kItemMotion]) {
    if (value < 0 || value > 2) return 0;
    DcInterp_SetMode((DcMotionMode)value);
    snesrecomp_desktop_request_clock_reset();
    save_choice();
    snprintf(g_status, sizeof(g_status), "120Hz motion: %s%s", kMotionLabels[value],
             value ? " (active on 100 Hz+ displays)" : "");
    recomp_runtime_ui_set_status(g_ui, g_status);
    return 1;
  }
  if (item != &g_items[kItemShader]) return 0;
  if (value < 0 || (size_t)value >= g_count) return 0;
  g_pending = value;
  g_pending_at = SDL_GetTicks();
  return 1;
}

static int ui_action(void *ctx, const RecompRuntimeUiItem *item) {
  (void)ctx;
  if (item == &g_items[kItemResume]) {
    recomp_runtime_ui_close(g_ui);
    return 1;
  }
  return 0;
}

static int ui_enabled(void *ctx, const RecompRuntimeUiItem *item) {
  (void)ctx;
  if (item == &g_items[kItemShader]) return OpenGLRenderer_IsActive() && g_count > 1;
  return 1;
}

void DcOptions_Init(void) {
  if (g_initialized) return;
  g_initialized = true;
  resolve_root();
  char *found[DC_MAX_PRESETS];
  size_t n = 0;
  scan_dir(g_root, found, &n);
  qsort(found, n, sizeof(found[0]), cmp_str);
  g_paths[0] = NULL;
  g_labels[0] = strdup("Off");
  for (size_t i = 0; i < n; i++) {
    g_paths[i + 1] = found[i];
    g_labels[i + 1] = make_label(found[i]);
  }
  g_count = n + 1;
  g_current = load_choice();
  load_motion();
  fprintf(stderr, "[dc] %zu shader presets under %s; startup shader=%s motion=%s\n", n,
          g_root, g_current ? g_labels[g_current] : "Off", kMotionKeys[DcInterp_GetMode()]);

  static RecompRuntimeUiItem items[kItemCount];
  memset(items, 0, sizeof(items));
  items[kItemShader].key = "graphics.shader_preset";
  items[kItemShader].section = "Video";
  items[kItemShader].label = "Shader";
  items[kItemShader].description =
      "RetroArch shader (OpenGL)";
  items[kItemShader].type = RECOMP_RUNTIME_UI_CHOICE;
  items[kItemShader].minimum = 0;
  items[kItemShader].maximum = (int)g_count - 1;
  items[kItemShader].step = 1;
  items[kItemShader].choices = (const char *const *)g_labels;
  items[kItemShader].choice_count = g_count;
  items[kItemMotion].key = "graphics.motion";
  items[kItemMotion].section = "Video";
  items[kItemMotion].label = "120Hz motion";
  items[kItemMotion].description =
      "120Hz output, 60Hz logic";
  items[kItemMotion].type = RECOMP_RUNTIME_UI_CHOICE;
  items[kItemMotion].minimum = 0;
  items[kItemMotion].maximum = 2;
  items[kItemMotion].step = 1;
  items[kItemMotion].choices = kMotionLabels;
  items[kItemMotion].choice_count = 3;
  items[kItemResume].key = RECOMP_RUNTIME_UI_KEY_RESUME;
  items[kItemResume].section = "Video";
  items[kItemResume].label = "Resume";
  items[kItemResume].type = RECOMP_RUNTIME_UI_ACTION;
  g_items = items;

  static RecompRuntimeUiConfig cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.title = "Options";
  cfg.subtitle = "Demon's Crest";
  cfg.items = items;
  cfg.item_count = kItemCount;
  cfg.theme = "snes";
  cfg.callbacks.get_value = ui_get;
  cfg.callbacks.set_value = ui_set;
  cfg.callbacks.run_action = ui_action;
  cfg.callbacks.is_enabled = ui_enabled;
  g_ui = recomp_runtime_ui_create(&cfg);
}

void DcOptions_AfterConfig(void) {
  DcOptions_Init();
  /* The preset is chosen only in the in-game menu: config.ini's Shader key
   * (and the launcher) never decide it. */
  static char path[1100];
  if (g_current > 0) {
    snprintf(path, sizeof(path), "%s", g_paths[g_current]);
    g_config.shader = path;
  } else {
    g_config.shader = NULL;
  }
}

static void toggle(void) {
  if (!g_ui) return;
  if (recomp_runtime_ui_is_open(g_ui)) recomp_runtime_ui_close(g_ui);
  else recomp_runtime_ui_open(g_ui);
  fprintf(stderr, "[dc] options menu %s\n",
          recomp_runtime_ui_is_open(g_ui) ? "opened" : "closed");
}

static int feed(RecompRuntimeUiInput in, int pressed, int repeat) {
  recomp_runtime_ui_handle_input(g_ui, in, pressed, repeat);
  return 1;
}

int DcOptions_OnKey(int key, int mod, int pressed, int repeat) {
  DcOptions_Init();
  if (!g_ui) return 0;
  /* Plain F12 opens the menu. Shift+F12 stays the host's Rewind. */
  /* Escape toggles it too (open, and close from anywhere in the menu). In
   * DC's single-player gameplay loop Escape had no binding (the framework
   * uses it only to leave a netplay session or close its own overlays). */
  if ((key == SDLK_F12 || key == SDLK_ESCAPE) &&
      !(mod & (KMOD_SHIFT | KMOD_CTRL | KMOD_ALT))) {
    if (pressed && !repeat) toggle();
    return 1;
  }
  if (!recomp_runtime_ui_is_open(g_ui)) return 0;
  switch (key) {
  case SDLK_UP: return feed(RECOMP_RUNTIME_UI_INPUT_UP, pressed, repeat);
  case SDLK_DOWN: return feed(RECOMP_RUNTIME_UI_INPUT_DOWN, pressed, repeat);
  case SDLK_LEFT: return feed(RECOMP_RUNTIME_UI_INPUT_LEFT, pressed, repeat);
  case SDLK_RIGHT: return feed(RECOMP_RUNTIME_UI_INPUT_RIGHT, pressed, repeat);
  case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
    return feed(RECOMP_RUNTIME_UI_INPUT_ACCEPT, pressed, repeat);
  case SDLK_BACKSPACE:
    return feed(RECOMP_RUNTIME_UI_INPUT_BACK, pressed, repeat);
  default: return 1; /* swallow everything else while open */
  }
}

int DcOptions_OnGamepadButton(int b, int pressed) {
  DcOptions_Init();
  if (!g_ui) return 0;
  if (b == SDL_CONTROLLER_BUTTON_GUIDE) {
    if (pressed) toggle();
    return 1;
  }
  if (!recomp_runtime_ui_is_open(g_ui)) return 0;
  switch (b) {
  case SDL_CONTROLLER_BUTTON_DPAD_UP: return feed(RECOMP_RUNTIME_UI_INPUT_UP, pressed, 0);
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return feed(RECOMP_RUNTIME_UI_INPUT_DOWN, pressed, 0);
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return feed(RECOMP_RUNTIME_UI_INPUT_LEFT, pressed, 0);
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return feed(RECOMP_RUNTIME_UI_INPUT_RIGHT, pressed, 0);
  case SDL_CONTROLLER_BUTTON_A: return feed(RECOMP_RUNTIME_UI_INPUT_ACCEPT, pressed, 0);
  case SDL_CONTROLLER_BUTTON_B: return feed(RECOMP_RUNTIME_UI_INPUT_BACK, pressed, 0);
  case SDL_CONTROLLER_BUTTON_START: return feed(RECOMP_RUNTIME_UI_INPUT_TOGGLE, pressed, 0);
  default: return 1;
  }
}

static void apply_pending(void) {
  int want = g_pending;
  g_pending = -1;
  if (want < 0 || want == g_current) return;
  if (OpenGLRenderer_SetShader(want > 0 ? g_paths[want] : NULL)) {
    g_current = want;
    save_choice();
    snprintf(g_status, sizeof(g_status), "Shader: %s", g_labels[want]);
  } else {
    snprintf(g_status, sizeof(g_status), "Failed: %s", g_labels[want]);
    fprintf(stderr, "[dc] shader preset failed to load: %s\n",
            want > 0 ? g_paths[want] : "(off)");
  }
  recomp_runtime_ui_set_status(g_ui, g_status);
}

/* Test hooks (headless, no input device):
 *   DC_OPTIONS_OPEN_AT=<sim frame>  open the menu at that frame
 *   DC_OPTIONS_CLOSE_AFTER=<n>      close it again after n modal presents
 *   DC_OPTIONS_SELECT=<label>       while open, pick that shader preset */
static long g_close_after = -1;
static long g_modal_presents;

void DcOptions_OnSimFrame(unsigned number) {
  static long open_at = -2;
  if (open_at == -2) {
    const char *e = getenv("DC_OPTIONS_OPEN_AT");
    open_at = e && *e ? atol(e) : -1;
    const char *c = getenv("DC_OPTIONS_CLOSE_AFTER");
    g_close_after = c && *c ? atol(c) : -1;
  }
  if (open_at >= 0 && (long)number == open_at && g_ui && !recomp_runtime_ui_is_open(g_ui)) {
    recomp_runtime_ui_open(g_ui);
    g_modal_presents = 0;
    const char *sel = getenv("DC_OPTIONS_SELECT");
    int i = sel ? find_label(sel) : -1;
    if (i >= 0) {
      g_pending = i;
      g_pending_at = SDL_GetTicks();
    }
  }
}

int DcOptions_ModalActive(void) {
  if (g_pending >= 0 && SDL_GetTicks() - g_pending_at >= 300) apply_pending();
  if (!g_ui || !recomp_runtime_ui_is_open(g_ui)) return 0;
  if (g_close_after >= 0 && g_modal_presents >= g_close_after) {
    recomp_runtime_ui_close(g_ui);
    return 0;
  }
  return 1;
}

void DcOptions_DrawModal(uint8_t *dst, size_t pitch, int w, int h) {
  g_modal_presents++;
  if (g_ui && recomp_runtime_ui_is_open(g_ui))
    recomp_runtime_ui_render_argb8888(g_ui, dst, w, h, (int)pitch);
}
