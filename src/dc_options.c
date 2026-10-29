/* See dc_options.h.
 *
 * The in-game Options menu (F12 / Escape / gamepad Guide). Built on the
 * framework's runtime menu (recomp_runtime_ui) and, for everything the
 * launcher's Settings screen owns, on the launcher's own settings model:
 * snesrecomp_desktop_settings_read seeds a RecompLauncherCSettings from the
 * live config exactly as the launcher does, and settings_apply commits it the
 * launcher's way (config.ini, live apply, or a resuming restart). Rows here
 * only edit that struct; nothing about a setting is re-implemented.
 *
 * Sections: Video (Shader, Widescreen, 120Hz motion, Renderer, VSync, Frame
 * blend, Run-ahead), Save states, Display, Audio, Controls, System.
 */
#include "dc_options.h"
#include "dc_interp.h"
#include "dc_libra.h"
#include "dc_widescreen.h"
#include "recomp_runtime_ui.h"
#include "desktop/config.h"
#include "desktop/host_main.h"
#include "desktop/opengl.h"
#include "desktop/sdl_compat.h"
#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#endif
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void RtlSaveSlotPath(int slot, char *buf, size_t buflen);

#define DC_GLSL_DIR "shaders/presets"
#define DC_SLANG_DIR "shaders/slang/libretro/presets"
#define DC_SLANG_ROOT "shaders/slang"
#define DC_OPTIONS_FILE "dc_options.ini"
#define DC_MAX_PRESETS 1024
#define DC_STATE_SLOTS 10
#define DC_SHADER_NOTE "Shaders work on Vulkan and OpenGL renderers only."

static const char *const kMotionLabels[] = {"Off", "Interpolate", "Low-latency"};
static const char *const kMotionKeys[] = {"off", "interpolate", "lowlatency"};
static const char *const kWidescreenLabels[] = {"Off", "Adaptive", "16:9", "21:9", "32:9"};
static const char *const kVsyncLabels[] = {"On", "Off", "Adaptive"};
static const int kVsyncValues[] = {1, 2, 3}; /* RECOMP_LAUNCHER_VSYNC_* */
static const char *const kFullscreenLabels[] = {"Windowed", "Borderless", "Exclusive"};
static const char *const kAudioRateLabels[] = {"22050 Hz", "32000 Hz", "44100 Hz", "48000 Hz"};
static const int kAudioRateValues[] = {22050, 32000, 44100, 48000};
static const char *const kSourceLabels[] = {"None", "Keyboard", "Gamepad"};

/* ---- shader presets ------------------------------------------------------ */

typedef enum { kKindOff, kKindSlang, kKindGlsl } PresetKind;
static char *g_paths[DC_MAX_PRESETS + 1];    /* [0] = Off (NULL) */
static char *g_labels[DC_MAX_PRESETS + 1];   /* shown */
static char *g_ids[DC_MAX_PRESETS + 1];      /* stored in dc_options.ini */
static PresetKind g_kinds[DC_MAX_PRESETS + 1];
static size_t g_count;                       /* including Off */
static int g_current;                        /* applied (or saved) index */
static int g_shader_pending = -1;            /* chosen with Left/Right */
static int g_shader_waiting = -1;            /* applied, awaiting librashader */

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void scan_dir(const char *dir, const char *ext, char **found, size_t *n) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  size_t el = strlen(ext);
  while ((e = readdir(d)) != NULL && *n < DC_MAX_PRESETS) {
    if (e->d_name[0] == '.') continue;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(path, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      scan_dir(path, ext, found, n);
    } else {
      size_t len = strlen(path);
      if (len > el && strcmp(path + len - el, ext) == 0) found[(*n)++] = strdup(path);
    }
  }
  closedir(d);
}

static bool dir_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The host chdirs to the directory holding config.ini (normally the exe's);
 * fall back to the executable's own directory. */
static void resolve_root(const char *rel, char *out, size_t cap) {
  snprintf(out, cap, "%s", rel);
  if (dir_exists(out)) return;
#if SNESRECOMP_SDL3
  const char *base = SDL_GetBasePath();
  if (base) {
    snprintf(out, cap, "%s%s", base, rel);
    if (!dir_exists(out)) snprintf(out, cap, "%s", rel);
  }
#else
  char *base = SDL_GetBasePath();
  if (base) {
    snprintf(out, cap, "%s%s", base, rel);
    if (!dir_exists(out)) snprintf(out, cap, "%s", rel);
    SDL_free(base);
  }
#endif
}

static char *strip(const char *path, const char *root, const char *ext) {
  const char *rel = path + strlen(root) + 1;
  size_t len = strlen(rel) - strlen(ext);
  char *s = (char *)malloc(len + 1);
  memcpy(s, rel, len);
  s[len] = 0;
  return s;
}

static char *join(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  char *s = (char *)malloc(la + lb + 1);
  memcpy(s, a, la);
  memcpy(s + la, b, lb + 1);
  return s;
}

static void add_presets(const char *rel, const char *ext, PresetKind kind,
                        const char *id_prefix, const char *label_prefix) {
  char root[1024];
  resolve_root(rel, root, sizeof(root));
  char **found = (char **)calloc(DC_MAX_PRESETS, sizeof(char *));
  size_t n = 0;
  scan_dir(root, ext, found, &n);
  qsort(found, n, sizeof(found[0]), cmp_str);
  for (size_t i = 0; i < n && g_count <= DC_MAX_PRESETS; i++) {
    char *name = strip(found[i], root, ext);
    g_paths[g_count] = found[i];
    g_ids[g_count] = join(id_prefix, name);
    g_labels[g_count] = join(label_prefix, name);
    g_kinds[g_count] = kind;
    g_count++;
    free(name);
  }
  fprintf(stderr, "[dc] %zu %s presets under %s\n", n, ext, root);
  free(found);
}

static int find_id(const char *want) {
  if (!want || !want[0] || strcmp(want, "Off") == 0) return 0;
  for (size_t i = 1; i < g_count; i++)
    if (strcmp(g_ids[i], want) == 0 || strcmp(g_labels[i], want) == 0) return (int)i;
  /* v1.0.0 stored glsl presets without a prefix. */
  for (size_t i = 1; i < g_count; i++)
    if (g_kinds[i] == kKindGlsl && strcmp(g_ids[i] + 5, want) == 0) return (int)i;
  return -1;
}

/* ---- dc_options.ini ------------------------------------------------------ */

static int g_slot = 1;

static void save_choice(void) {
  FILE *f = fopen(DC_OPTIONS_FILE, "w");
  if (!f) return;
  fprintf(f, "# Written by the in-game options menu (F12 or Escape / gamepad Guide). Edit there.\n");
  fprintf(f, "# ShaderPreset: slang/<preset below shaders/slang/libretro/presets> or\n");
  fprintf(f, "# glsl/<preset below shaders/presets>, without the extension (empty = off)\n");
  fprintf(f, "ShaderPreset = %s\n", g_current > 0 ? g_ids[g_current] : "");
  fprintf(f, "# MotionMode: off | interpolate | lowlatency (needs a 100 Hz+ display)\n");
  fprintf(f, "MotionMode = %s\n", kMotionKeys[DcInterp_GetMode()]);
  fprintf(f, "# StateSlot: the slot Save State / Load State use (1-%d)\n", DC_STATE_SLOTS);
  fprintf(f, "StateSlot = %d\n", g_slot);
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

static int parse_motion(const char *v) {
  for (int i = 0; i < 3; i++)
    if (strcmp(v, kMotionKeys[i]) == 0) return i;
  return -1;
}

static void load_saved(void) {
  char v[1024];
  int m = -1;
  if (read_key("MotionMode", v, sizeof(v))) m = parse_motion(v);
  const char *env = getenv("DC_MOTION"); /* test override */
  if (env && parse_motion(env) >= 0) m = parse_motion(env);
  if (m >= 0) DcInterp_SetMode((DcMotionMode)m);
  if (read_key("StateSlot", v, sizeof(v))) {
    int s = atoi(v);
    if (s >= 1 && s <= DC_STATE_SLOTS) g_slot = s;
  }
  v[0] = 0;
  env = getenv("DC_SHADER_PRESET"); /* test override */
  if (env) snprintf(v, sizeof(v), "%s", env);
  else read_key("ShaderPreset", v, sizeof(v));
  int i = find_id(v);
  if (i < 0) {
    fprintf(stderr, "[dc] shader preset '%s' not found\n", v);
    i = 0;
  }
  g_current = i;
}

/* ---- the launcher's settings model -------------------------------------- */

#if defined(RECOMP_LAUNCHER)
static RecompLauncherCSettings g_ls;
static bool g_ls_valid;
static int g_renderer_pending = -1;
static int g_rate_pending = -1;
static const char *const *g_renderer_labels;
static int g_renderer_count;

static RecompLauncherCSettings *ls(void) {
  if (!g_ls_valid) {
    memset(&g_ls, 0, sizeof(g_ls));
    snesrecomp_desktop_settings_read(&g_ls);
    g_ls_valid = true;
  }
  return &g_ls;
}

/* Commit; returns 1 when a resuming restart was scheduled. */
static int ls_apply(void) {
  int restart = snesrecomp_desktop_settings_apply(&g_ls);
  g_ls_valid = false;
  return restart;
}
#endif

/* ---- menu ---------------------------------------------------------------- */

static RecompRuntimeUi *g_ui;
static char g_status[256];
static bool g_initialized;
static bool g_accepting;      /* the ACCEPT input is being handled */
static char g_shader_desc[400];
static char g_slot_desc[200];
static char g_renderer_desc[200];

static bool is(const RecompRuntimeUiItem *item, const char *key) {
  return item && item->key && strcmp(item->key, key) == 0;
}

static void status(const char *fmt, const char *a, const char *b) {
  snprintf(g_status, sizeof(g_status), fmt, a ? a : "", b ? b : "");
  recomp_runtime_ui_set_status(g_ui, g_status);
  fprintf(stderr, "[dc] options: %s\n", g_status);
}

static const char *renderer_now(void) {
  switch (DcLibra_Backend()) {
  case DC_LIBRA_GL: return "OpenGL";
  case DC_LIBRA_VULKAN: return "Vulkan";
  default: return NULL;
  }
}

/* What the selected preset would do on the live renderer. */
static void describe_shader(int index) {
  DcLibraBackend be = DcLibra_Backend();
  const char *pending = index != g_current ? "Enter applies. " : "";
  if (be == DC_LIBRA_NONE) {
    snprintf(g_shader_desc, sizeof(g_shader_desc),
             "%s This renderer (%s) cannot run them: pick a preset and press Enter "
             "to switch to OpenGL (restarts, resumes where you are), or set "
             "Renderer to Vulkan or OpenGL.",
             DC_SHADER_NOTE, snesrecomp_desktop_renderer_id());
  } else if (index > 0 && g_kinds[index] == kKindGlsl && be == DC_LIBRA_VULKAN) {
    snprintf(g_shader_desc, sizeof(g_shader_desc),
             "%sGLSL presets need OpenGL: Enter switches the renderer to OpenGL "
             "(restarts, resumes where you are). Slang presets run on Vulkan.",
             pending);
  } else {
    snprintf(g_shader_desc, sizeof(g_shader_desc),
             "%s%s L/R: %zu presets, Enter applies (now %s).",
             pending, DC_SHADER_NOTE, g_count - 1, renderer_now());
  }
}

static void describe_slot(void) {
  char path[512];
  RtlSaveSlotPath(g_slot - 1, path, sizeof(path));
  struct stat st;
  if (stat(path, &st) == 0)
    snprintf(g_slot_desc, sizeof(g_slot_desc),
             "Slot %d holds a state. Save State overwrites it; Load State "
             "returns to it.", g_slot);
  else
    snprintf(g_slot_desc, sizeof(g_slot_desc), "Slot %d is empty.", g_slot);
}

static void apply_shader(int want) {
  g_shader_pending = -1;
  if (want < 0 || (size_t)want >= g_count) return;
  DcLibraBackend be = DcLibra_Backend();
  PresetKind kind = g_kinds[want];
  if (want > 0 && (be == DC_LIBRA_NONE || (kind == kKindGlsl && be == DC_LIBRA_VULKAN))) {
    /* Save the pick, then restart on OpenGL, which runs both kinds. */
    g_current = want;
    save_choice();
    status("Shader %s: switching to OpenGL (restart, resumes here)%s", g_labels[want], "");
    snesrecomp_desktop_request_renderer_restart("opengl");
    return;
  }
  /* Turn the other kind off first. */
  if (kind != kKindGlsl && OpenGLRenderer_IsActive()) OpenGLRenderer_SetShader(NULL);
  if (kind != kKindSlang) DcLibra_SetPreset(NULL);
  if (want == 0) {
    g_current = 0;
    g_config.shader = NULL;
    save_choice();
    status("Shader: Off%s%s", "", "");
    return;
  }
  if (kind == kKindGlsl) {
    if (OpenGLRenderer_SetShader(g_paths[want])) {
      g_current = want;
      static char path[1100];
      snprintf(path, sizeof(path), "%s", g_paths[want]);
      g_config.shader = path;
      save_choice();
      status("Shader: %s%s", g_labels[want], "");
    } else {
      status("Failed: %s%s", g_labels[want], "");
    }
    return;
  }
  g_config.shader = NULL;
  DcLibra_SetPreset(g_paths[want]);
  g_shader_waiting = want;
  status("Loading %s...%s", g_labels[want], "");
}

/* librashader builds the chain on the next presented frame. */
static void poll_shader(void) {
  char msg[256];
  int o = DcLibra_Poll(msg, sizeof(msg));
  if (!o || g_shader_waiting < 0) return;
  int want = g_shader_waiting;
  g_shader_waiting = -1;
  if (o > 0) {
    g_current = want;
    save_choice();
    status("Shader: %s (%s)", g_labels[want], msg);
  } else {
    status("Failed: %s: %s", g_labels[want], msg);
  }
}

static int ui_get(void *ctx, const RecompRuntimeUiItem *item, int *out) {
  (void)ctx;
  if (is(item, "graphics.shader_preset")) {
    *out = g_shader_pending >= 0 ? g_shader_pending : g_current;
    describe_shader(*out);
    return 1;
  }
  if (is(item, "graphics.widescreen")) { *out = DcWidescreenSetting_Get(); return 1; }
  if (is(item, "graphics.motion")) { *out = (int)DcInterp_GetMode(); return 1; }
  if (is(item, RECOMP_RUNTIME_UI_KEY_STATE_SLOT)) { *out = g_slot; describe_slot(); return 1; }
#if defined(RECOMP_LAUNCHER)
  RecompLauncherCSettings *s = ls();
  if (is(item, "video.renderer")) {
    *out = g_renderer_pending >= 0 ? g_renderer_pending : s->renderer;
    snprintf(g_renderer_desc, sizeof(g_renderer_desc),
             "%sNeeds a restart: Enter restarts the game on it and resumes where "
             "you are. %s",
             *out != s->renderer ? "Not applied yet. " : "", DC_SHADER_NOTE);
    return 1;
  }
  if (is(item, "video.vsync")) { *out = s->vsync >= 1 && s->vsync <= 3 ? s->vsync : 1; return 1; }
  if (is(item, "video.frame_blend")) { *out = s->frame_blend != 0; return 1; }
  if (is(item, "video.run_ahead")) { *out = s->run_ahead; return 1; }
  if (is(item, RECOMP_RUNTIME_UI_KEY_FULLSCREEN)) { *out = s->fullscreen; return 1; }
  if (is(item, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE)) { *out = s->window_scale; return 1; }
  if (is(item, "display.stretch")) { *out = s->ignore_aspect != 0; return 1; }
  if (is(item, RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER)) { *out = s->linear_filter != 0; return 1; }
  if (is(item, RECOMP_RUNTIME_UI_KEY_VOLUME)) { *out = s->volume; return 1; }
  if (is(item, "audio.rate")) {
    int want = g_rate_pending >= 0 ? g_rate_pending : s->audio_freq;
    *out = kAudioRateValues[0];
    for (int i = 0; i < 4; i++)
      if (kAudioRateValues[i] == want) *out = want;
    return 1;
  }
  if (is(item, "controls.p1")) { *out = s->player_src[0]; return 1; }
  if (is(item, "controls.p2")) { *out = s->player_src[1]; return 1; }
  if (is(item, "controls.deadzone")) { *out = s->deadzone[0]; return 1; }
  if (is(item, "system.skip_launcher")) { *out = s->skip_launcher != 0; return 1; }
#endif
  return 0;
}

static int ui_set(void *ctx, const RecompRuntimeUiItem *item, int value) {
  (void)ctx;
  if (is(item, "graphics.shader_preset")) {
    if (g_accepting) {   /* Enter applies the pick (or the shown preset) */
      apply_shader(g_shader_pending >= 0 ? g_shader_pending : g_current);
      return 0;
    }
    if (value < 0 || (size_t)value >= g_count) return 0;
    g_shader_pending = value;
    return 0;  /* nothing saved yet: no "Saved" toast */
  }
  if (is(item, "graphics.widescreen")) {
    if (value < 0 || value > 4) return 0;
    if (!DcWidescreenSetting_Set(value))
      fprintf(stderr, "[dc] widescreen: the mod state could not be saved\n");
    status("Widescreen: %s%s", kWidescreenLabels[value], "");
    return 0;
  }
  if (is(item, "graphics.motion")) {
    if (value < 0 || value > 2) return 0;
    DcInterp_SetMode((DcMotionMode)value);
    snesrecomp_desktop_request_clock_reset();
    save_choice();
    status("120Hz motion: %s%s", kMotionLabels[value],
           value ? " (active on 100 Hz+ displays)" : "");
    return 0;
  }
  if (is(item, RECOMP_RUNTIME_UI_KEY_STATE_SLOT)) {
    if (value < 1 || value > DC_STATE_SLOTS) return 0;
    g_slot = value;
    save_choice();
    return 1;
  }
#if defined(RECOMP_LAUNCHER)
  RecompLauncherCSettings *s = ls();
  if (is(item, "video.renderer")) {
    if (g_accepting) {
      int want = g_renderer_pending >= 0 ? g_renderer_pending : s->renderer;
      g_renderer_pending = -1;
      if (want == s->renderer) return 0;
      s->renderer = want;
      status("Renderer: %s (restarting, resumes here)%s",
             g_renderer_labels && want < g_renderer_count ? g_renderer_labels[want] : "", "");
      ls_apply();
      return 0;
    }
    if (value < 0 || value >= g_renderer_count) return 0;
    g_renderer_pending = value;
    return 0;
  }
  if (is(item, "audio.rate")) {
    if (g_accepting) {
      int want = g_rate_pending >= 0 ? g_rate_pending : s->audio_freq;
      g_rate_pending = -1;
      if (want == s->audio_freq) return 0;
      s->audio_freq = want;
      status("Audio rate changed (restarting, resumes here)%s%s", "", "");
      ls_apply();
      return 0;
    }
    g_rate_pending = value;
    return 0;
  }
  if (is(item, "video.vsync")) s->vsync = value;
  else if (is(item, "video.frame_blend")) s->frame_blend = value;
  else if (is(item, "video.run_ahead")) s->run_ahead = value;
  else if (is(item, RECOMP_RUNTIME_UI_KEY_FULLSCREEN)) s->fullscreen = value;
  else if (is(item, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE)) s->window_scale = value;
  else if (is(item, "display.stretch")) s->ignore_aspect = value;
  else if (is(item, RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER)) s->linear_filter = value;
  else if (is(item, RECOMP_RUNTIME_UI_KEY_VOLUME)) s->volume = value;
  else if (is(item, "controls.p1")) s->player_src[0] = value;
  else if (is(item, "controls.p2")) s->player_src[1] = value;
  else if (is(item, "controls.deadzone")) s->deadzone[0] = s->deadzone[1] = value;
  else if (is(item, "system.skip_launcher")) s->skip_launcher = value;
  else return 0;
  ls_apply();
  return 1;
#else
  return 0;
#endif
}

static int ui_action(void *ctx, const RecompRuntimeUiItem *item) {
  (void)ctx;
  char n[16];
  snprintf(n, sizeof(n), "%d", g_slot);
  if (is(item, RECOMP_RUNTIME_UI_KEY_RESUME)) {
    recomp_runtime_ui_close(g_ui);
    return 1;
  }
  if (is(item, RECOMP_RUNTIME_UI_KEY_SAVE_STATE)) {
    snesrecomp_desktop_command(kKeys_Save + g_slot - 1);
    status("Saved state to slot %s%s", n, "");
    return 0;
  }
  if (is(item, RECOMP_RUNTIME_UI_KEY_LOAD_STATE)) {
    char path[512];
    struct stat st;
    RtlSaveSlotPath(g_slot - 1, path, sizeof(path));
    if (stat(path, &st) != 0) {
      status("Slot %s is empty%s", n, "");
      return 0;
    }
    snesrecomp_desktop_command(kKeys_Load + g_slot - 1);
    fprintf(stderr, "[dc] options: loaded state slot %d\n", g_slot);
    recomp_runtime_ui_close(g_ui);
    return 1;
  }
  if (is(item, "controls.bindings")) {
    recomp_runtime_ui_close(g_ui);
    snesrecomp_desktop_open_launcher();
    return 1;
  }
  if (is(item, RECOMP_RUNTIME_UI_KEY_RESET)) {
    if (!recomp_runtime_ui_confirm(g_ui, item, "Press again to reset the console")) return 0;
    recomp_runtime_ui_close(g_ui);
    snesrecomp_desktop_command(kKeys_Reset);
    return 1;
  }
  if (is(item, RECOMP_RUNTIME_UI_KEY_QUIT)) {
    if (!recomp_runtime_ui_confirm(g_ui, item, "Press again to quit")) return 0;
    SDL_Event e;
    memset(&e, 0, sizeof(e));
#if SNESRECOMP_SDL3
    e.type = SDL_EVENT_QUIT;
#else
    e.type = SDL_QUIT;
#endif
    SDL_PushEvent(&e);
    return 1;
  }
  return 0;
}

static int ui_enabled(void *ctx, const RecompRuntimeUiItem *item) {
  (void)ctx;
  /* Every row stays live: on a renderer that cannot run shaders the Shader
   * row says why and offers the switch (describe_shader). */
  if (is(item, "graphics.shader_preset")) return g_count > 1;
  return 1;
}

static void ui_visibility(void *ctx, int open) {
  (void)ctx;
#if defined(RECOMP_LAUNCHER)
  g_ls_valid = false;          /* re-seed from the live config */
  g_renderer_pending = g_rate_pending = -1;
#endif
  if (!open) g_shader_pending = -1;
}

#define ITEM_COUNT 32
static RecompRuntimeUiItem g_items[ITEM_COUNT];
static size_t g_item_count;

static RecompRuntimeUiItem *add(const char *key, const char *section, const char *label,
                                const char *desc, RecompRuntimeUiItemType type) {
  RecompRuntimeUiItem *it = &g_items[g_item_count++];
  memset(it, 0, sizeof(*it));
  it->key = key;
  it->section = section;
  it->label = label;
  it->description = desc;
  it->type = type;
  it->step = 1;
  return it;
}

static void choices(RecompRuntimeUiItem *it, const char *const *labels, size_t n,
                    const int *values, int minimum) {
  it->type = RECOMP_RUNTIME_UI_CHOICE;
  it->choices = labels;
  it->choice_count = n;
  it->choice_values = values;
  it->minimum = minimum;
  it->maximum = minimum + (int)n - 1;
}

static void build_items(void) {
  RecompRuntimeUiItem *it;
  /* Video */
  it = add("graphics.shader_preset", "Video", "Shader", g_shader_desc, RECOMP_RUNTIME_UI_CHOICE);
  choices(it, (const char *const *)g_labels, g_count, NULL, 0);
  it = add("graphics.widescreen", "Video", "Widescreen",
           "Show the stage beyond the 256-column view. Adaptive fits the window; "
           "16:9, 21:9 and 32:9 are fixed. Applies now; same switch as the "
           "launcher's Widescreen row and Mods screen.", RECOMP_RUNTIME_UI_CHOICE);
  choices(it, kWidescreenLabels, 5, NULL, 0);
  it = add("graphics.motion", "Video", "120Hz motion",
           "120Hz output, 60Hz logic (100 Hz+ displays)", RECOMP_RUNTIME_UI_CHOICE);
  choices(it, kMotionLabels, 3, NULL, 0);
#if defined(RECOMP_LAUNCHER)
  g_renderer_labels = snesrecomp_desktop_renderer_labels(&g_renderer_count);
  if (g_renderer_labels && g_renderer_count > 0) {
    it = add("video.renderer", "Video", "Renderer", g_renderer_desc, RECOMP_RUNTIME_UI_CHOICE);
    choices(it, g_renderer_labels, (size_t)g_renderer_count, NULL, 0);
  }
  it = add("video.vsync", "Video", "VSync",
           "On: tear-free. Off: lowest latency. Adaptive: vsync above the "
           "refresh rate, immediate below.", RECOMP_RUNTIME_UI_CHOICE);
  choices(it, kVsyncLabels, 3, kVsyncValues, 1);
  add("video.frame_blend", "Video", "Frame blend",
      "Blend consecutive frames (flicker transparency)", RECOMP_RUNTIME_UI_BOOL);
  it = add("video.run_ahead", "Video", "Run-ahead",
           "Frames of input latency to hide (0 = off)", RECOMP_RUNTIME_UI_INT);
  it->minimum = 0;
  it->maximum = 4;
#endif
  /* Save states, right after Video */
  it = add(RECOMP_RUNTIME_UI_KEY_STATE_SLOT, "Save states", "Slot", g_slot_desc,
           RECOMP_RUNTIME_UI_INT);
  it->minimum = 1;
  it->maximum = DC_STATE_SLOTS;
  add(RECOMP_RUNTIME_UI_KEY_SAVE_STATE, "Save states", "Save State",
      "Save this moment to the selected slot", RECOMP_RUNTIME_UI_ACTION);
  add(RECOMP_RUNTIME_UI_KEY_LOAD_STATE, "Save states", "Load State",
      "Return to the state in the selected slot", RECOMP_RUNTIME_UI_ACTION);
#if defined(RECOMP_LAUNCHER)
  /* Display */
  it = add(RECOMP_RUNTIME_UI_KEY_FULLSCREEN, "Display", "Fullscreen", NULL,
           RECOMP_RUNTIME_UI_CHOICE);
  choices(it, kFullscreenLabels, 3, NULL, 0);
  it = add(RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE, "Display", "Window scale", NULL,
           RECOMP_RUNTIME_UI_INT);
  it->minimum = 1;
  it->maximum = 10;
  add("display.stretch", "Display", "Stretch to window",
      "Ignore the aspect ratio and fill the window", RECOMP_RUNTIME_UI_BOOL);
  add(RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER, "Display", "Linear filter",
      "Smooth scaling (off: sharp pixels)", RECOMP_RUNTIME_UI_BOOL);
  /* Audio */
  it = add(RECOMP_RUNTIME_UI_KEY_VOLUME, "Audio", "Volume", NULL, RECOMP_RUNTIME_UI_INT);
  it->minimum = 0;
  it->maximum = 100;
  it->step = 5;
  it = add("audio.rate", "Audio", "Sample rate",
           "Needs a restart: Enter restarts and resumes where you are.",
           RECOMP_RUNTIME_UI_CHOICE);
  choices(it, kAudioRateLabels, 4, kAudioRateValues, kAudioRateValues[0]);
  /* Controls */
  it = add("controls.p1", "Controls", "Player 1 input", NULL, RECOMP_RUNTIME_UI_CHOICE);
  choices(it, kSourceLabels, 3, NULL, 0);
  it = add("controls.p2", "Controls", "Player 2 input", NULL, RECOMP_RUNTIME_UI_CHOICE);
  choices(it, kSourceLabels, 3, NULL, 0);
  it = add("controls.deadzone", "Controls", "Stick deadzone", "Percent", RECOMP_RUNTIME_UI_INT);
  it->minimum = 0;
  it->maximum = 50;
  it->step = 5;
  add("controls.bindings", "Controls", "Keybinds & controller mapping",
      "Opens the launcher's controller page; Play returns here.",
      RECOMP_RUNTIME_UI_ACTION);
#endif
  /* System */
  add(RECOMP_RUNTIME_UI_KEY_RESUME, "System", "Resume", NULL, RECOMP_RUNTIME_UI_ACTION);
#if defined(RECOMP_LAUNCHER)
  add("system.skip_launcher", "System", "Skip launcher at startup", NULL,
      RECOMP_RUNTIME_UI_BOOL);
#endif
  add(RECOMP_RUNTIME_UI_KEY_RESET, "System", "Reset console", NULL, RECOMP_RUNTIME_UI_ACTION);
  add(RECOMP_RUNTIME_UI_KEY_QUIT, "System", "Quit", NULL, RECOMP_RUNTIME_UI_ACTION);
}

/* shaders/slang/manifest.txt (same format as N64 Recomp Magical Tetris:
 * "id | path relative to shaders/slang | display name | license"), in its
 * order. Returns false (and the caller scans the folder) without one. */
static char *trim(char *t) {
  while (*t == ' ' || *t == '\t') t++;
  size_t n = strlen(t);
  while (n && (t[n - 1] == ' ' || t[n - 1] == '\t' || t[n - 1] == '\r' || t[n - 1] == '\n')) t[--n] = 0;
  return t;
}
static bool add_slang_manifest(void) {
  char root[1024], path[1200];
  resolve_root(DC_SLANG_ROOT, root, sizeof(root));
  snprintf(path, sizeof(path), "%s/manifest.txt", root);
  FILE *f = fopen(path, "r");
  if (!f) return false;
  char line[1400];
  size_t n = 0;
  while (fgets(line, sizeof(line), f) && g_count <= DC_MAX_PRESETS) {
    if (line[0] == '#') continue;
    char *fld[4] = {0};
    char *p = line;
    int k = 0;
    for (; k < 4 && p; k++) {
      fld[k] = p;
      p = strchr(p, '|');
      if (p) *p++ = 0;
    }
    if (k < 3 || !fld[2]) continue;
    char *id = trim(fld[0]), *rel = trim(fld[1]), *name = trim(fld[2]);
    if (!id[0] || !rel[0]) continue;
    char full[1600];
    snprintf(full, sizeof(full), "%s/%s", root, rel);
    struct stat st;
    if (stat(full, &st) != 0) continue;
    g_paths[g_count] = strdup(full);
    g_ids[g_count] = join("slang/", id);
    g_labels[g_count] = strdup(name[0] ? name : id);
    g_kinds[g_count] = kKindSlang;
    g_count++;
    n++;
  }
  fclose(f);
  fprintf(stderr, "[dc] %zu .slangp presets from %s\n", n, path);
  return n > 0;
}

void DcOptions_Init(void) {
  if (g_initialized) return;
  g_initialized = true;
  g_paths[0] = NULL;
  g_labels[0] = strdup("Off");
  g_ids[0] = strdup("");
  g_kinds[0] = kKindOff;
  g_count = 1;
  if (!add_slang_manifest())
    add_presets(DC_SLANG_DIR, ".slangp", kKindSlang, "slang/", "");
  add_presets(DC_GLSL_DIR, ".glslp", kKindGlsl, "glsl/", "GLSL ");
  load_saved();
  fprintf(stderr, "[dc] %zu shader presets; startup shader=%s motion=%s slot=%d\n",
          g_count - 1, g_current ? g_ids[g_current] : "Off",
          kMotionKeys[DcInterp_GetMode()], g_slot);
  describe_slot();
  snprintf(g_shader_desc, sizeof(g_shader_desc), "%s", DC_SHADER_NOTE);
  build_items();

  static RecompRuntimeUiConfig cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.title = "Options";
  cfg.subtitle = "Demon's Crest";
  cfg.items = g_items;
  cfg.item_count = g_item_count;
  cfg.theme = "snes";
  cfg.callbacks.get_value = ui_get;
  cfg.callbacks.set_value = ui_set;
  cfg.callbacks.run_action = ui_action;
  cfg.callbacks.is_enabled = ui_enabled;
  cfg.callbacks.visibility_changed = ui_visibility;
  g_ui = recomp_runtime_ui_create(&cfg);
}

void DcOptions_AfterConfig(void) {
  DcOptions_Init();
  DcLibra_Install();
  /* The preset is chosen only in the in-game menu: config.ini's Shader key
   * (and the launcher) never decide it. */
  g_config.shader = NULL;
  if (g_current > 0 && g_kinds[g_current] == kKindGlsl) {
    static char path[1100];
    snprintf(path, sizeof(path), "%s", g_paths[g_current]);
    g_config.shader = path;
  } else if (g_current > 0) {
    DcLibra_SetPreset(g_paths[g_current]);
    g_shader_waiting = g_current;
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
  if (in == RECOMP_RUNTIME_UI_INPUT_ACCEPT) g_accepting = true;
  recomp_runtime_ui_handle_input(g_ui, in, pressed, repeat);
  g_accepting = false;
  return 1;
}

int DcOptions_OnKey(int key, int mod, int pressed, int repeat) {
  DcOptions_Init();
  if (!g_ui) return 0;
  /* Plain F12 opens the menu. Shift+F12 stays the host's Rewind. Escape
   * toggles it too: in DC's single-player loop Escape had no binding. */
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

/* Test hooks (headless, no input device):
 *   DC_OPTIONS_OPEN_AT=<sim frame>  open the menu at that frame
 *   DC_OPTIONS_CLOSE_AFTER=<n>      close it again after n modal presents
 *   DC_OPTIONS_SELECT=<id>          while open, apply that shader preset */
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
    int i = sel ? find_id(sel) : -1;
    if (i >= 0) apply_shader(i);
  }
}

int DcOptions_ModalActive(void) {
  poll_shader();
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
