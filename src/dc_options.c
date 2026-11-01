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
 * v1.1.0: a side panel drawn at window resolution (host draw_modal_layer)
 * over the live, shaded game: tabs Video / Display / Audio / Controls /
 * Shaders / System, edits are pending until Apply, shaders preview live.
 * Input is read here ONLY while the panel is open; closed, every key and
 * button goes to the game exactly as before.
 */
#include "dc_options.h"
#include "dc_interp.h"
#include "dc_libra.h"
#include "dc_widescreen.h"
#include "ui/rpanel.h"
#include <math.h>
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

/* ---- shader apply / preview --------------------------------------------- */

static char g_status[256];
static double g_status_at;
static int g_dirty = 1;
static int g_preview = -1;           /* shown live behind the panel, not saved */
static int g_preview_waiting = -1;

static double now_s(void) { return (double)SDL_GetTicks() / 1000.0; }

static void status(const char *fmt, const char *a, const char *b) {
  snprintf(g_status, sizeof(g_status), fmt, a ? a : "", b ? b : "");
  g_status_at = now_s();
  g_dirty = 1;
  fprintf(stderr, "[dc] options: %s\n", g_status);
}

static const char *renderer_now(void) {
  switch (DcLibra_Backend()) {
  case DC_LIBRA_GL: return "OpenGL";
  case DC_LIBRA_VULKAN: return "Vulkan";
  default: return NULL;
  }
}

/* Can `i` run on the live renderer without a restart? */
static bool shader_live_ok(int i) {
  if (i <= 0) return true;
  DcLibraBackend be = DcLibra_Backend();
  if (be == DC_LIBRA_NONE) return false;
  if (g_kinds[i] == kKindGlsl && be == DC_LIBRA_VULKAN) return false;
  return true;
}

/* Put preset `want` on screen. save: also make it the saved choice. */
static void run_shader(int want, bool save) {
  if (want < 0 || (size_t)want >= g_count) return;
  PresetKind kind = g_kinds[want];
  if (kind != kKindGlsl && OpenGLRenderer_IsActive()) OpenGLRenderer_SetShader(NULL);
  if (kind != kKindSlang) DcLibra_SetPreset(NULL);
  if (want == 0) {
    g_config.shader = NULL;
    if (save) { g_current = 0; save_choice(); status("Shader: Off%s%s", "", ""); }
    return;
  }
  if (kind == kKindGlsl) {
    if (OpenGLRenderer_SetShader(g_paths[want])) {
      static char path[1100];
      snprintf(path, sizeof(path), "%s", g_paths[want]);
      g_config.shader = path;
      if (save) { g_current = want; save_choice(); status("Shader: %s%s", g_labels[want], ""); }
    } else {
      status("Failed: %s%s", g_labels[want], "");
    }
    return;
  }
  g_config.shader = NULL;
  DcLibra_SetPreset(g_paths[want]);
  if (save) { g_shader_waiting = want; status("Loading %s...%s", g_labels[want], ""); }
  else g_preview_waiting = want;
}

/* Commit (Apply). */
static void apply_shader(int want) {
  g_shader_pending = -1;
  if (want < 0 || (size_t)want >= g_count) return;
  if (want > 0 && !shader_live_ok(want)) {
    g_current = want;
    save_choice();
    status("Shader %s: switching to OpenGL (restart, resumes here)%s", g_labels[want], "");
    snesrecomp_desktop_request_renderer_restart("opengl");
    return;
  }
  g_preview = want;
  run_shader(want, true);
}

static void preview_shader(int want) {
  if (want == g_preview) return;
  if (!shader_live_ok(want)) {
    status("%s needs OpenGL: Apply switches the renderer (restart)%s", g_labels[want], "");
    return;
  }
  g_preview = want;
  run_shader(want, false);
  if (want > 0) status("Previewing %s%s", g_labels[want], "");
}

static void poll_shader(void) {
  char msg[256];
  int o = DcLibra_Poll(msg, sizeof(msg));
  if (!o) return;
  if (g_shader_waiting >= 0) {
    int want = g_shader_waiting;
    g_shader_waiting = -1;
    if (o > 0) {
      g_current = want;
      save_choice();
      status("Shader: %s (%s)", g_labels[want], msg);
    } else {
      status("Failed: %s: %s", g_labels[want], msg);
    }
  } else if (g_preview_waiting >= 0) {
    int want = g_preview_waiting;
    g_preview_waiting = -1;
    if (o > 0) status("Previewing %s (%s)", g_labels[want], msg);
    else status("Failed: %s: %s", g_labels[want], msg);
  }
}

/* ---- rows ---------------------------------------------------------------- */

enum { T_VIDEO, T_DISPLAY, T_AUDIO, T_CONTROLS, T_SHADERS, T_SYSTEM, T_COUNT };
static const char *const kTabNames[T_COUNT] = {"Video", "Display", "Audio", "Controls",
                                               "Shaders", "System"};
typedef enum { R_PICK, R_TOGGLE, R_SLIDER, R_ACTION, R_SHADER, R_NOTE } RowType;
typedef struct Row {
  const char *key, *label, *desc;
  RowType type;
  int tab;
  const char *const *choices;
  int nchoices;
  const int *values;      /* pick: stored value per choice (NULL: min + index) */
  int min, max, step;
  const char *unit;
  int cur, pend;          /* committed and edited values */
} Row;
#define DC_MAX_ROWS 40
static Row g_rows[DC_MAX_ROWS];
static int g_nrows;
static const char *const *g_renderer_labels;
static int g_renderer_count;
static const char *const kOnOff[] = {"Off", "On"};

static Row *row(const char *key, int tab, const char *label, const char *desc, RowType t) {
  Row *r = &g_rows[g_nrows++];
  memset(r, 0, sizeof(*r));
  r->key = key; r->tab = tab; r->label = label; r->desc = desc; r->type = t;
  r->step = 1;
  return r;
}
static void pick(Row *r, const char *const *labels, int n, const int *values, int min) {
  r->choices = labels; r->nchoices = n; r->values = values; r->min = min;
  r->max = min + n - 1;
}
static bool key_is(const Row *r, const char *k) { return r->key && strcmp(r->key, k) == 0; }

static int pick_index(const Row *r, int v) {
  if (r->values) {
    for (int i = 0; i < r->nchoices; i++) if (r->values[i] == v) return i;
    return 0;
  }
  int i = v - r->min;
  return i < 0 ? 0 : i >= r->nchoices ? r->nchoices - 1 : i;
}
static int pick_value(const Row *r, int i) { return r->values ? r->values[i] : r->min + i; }

static void build_rows(void) {
  Row *r;
  g_nrows = 0;
#if defined(RECOMP_LAUNCHER)
  g_renderer_labels = snesrecomp_desktop_renderer_labels(&g_renderer_count);
  if (g_renderer_labels && g_renderer_count > 0) {
    r = row("video.renderer", T_VIDEO, "Renderer",
            "Restarts on Apply and resumes where you are. Shaders only work on the "
            "Vulkan and OpenGL renderers.", R_PICK);
    pick(r, g_renderer_labels, g_renderer_count, NULL, 0);
  }
  r = row("video.vsync", T_VIDEO, "VSync",
          "On: tear-free. Off: lowest latency. Adaptive: vsync above the refresh "
          "rate, immediate below.", R_PICK);
  pick(r, kVsyncLabels, 3, kVsyncValues, 1);
#endif
  r = row("graphics.motion", T_VIDEO, "120Hz motion",
          "120 Hz output with 60 Hz game logic. Needs a 100 Hz+ display.", R_PICK);
  pick(r, kMotionLabels, 3, NULL, 0);
  r = row("graphics.widescreen", T_VIDEO, "Widescreen",
          "Show the stage beyond the 256-column view. Adaptive fits the window.", R_PICK);
  pick(r, kWidescreenLabels, 5, NULL, 0);
#if defined(RECOMP_LAUNCHER)
  row("video.frame_blend", T_VIDEO, "Frame blend",
      "Blend consecutive frames (flicker transparency).", R_TOGGLE);
  r = row("video.run_ahead", T_VIDEO, "Run-ahead",
          "Frames of input latency to hide (0 = off).", R_SLIDER);
  r->min = 0; r->max = 4; r->unit = "";
  r = row("display.fullscreen", T_DISPLAY, "Fullscreen", "Window mode.", R_PICK);
  pick(r, kFullscreenLabels, 3, NULL, 0);
  r = row("display.scale", T_DISPLAY, "Window scale", "Window size in SNES pixels.", R_SLIDER);
  r->min = 1; r->max = 10; r->unit = "x";
  row("display.stretch", T_DISPLAY, "Stretch to window",
      "Ignore the aspect ratio and fill the window.", R_TOGGLE);
  row("display.linear", T_DISPLAY, "Linear filter",
      "Smooth scaling. Off keeps sharp pixels.", R_TOGGLE);
  r = row("audio.volume", T_AUDIO, "Volume", "Master volume.", R_SLIDER);
  r->min = 0; r->max = 100; r->step = 5; r->unit = "%";
  r = row("audio.rate", T_AUDIO, "Sample rate",
          "Restarts on Apply and resumes where you are.", R_PICK);
  pick(r, kAudioRateLabels, 4, kAudioRateValues, kAudioRateValues[0]);
  r = row("controls.p1", T_CONTROLS, "Player 1 input", "Which device drives player 1.", R_PICK);
  pick(r, kSourceLabels, 3, NULL, 0);
  r = row("controls.p2", T_CONTROLS, "Player 2 input", "Which device drives player 2.", R_PICK);
  pick(r, kSourceLabels, 3, NULL, 0);
  r = row("controls.deadzone", T_CONTROLS, "Stick deadzone",
          "How far the stick moves before it counts.", R_SLIDER);
  r->min = 0; r->max = 50; r->step = 5; r->unit = "%";
#endif
  row("controls.background", T_CONTROLS, "Background input",
      "Off: controller input is ignored while the window is not focused.", R_TOGGLE);
#if defined(RECOMP_LAUNCHER)
  row("controls.bindings", T_CONTROLS, "Keybinds & controller mapping",
      "Opens the launcher's controls page; Play returns here.", R_ACTION);
#endif
  row("graphics.shader_preset", T_SHADERS, "Shader",
      "Choose a preset: it previews live behind this panel, Apply keeps it.", R_SHADER);
  row(NULL, T_SHADERS, "Shaders only work on the Vulkan and OpenGL renderers.",
      NULL, R_NOTE);
  r = row("system.slot", T_SYSTEM, "Save state slot", "The slot Save and Load use.", R_SLIDER);
  r->min = 1; r->max = DC_STATE_SLOTS; r->unit = "";
  row("system.save", T_SYSTEM, "Save state", "Save this moment to the selected slot.", R_ACTION);
  row("system.load", T_SYSTEM, "Load state", "Return to the state in the selected slot.", R_ACTION);
#if defined(RECOMP_LAUNCHER)
  row("system.skip_launcher", T_SYSTEM, "Skip launcher at startup",
      "Start straight into the game.", R_TOGGLE);
#endif
  row("system.resume", T_SYSTEM, "Resume", "Close this panel.", R_ACTION);
  row("system.reset", T_SYSTEM, "Reset console", "Restart the game from power-on.", R_ACTION);
  row("system.quit", T_SYSTEM, "Quit", "Close Demon's Crest.", R_ACTION);
}

/* Committed value of a row. */
static int read_value(const Row *r) {
  if (key_is(r, "graphics.motion")) return (int)DcInterp_GetMode();
  if (key_is(r, "graphics.widescreen")) return DcWidescreenSetting_Get();
  if (key_is(r, "graphics.shader_preset")) return g_current;
  if (key_is(r, "controls.background")) return snesrecomp_desktop_background_input();
  if (key_is(r, "system.slot")) return g_slot;
#if defined(RECOMP_LAUNCHER)
  RecompLauncherCSettings *s = ls();
  if (key_is(r, "video.renderer")) return s->renderer;
  if (key_is(r, "video.vsync")) return s->vsync >= 1 && s->vsync <= 3 ? s->vsync : 1;
  if (key_is(r, "video.frame_blend")) return s->frame_blend != 0;
  if (key_is(r, "video.run_ahead")) return s->run_ahead;
  if (key_is(r, "display.fullscreen")) return s->fullscreen;
  if (key_is(r, "display.scale")) return s->window_scale;
  if (key_is(r, "display.stretch")) return s->ignore_aspect != 0;
  if (key_is(r, "display.linear")) return s->linear_filter != 0;
  if (key_is(r, "audio.volume")) return s->volume;
  if (key_is(r, "audio.rate")) {
    for (int i = 0; i < 4; i++) if (kAudioRateValues[i] == s->audio_freq) return s->audio_freq;
    return kAudioRateValues[2];
  }
  if (key_is(r, "controls.p1")) return s->player_src[0];
  if (key_is(r, "controls.p2")) return s->player_src[1];
  if (key_is(r, "controls.deadzone")) return s->deadzone[0];
  if (key_is(r, "system.skip_launcher")) return s->skip_launcher != 0;
#endif
  return 0;
}

static void seed_rows(void) {
#if defined(RECOMP_LAUNCHER)
  g_ls_valid = false;
#endif
  for (int i = 0; i < g_nrows; i++) {
    g_rows[i].cur = read_value(&g_rows[i]);
    g_rows[i].pend = g_rows[i].cur;
  }
}

static int pending_count(void) {
  int n = 0;
  for (int i = 0; i < g_nrows; i++)
    if (g_rows[i].type != R_ACTION && g_rows[i].type != R_NOTE && g_rows[i].pend != g_rows[i].cur) n++;
  return n;
}

static void close_menu(bool revert);

/* Apply: every edited row, the launcher's way. */
static void apply_all(void) {
  int changed = pending_count();
  if (!changed) { status("Nothing to apply%s%s", "", ""); return; }
  int shader_want = -1;
  bool ls_dirty = false;
#if defined(RECOMP_LAUNCHER)
  RecompLauncherCSettings *s = ls();
#endif
  for (int i = 0; i < g_nrows; i++) {
    Row *r = &g_rows[i];
    if (r->type == R_ACTION || r->type == R_NOTE || r->pend == r->cur) continue;
    int v = r->pend;
    if (key_is(r, "graphics.shader_preset")) { shader_want = v; continue; }
    if (key_is(r, "graphics.motion")) {
      DcInterp_SetMode((DcMotionMode)v);
      snesrecomp_desktop_request_clock_reset();
      save_choice();
    } else if (key_is(r, "graphics.widescreen")) {
      if (!DcWidescreenSetting_Set(v))
        fprintf(stderr, "[dc] widescreen: the mod state could not be saved\n");
    } else if (key_is(r, "controls.background")) {
      snesrecomp_desktop_set_background_input(v);
    } else if (key_is(r, "system.slot")) {
      g_slot = v;
      save_choice();
    }
#if defined(RECOMP_LAUNCHER)
    else {
      ls_dirty = true;
      if (key_is(r, "video.renderer")) s->renderer = v;
      else if (key_is(r, "video.vsync")) s->vsync = v;
      else if (key_is(r, "video.frame_blend")) s->frame_blend = v;
      else if (key_is(r, "video.run_ahead")) s->run_ahead = v;
      else if (key_is(r, "display.fullscreen")) s->fullscreen = v;
      else if (key_is(r, "display.scale")) s->window_scale = v;
      else if (key_is(r, "display.stretch")) s->ignore_aspect = v;
      else if (key_is(r, "display.linear")) s->linear_filter = v;
      else if (key_is(r, "audio.volume")) s->volume = v;
      else if (key_is(r, "audio.rate")) s->audio_freq = v;
      else if (key_is(r, "controls.p1")) s->player_src[0] = v;
      else if (key_is(r, "controls.p2")) s->player_src[1] = v;
      else if (key_is(r, "controls.deadzone")) s->deadzone[0] = s->deadzone[1] = v;
      else if (key_is(r, "system.skip_launcher")) s->skip_launcher = v;
    }
#endif
  }
  int restart = 0;
  if (shader_want >= 0) {
    /* Saved first, so a restart below comes back with it. */
    g_current = shader_want;
    save_choice();
  }
#if defined(RECOMP_LAUNCHER)
  if (ls_dirty) restart = ls_apply();
#else
  (void)ls_dirty;
#endif
  if (shader_want >= 0 && !restart) apply_shader(shader_want);
  seed_rows();
  if (restart) status("Applied: restarting (resumes where you are)%s%s", "", "");
  else if (shader_want < 0 || shader_live_ok(shader_want)) {
    char n[16];
    snprintf(n, sizeof(n), "%d", changed);
    status("Applied %s change%s", n, changed == 1 ? "" : "s");
  }
}

/* ---- the panel ----------------------------------------------------------- */

static bool g_open;
static int g_tab;
static int g_focus;            /* row index within the tab; == count: Apply */
static bool g_list;            /* the shader list is up */
static int g_list_sel, g_list_top;
static double g_list_moved_at;
static int g_confirm_row = -1;
static double g_confirm_at;
static int g_last_pad;         /* last input came from a controller */
static float g_hl_y = -1, g_tab_x = -1, g_tab_w;  /* animated */
static double g_opened_at;

static int tab_rows(int tab, int *idx) {
  int n = 0;
  for (int i = 0; i < g_nrows; i++)
    if (g_rows[i].tab == tab && g_rows[i].type != R_NOTE) { if (idx) idx[n] = i; n++; }
  return n;
}
static Row *focused_row(void) {
  int idx[DC_MAX_ROWS];
  int n = tab_rows(g_tab, idx);
  return g_focus < n ? &g_rows[idx[g_focus]] : NULL;
}

static void open_menu(void) {
  if (g_open) return;
  g_open = true;
  g_list = false;
  g_confirm_row = -1;
  seed_rows();
  g_preview = g_current;
  g_hl_y = -1; g_tab_x = -1;
  g_status[0] = 0;
  g_opened_at = now_s();
  g_dirty = 1;
  fprintf(stderr, "[dc] options menu opened\n");
}

static void close_menu(bool revert) {
  if (!g_open) return;
  g_open = false;
  g_list = false;
  if (revert && g_preview != g_current && g_shader_waiting < 0) {
    if (shader_live_ok(g_current)) run_shader(g_current, false);
    g_preview = g_current;
  }
  fprintf(stderr, "[dc] options menu closed\n");
}

static int shader_row_index(void) {
  for (int i = 0; i < g_nrows; i++) if (key_is(&g_rows[i], "graphics.shader_preset")) return i;
  return -1;
}

static void run_action(Row *r) {
  char n[16];
  snprintf(n, sizeof(n), "%d", g_slot);
  int ri = (int)(r - g_rows);
  if (key_is(r, "system.resume")) { close_menu(true); return; }
  if (key_is(r, "system.save")) {
    snesrecomp_desktop_command(kKeys_Save + g_slot - 1);
    status("Saved state to slot %s%s", n, "");
    return;
  }
  if (key_is(r, "system.load")) {
    char path[512];
    struct stat st;
    RtlSaveSlotPath(g_slot - 1, path, sizeof(path));
    if (stat(path, &st) != 0) { status("Slot %s is empty%s", n, ""); return; }
    snesrecomp_desktop_command(kKeys_Load + g_slot - 1);
    fprintf(stderr, "[dc] options: loaded state slot %d\n", g_slot);
    close_menu(true);
    return;
  }
#if defined(RECOMP_LAUNCHER)
  if (key_is(r, "controls.bindings")) {
    close_menu(true);
    snesrecomp_desktop_open_launcher();
    return;
  }
#endif
  if (key_is(r, "system.reset") || key_is(r, "system.quit")) {
    bool quit = key_is(r, "system.quit");
    if (g_confirm_row != ri || now_s() - g_confirm_at > 3.0) {
      g_confirm_row = ri;
      g_confirm_at = now_s();
      status(quit ? "Press again to quit%s%s" : "Press again to reset the console%s%s", "", "");
      return;
    }
    g_confirm_row = -1;
    close_menu(true);
    if (quit) {
      SDL_Event e;
      memset(&e, 0, sizeof(e));
#if SNESRECOMP_SDL3
      e.type = SDL_EVENT_QUIT;
#else
      e.type = SDL_QUIT;
#endif
      SDL_PushEvent(&e);
    } else {
      snesrecomp_desktop_command(kKeys_Reset);
    }
  }
}

static void step_value(Row *r, int dir, bool wrap) {
  if (r->type == R_TOGGLE) { r->pend = !r->pend; return; }
  if (r->type == R_PICK) {
    int i = pick_index(r, r->pend) + dir;
    if (i < 0) i = wrap ? r->nchoices - 1 : 0;
    if (i >= r->nchoices) i = wrap ? 0 : r->nchoices - 1;
    r->pend = pick_value(r, i);
    return;
  }
  if (r->type == R_SLIDER) {
    int v = r->pend + dir * r->step;
    if (v < r->min) v = r->min;
    if (v > r->max) v = r->max;
    r->pend = v;
    return;
  }
  if (r->type == R_SHADER) {
    int v = r->pend + dir;
    if (v < 0) v = (int)g_count - 1;
    if (v >= (int)g_count) v = 0;
    r->pend = v;
    preview_shader(v);
  }
}

typedef enum { IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_OK, IN_BACK, IN_APPLY,
               IN_TAB_PREV, IN_TAB_NEXT, IN_PAGE_UP, IN_PAGE_DOWN } MenuIn;

static void list_move(int d) {
  int v = g_list_sel + d;
  if (v < 0) v = 0;
  if (v >= (int)g_count) v = (int)g_count - 1;
  if (v != g_list_sel) { g_list_sel = v; g_list_moved_at = now_s(); }
}

static void menu_input(MenuIn in) {
  g_dirty = 1;
  if (g_list) {
    int si = shader_row_index();
    switch (in) {
    case IN_UP: list_move(-1); break;
    case IN_DOWN: list_move(1); break;
    case IN_LEFT: case IN_PAGE_UP: case IN_TAB_PREV: list_move(-10); break;
    case IN_RIGHT: case IN_PAGE_DOWN: case IN_TAB_NEXT: list_move(10); break;
    case IN_OK:
      if (si >= 0) g_rows[si].pend = g_list_sel;
      preview_shader(g_list_sel);
      g_list = false;
      break;
    case IN_BACK:
      g_list = false;
      if (si >= 0 && shader_live_ok(g_rows[si].pend)) preview_shader(g_rows[si].pend);
      break;
    case IN_APPLY:
      if (si >= 0) g_rows[si].pend = g_list_sel;
      g_list = false;
      apply_all();
      break;
    }
    return;
  }
  int n = tab_rows(g_tab, NULL);
  Row *r = focused_row();
  switch (in) {
  case IN_UP: g_focus = g_focus > 0 ? g_focus - 1 : n; break;
  case IN_DOWN: g_focus = g_focus < n ? g_focus + 1 : 0; break;
  case IN_TAB_PREV: g_tab = (g_tab + T_COUNT - 1) % T_COUNT; g_focus = 0; break;
  case IN_TAB_NEXT: g_tab = (g_tab + 1) % T_COUNT; g_focus = 0; break;
  case IN_PAGE_UP: g_focus = 0; break;
  case IN_PAGE_DOWN: g_focus = n; break;
  case IN_LEFT: if (r) step_value(r, -1, false); break;
  case IN_RIGHT: if (r) step_value(r, 1, false); break;
  case IN_OK:
    if (!r) { apply_all(); break; }
    if (r->type == R_ACTION) run_action(r);
    else if (r->type == R_SHADER) {
      g_list = true;
      g_list_sel = r->pend;
      g_list_top = -1;
      g_list_moved_at = 0;
    } else step_value(r, 1, true);
    break;
  case IN_BACK: close_menu(true); break;
  case IN_APPLY: apply_all(); break;
  }
}

/* ---- drawing ------------------------------------------------------------- */

/* Demon's Crest: gothic purple with a crimson edge. */
#define C_TEXT    0xFFF4EEF8u
#define C_DIM     0xFFB3A6C0u
#define C_FAINT   0x66FFFFFFu
#define C_ACCENT  0xFF8E4DFFu
#define C_ACCENT2 0xFFC23A5Au
#define C_TINT    0xFF140B1Cu

static uint32_t *g_layer;
static size_t g_layer_cap;
static int g_lw, g_lh, g_lx, g_ly;
static int g_last_out_w, g_last_out_h;
static uint32_t g_blur[64 * 64];
static int g_blur_w, g_blur_h;
static const uint32_t *g_blur_src;

static void make_blur(const SnesModalLayerView *v) {
  g_blur_w = g_blur_h = 0;
  if (!v->frame || v->frame_w <= 0 || v->frame_h <= 0) return;
  int bw = 48, bh = 42;
  float acc[48 * 42][3];
  for (int y = 0; y < bh; y++)
    for (int x = 0; x < bw; x++) {
      int x0 = x * v->frame_w / bw, x1 = (x + 1) * v->frame_w / bw;
      int y0 = y * v->frame_h / bh, y1 = (y + 1) * v->frame_h / bh;
      float r = 0, g = 0, b = 0;
      int n = 0;
      for (int yy = y0; yy < y1; yy += 2)
        for (int xx = x0; xx < x1; xx += 2) {
          uint32_t p = v->frame[(size_t)yy * v->frame_w + xx];
          r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++;
        }
      if (!n) n = 1;
      acc[y * bw + x][0] = r / n; acc[y * bw + x][1] = g / n; acc[y * bw + x][2] = b / n;
    }
  for (int pass = 0; pass < 2; pass++) {
    static float tmp[48 * 42][3];
    for (int y = 0; y < bh; y++)
      for (int x = 0; x < bw; x++) {
        float s[3] = {0, 0, 0};
        int n = 0;
        for (int dy = -2; dy <= 2; dy++)
          for (int dx = -2; dx <= 2; dx++) {
            int xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= bw || yy >= bh) continue;
            for (int k = 0; k < 3; k++) s[k] += acc[yy * bw + xx][k];
            n++;
          }
        for (int k = 0; k < 3; k++) tmp[y * bw + x][k] = s[k] / n;
      }
    memcpy(acc, tmp, sizeof(tmp));
  }
  for (int i = 0; i < bw * bh; i++)
    g_blur[i] = 0xFF000000u | ((uint32_t)acc[i][0] << 16) | ((uint32_t)acc[i][1] << 8) |
                (uint32_t)acc[i][2];
  g_blur_w = bw;
  g_blur_h = bh;
}

static uint32_t blur_at(const SnesModalLayerView *v, float sx, float sy) {
  if (!g_blur_w || v->vp_w <= 0 || v->vp_h <= 0) return 0xFF000000u;
  float u = (sx - v->vp_x) / v->vp_w * g_blur_w - .5f;
  float t = (sy - v->vp_y) / v->vp_h * g_blur_h - .5f;
  if (u < -1 || t < -1 || u > g_blur_w || t > g_blur_h) return 0xFF000000u;
  if (u < 0) u = 0; if (t < 0) t = 0;
  if (u > g_blur_w - 1) u = (float)g_blur_w - 1;
  if (t > g_blur_h - 1) t = (float)g_blur_h - 1;
  int x0 = (int)u, y0 = (int)t;
  int x1 = x0 + 1 < g_blur_w ? x0 + 1 : x0, y1 = y0 + 1 < g_blur_h ? y0 + 1 : y0;
  float fx = u - x0, fy = t - y0;
  uint32_t a = rp_mix(g_blur[y0 * g_blur_w + x0], g_blur[y0 * g_blur_w + x1], fx);
  uint32_t b = rp_mix(g_blur[y1 * g_blur_w + x0], g_blur[y1 * g_blur_w + x1], fx);
  return rp_mix(a, b, fy);
}

/* The frosted panel body: the blurred game, darkened and tinted. */
static void draw_glass(RpCanvas *c, const SnesModalLayerView *v, float rad) {
  float hw = c->w / 2.f, hh = c->h / 2.f;
  for (int y = 0; y < c->h; y++) {
    uint32_t rowcol = 0;
    int last_bx = -99;
    for (int x = 0; x < c->w; x++) {
      float fx = x + .5f, fy = y + .5f;
      float qx = fabsf(fx - hw) - (hw - rad), qy = fabsf(fy - hh) - (hh - rad);
      float d;
      if (qx <= 0 || qy <= 0) d = (qx > qy ? qx : qy) - rad;
      else d = sqrtf(qx * qx + qy * qy) - rad;
      float cov = .5f - d;
      if (cov <= 0) { c->px[(size_t)y * c->w + x] = 0; continue; }
      if (cov > 1) cov = 1;
      int bx = x / 6;
      if (bx != last_bx) {
        uint32_t g = blur_at(v, (float)(g_lx + x), (float)(g_ly + y));
        rowcol = rp_mix(g, C_TINT, 0.74f);
        last_bx = bx;
      }
      float a = 0.93f * cov;
      uint32_t A = (uint32_t)(a * 255.f + .5f);
      uint32_t r = (uint32_t)(((rowcol >> 16) & 255) * a), gg = (uint32_t)(((rowcol >> 8) & 255) * a),
               b = (uint32_t)((rowcol & 255) * a);
      c->px[(size_t)y * c->w + x] = (A << 24) | (r << 16) | (gg << 8) | b;
    }
  }
  rp_stroke_rrect(c, .5f, .5f, c->w - 1.f, c->h - 1.f, rad, 1.2f, 0x30FFFFFFu);
}

/* Controller glyph style: 0 Xbox, 1 PlayStation, 2 Nintendo. */
static int pad_style(void) {
#if SNESRECOMP_SDL3
  int n = 0;
  SDL_JoystickID *ids = SDL_GetGamepads(&n);
  int st = 0;
  if (ids && n > 0) {
    SDL_GamepadType t = SDL_GetGamepadTypeForID(ids[0]);
    if (t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5) st = 1;
    else if (t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ||
             t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
             t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT ||
             t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) st = 2;
  }
  SDL_free(ids);
  return st;
#else
  return 0;
#endif
}

enum { G_SOUTH, G_EAST, G_WEST, G_LB, G_RB };

/* One face/shoulder button glyph; returns its width. */
static float draw_glyph(RpCanvas *c, int style, int which, float x, float cy, float s) {
  float r = 13 * s;
  if (which == G_LB || which == G_RB) {
    const char *t = style == 1 ? (which == G_LB ? "L1" : "R1")
                  : style == 2 ? (which == G_LB ? "L" : "R") : (which == G_LB ? "LB" : "RB");
    float w = rp_text_width(RP_BOLD, 14 * s, t) + 16 * s;
    rp_fill_rrect(c, x, cy - r * .85f, w, r * 1.7f, 7 * s, 0xFF3A3346u);
    rp_text_center(c, RP_BOLD, 14 * s, x, cy - r * .85f, w, r * 1.7f, t, C_TEXT);
    return w;
  }
  float cx = x + r;
  if (style == 1) {
    rp_fill_circle(c, cx, cy, r, 0xFF2B2635u);
    float k = r * .45f;
    if (which == G_SOUTH) {
      rp_line(c, cx - k, cy - k, cx + k, cy + k, 2.4f * s, 0xFF7FA8FFu);
      rp_line(c, cx - k, cy + k, cx + k, cy - k, 2.4f * s, 0xFF7FA8FFu);
    } else if (which == G_EAST) {
      rp_stroke_circle(c, cx, cy, k * 1.1f, 2.4f * s, 0xFFFF6F7Fu);
    } else {
      rp_stroke_rrect(c, cx - k, cy - k, 2 * k, 2 * k, 1.5f * s, 2.4f * s, 0xFFF59BD8u);
    }
    return 2 * r;
  }
  const char *t;
  uint32_t col;
  if (style == 2) {   /* Nintendo labels by position */
    t = which == G_SOUTH ? "B" : which == G_EAST ? "A" : "Y";
    col = 0xFF5A5466u;
  } else {
    t = which == G_SOUTH ? "A" : which == G_EAST ? "B" : "X";
    col = which == G_SOUTH ? 0xFF3FA34Du : which == G_EAST ? 0xFFD0433Fu : 0xFF3B76D6u;
  }
  rp_fill_circle(c, cx, cy, r, col);
  rp_text_center(c, RP_BOLD, 15 * s, cx - r, cy - r, 2 * r, 2 * r, t, 0xFFFFFFFFu);
  return 2 * r;
}

static float draw_keycap(RpCanvas *c, const char *t, float x, float cy, float s) {
  float h = 24 * s;
  float w = rp_text_width(RP_BOLD, 13 * s, t) + 14 * s;
  if (w < h) w = h;
  rp_fill_rrect(c, x, cy - h / 2, w, h, 6 * s, 0xFF3A3346u);
  rp_stroke_rrect(c, x, cy - h / 2, w, h, 6 * s, 1.f, 0x40FFFFFFu);
  rp_text_center(c, RP_BOLD, 13 * s, x, cy - h / 2, w, h, t, C_TEXT);
  return w;
}

static void draw_hints(RpCanvas *c, float x, float y, float w, float h, float s) {
  rp_fill_rrect(c, x, y, w, h, 12 * s, 0x50000000u);
  float cy = y + h / 2, gap = 8 * s, sep = 18 * s, fs = 16 * s;
  struct { int g; const char *k; const char *k2; const char *label; } it[4] = {
    {G_SOUTH, "Enter", NULL, "Select"},
    {G_EAST, "Esc", NULL, "Back"},
    {G_WEST, "F5", NULL, "Apply"},
    {G_LB, "Q", "E", "Tab"},
  };
  int style = pad_style();
  /* Measure, then scale down to fit. */
  float total = 0;
  float widths[4];
  for (int pass = 0; pass < 2; pass++) {
    RpCanvas dummy = *c;
    dummy.cx0 = dummy.cy0 = 0; dummy.cx1 = dummy.cy1 = 0;  /* draw nothing */
    RpCanvas *dc = pass == 0 ? &dummy : c;
    float px = pass == 0 ? 0 : x + (w - total) / 2;
    for (int i = 0; i < 4; i++) {
      float start = px;
      if (g_last_pad) {
        px += draw_glyph(dc, style, it[i].g, px, cy, s);
        if (it[i].g == G_LB) px += 4 * s + draw_glyph(dc, style, G_RB, px + 4 * s, cy, s);
      } else {
        px += draw_keycap(dc, it[i].k, px, cy, s);
        if (it[i].k2) px += 4 * s + draw_keycap(dc, it[i].k2, px + 4 * s, cy, s);
      }
      px += gap;
      px += rp_text(dc, RP_REGULAR, fs, px, cy - fs / 2, it[i].label, C_DIM, 0);
      widths[i] = px - start;
      if (i < 3) px += sep;
    }
    if (pass == 0) total = px;
  }
  (void)widths;
}

static void wrap_text(RpCanvas *c, int font, float size, float x, float y, float w,
                      int max_lines, const char *s, uint32_t col) {
  char line[256];
  const char *p = s;
  for (int ln = 0; ln < max_lines && *p; ln++) {
    const char *best = NULL, *q = p;
    while (1) {
      const char *sp = strchr(q, ' ');
      const char *end = sp ? sp : q + strlen(q);
      size_t len = (size_t)(end - p);
      if (len >= sizeof(line)) len = sizeof(line) - 1;
      memcpy(line, p, len);
      line[len] = 0;
      if (rp_text_width(font, size, line) > w && best) break;
      best = end;
      if (!sp) break;
      q = sp + 1;
    }
    size_t len = (size_t)(best - p);
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = 0;
    bool last = ln == max_lines - 1 && *best;
    rp_text(c, font, size, x, y + ln * size * 1.3f, last ? p : line, col, w);
    p = *best ? best + 1 : best;
  }
}

static float approach(float cur, float target, float k) {
  if (cur < 0) return target;
  float d = target - cur;
  if (fabsf(d) < .5f) return target;
  return cur + d * k;
}

static bool g_animating;

static void draw_toggle(RpCanvas *c, float x, float cy, float s, int on, bool changed) {
  float w = 50 * s, h = 28 * s;
  rp_fill_rrect(c, x, cy - h / 2, w, h, h / 2, on ? C_ACCENT : 0x40FFFFFFu);
  if (changed) rp_stroke_rrect(c, x - 2 * s, cy - h / 2 - 2 * s, w + 4 * s, h + 4 * s, h / 2 + 2 * s, 1.5f * s, C_ACCENT2);
  float kr = h / 2 - 3.5f * s;
  rp_fill_circle(c, on ? x + w - h / 2 : x + h / 2, cy, kr, 0xFFFFFFFFu);
}

static void draw_row(RpCanvas *c, Row *r, float x, float y, float w, float h, float s,
                     bool focus) {
  float fs = 21 * s;
  float ty = y + (h - fs) / 2;
  float cy = y + h / 2;
  bool changed = r->type != R_ACTION && r->pend != r->cur;
  uint32_t lc = focus ? C_TEXT : 0xFFE0D8E8u;
  float right = x + w;
  float vw = 0;   /* width taken by the value on the right */
  char buf[64];
  if (r->type == R_TOGGLE) {
    vw = 50 * s;
    draw_toggle(c, right - vw, cy, s, r->pend, changed);
  } else if (r->type == R_SLIDER) {
    float tw = w * .30f;
    if (tw > 180 * s) tw = 180 * s;
    snprintf(buf, sizeof(buf), "%d%s", r->pend, r->unit ? r->unit : "");
    float nw = 52 * s;
    float tx = right - tw;
    float frac = r->max > r->min ? (float)(r->pend - r->min) / (r->max - r->min) : 0;
    rp_fill_rrect(c, tx, cy - 3 * s, tw, 6 * s, 3 * s, 0x40FFFFFFu);
    rp_fill_rrect(c, tx, cy - 3 * s, tw * frac > 6 * s ? tw * frac : 6 * s, 6 * s, 3 * s, C_ACCENT);
    rp_fill_circle(c, tx + tw * frac, cy, 9 * s, focus ? 0xFFFFFFFFu : 0xFFE6DDF2u);
    float numw = rp_text_width(RP_BOLD, fs * .9f, buf);
    rp_text(c, RP_BOLD, fs * .9f, tx - 14 * s - numw, y + (h - fs * .9f) / 2, buf,
            changed ? C_ACCENT2 : C_DIM, 0);
    vw = tw + 14 * s + (numw > nw ? numw : nw);
  } else if (r->type == R_PICK || r->type == R_SHADER) {
    const char *v;
    if (r->type == R_SHADER) v = r->pend >= 0 && (size_t)r->pend < g_count ? g_labels[r->pend] : "Off";
    else v = r->choices[pick_index(r, r->pend)];
    float maxv = w * .52f;
    float aw = 16 * s;
    float tw = rp_text_width(RP_BOLD, fs * .95f, v);
    if (tw > maxv) tw = maxv;
    uint32_t vc = changed ? 0xFFE7A1FFu : (focus ? C_TEXT : C_DIM);
    if (r->type == R_SHADER) {
      rp_text(c, RP_BOLD, fs * .95f, right - aw - tw - 6 * s, y + (h - fs * .95f) / 2, v, vc, maxv);
      float ax = right - aw * .6f;
      rp_line(c, ax - 5 * s, cy - 7 * s, ax + 2 * s, cy, 2.2f * s, focus ? C_ACCENT : C_DIM);
      rp_line(c, ax + 2 * s, cy, ax - 5 * s, cy + 7 * s, 2.2f * s, focus ? C_ACCENT : C_DIM);
      vw = aw + tw + 6 * s;
    } else {
      uint32_t ac = focus ? C_ACCENT : 0x50FFFFFFu;
      float ax = right - aw * .5f;
      rp_line(c, ax - 3 * s, cy - 6 * s, ax + 3 * s, cy, 2.2f * s, ac);
      rp_line(c, ax + 3 * s, cy, ax - 3 * s, cy + 6 * s, 2.2f * s, ac);
      float vx = right - aw - 6 * s - tw;
      rp_text(c, RP_BOLD, fs * .95f, vx, y + (h - fs * .95f) / 2, v, vc, maxv);
      float lx = vx - 6 * s - aw * .5f;
      rp_line(c, lx + 3 * s, cy - 6 * s, lx - 3 * s, cy, 2.2f * s, ac);
      rp_line(c, lx - 3 * s, cy, lx + 3 * s, cy + 6 * s, 2.2f * s, ac);
      vw = aw * 2 + tw + 12 * s;
    }
  } else if (r->type == R_ACTION) {
    float ax = right - 10 * s;
    if (g_confirm_row == (int)(r - g_rows) && now_s() - g_confirm_at < 3.0) {
      const char *t = "Press again";
      float tw = rp_text_width(RP_BOLD, fs * .85f, t);
      rp_text(c, RP_BOLD, fs * .85f, right - tw, y + (h - fs * .85f) / 2, t, C_ACCENT2, 0);
      vw = tw;
    } else {
      rp_line(c, ax - 5 * s, cy - 7 * s, ax + 2 * s, cy, 2.2f * s, focus ? C_ACCENT : 0x50FFFFFFu);
      rp_line(c, ax + 2 * s, cy, ax - 5 * s, cy + 7 * s, 2.2f * s, focus ? C_ACCENT : 0x50FFFFFFu);
      vw = 16 * s;
    }
  }
  float lw = w - vw - 14 * s;
  rp_text(c, focus ? RP_BOLD : RP_REGULAR, fs, x, ty, r->label, lc, lw > 20 * s ? lw : 20 * s);
}

static void render_panel(const SnesModalLayerView *v) {
  /* Scale with the window height (readable on a TV), and keep the panel to
   * about half the width on narrow windows. */
  float s = v->out_h / 920.f;
  if (640 * s > v->out_w * .56f) s = v->out_w * .56f / 640.f;
  if (s < .42f) s = .42f;
  float margin = 24 * s;
  int pw = (int)(640 * s), ph = (int)(v->out_h - 2 * margin);
  if (pw > v->out_w - 2 * (int)margin) pw = v->out_w - 2 * (int)margin;
  if (pw < 60) pw = 60;
  if (ph < 60) ph = 60;
  size_t need = (size_t)pw * ph;
  if (need > g_layer_cap) {
    free(g_layer);
    g_layer = (uint32_t *)malloc(need * 4);
    g_layer_cap = g_layer ? need : 0;
  }
  if (!g_layer) return;
  g_lw = pw; g_lh = ph;
  g_lx = v->out_w - (int)margin - pw;
  g_ly = (int)margin;
  RpCanvas cv = {g_layer, pw, ph, 0, 0, pw, ph};
  RpCanvas *c = &cv;
  float rad = 26 * s;
  draw_glass(c, v, rad);
  g_animating = false;

  float pad = 28 * s;
  float x = pad, w = pw - 2 * pad;
  float y = pad - 4 * s;
  /* Header */
  rp_text(c, RP_BOLD, 40 * s, x, y, "Options", C_TEXT, w * .6f);
  {
    const char *game = "DEMON'S CREST";
    float gs = 14 * s;
    float gw = rp_text_width(RP_BOLD, gs, game);
    rp_text(c, RP_BOLD, gs, x + w - gw, y + 16 * s, game, C_ACCENT2, w * .38f);
  }
  y += 50 * s;
  {
    bool live = DcLibra_Backend() != DC_LIBRA_NONE || OpenGLRenderer_IsActive();
    rp_fill_circle(c, x + 5 * s, y + 9 * s, 4.5f * s, live ? 0xFF5BE08Au : 0xFFE0B45Bu);
    char line[200];
    const char *rn = renderer_now();
    snprintf(line, sizeof(line), "Live preview%s%s", rn ? "  \xC2\xB7  " : "", rn ? rn : "");
    rp_text(c, RP_REGULAR, 17 * s, x + 16 * s, y, line, C_DIM, w - 16 * s);
  }
  y += 34 * s;
  /* Tabs */
  float th = 44 * s;
  rp_fill_rrect(c, x, y, w, th, 12 * s, 0x38000000u);
  float tabw = w / T_COUNT;
  float tfs = 17 * s;
  for (int i = 0; i < T_COUNT; i++) {
    float need_w = rp_text_width(RP_BOLD, tfs, kTabNames[i]);
    if (need_w > tabw - 8 * s) tfs = tfs * (tabw - 8 * s) / need_w;
  }
  float target = x + tabw * g_tab;
  g_tab_x = approach(g_tab_x, target, .38f);
  if (g_tab_x != target) g_animating = true;
  rp_fill_rrect(c, g_tab_x + 3 * s, y + 3 * s, tabw - 6 * s, th - 6 * s, 10 * s,
                g_list ? 0x60FFFFFFu & 0x40FFFFFF : rp_alpha(C_ACCENT, .85f));
  for (int i = 0; i < T_COUNT; i++)
    rp_text_center(c, i == g_tab ? RP_BOLD : RP_REGULAR, tfs, x + tabw * i, y, tabw, th,
                   kTabNames[i], i == g_tab ? 0xFFFFFFFFu : C_DIM);
  y += th + 18 * s;

  /* Footer layout from the bottom up. */
  float hint_h = 46 * s;
  float hint_y = ph - pad - hint_h + 6 * s;
  float ab_h = 56 * s;
  float ab_y = hint_y - 14 * s - ab_h;
  float desc_fs = 16 * s;
  float desc_h = desc_fs * 1.3f * 2 + desc_fs * 1.3f;   /* 2 lines + status */
  float desc_y = ab_y - 12 * s - desc_h;
  float rows_top = y, rows_bot = desc_y - 8 * s;
  float rh = 56 * s;

  if (g_list) {
    /* Shader list */
    int vis = (int)((rows_bot - rows_top) / (rh * .78f));
    if (vis < 1) vis = 1;
    float lh = (rows_bot - rows_top) / vis;
    if (g_list_top < 0) g_list_top = g_list_sel - vis / 2;
    if (g_list_sel < g_list_top) g_list_top = g_list_sel;
    if (g_list_sel >= g_list_top + vis) g_list_top = g_list_sel - vis + 1;
    if (g_list_top > (int)g_count - vis) g_list_top = (int)g_count - vis;
    if (g_list_top < 0) g_list_top = 0;
    int si = shader_row_index();
    rp_canvas_clip(c, 0, (int)rows_top, pw, (int)rows_bot);
    for (int i = 0; i < vis && g_list_top + i < (int)g_count; i++) {
      int k = g_list_top + i;
      float ry = rows_top + i * lh;
      bool f = k == g_list_sel;
      if (f) rp_fill_rrect(c, x - 10 * s, ry + 2 * s, w + 20 * s, lh - 4 * s, 12 * s, rp_alpha(C_ACCENT, .30f));
      bool chosen = si >= 0 && g_rows[si].pend == k;
      bool live = k == g_preview;
      float fsz = 19 * s;
      const char *lab = g_labels[k];
      rp_text(c, f ? RP_BOLD : RP_REGULAR, fsz, x + 4 * s, ry + (lh - fsz) / 2, lab,
              f ? C_TEXT : (shader_live_ok(k) ? 0xFFDCD3E6u : 0xFF8A8094u), w - 80 * s);
      if (chosen) rp_fill_circle(c, x + w - 10 * s, ry + lh / 2, 5 * s, C_ACCENT2);
      if (live && !chosen) rp_stroke_circle(c, x + w - 10 * s, ry + lh / 2, 5 * s, 1.5f * s, C_ACCENT);
    }
    rp_canvas_noclip(c);
    /* scrollbar */
    if ((int)g_count > vis) {
      float sh = (rows_bot - rows_top);
      float bh = sh * vis / g_count;
      if (bh < 20 * s) bh = 20 * s;
      float by = rows_top + (sh - bh) * g_list_top / (float)((int)g_count - vis);
      rp_fill_rrect(c, x + w + 12 * s, by, 4 * s, bh, 2 * s, 0x60FFFFFFu);
    }
    char d[200];
    snprintf(d, sizeof(d), "%d of %zu presets. Highlight to preview, A/Enter picks, "
             "Left/Right pages.", g_list_sel, g_count - 1);
    wrap_text(c, RP_REGULAR, desc_fs, x, desc_y, w, 2, d, C_DIM);
  } else {
    int idx[DC_MAX_ROWS];
    int n = tab_rows(g_tab, idx);
    if (g_focus > n) g_focus = n;
    /* Rows (+ a note) scroll to keep the focus in view. */
    float avail = rows_bot - rows_top;
    int vis = (int)(avail / rh);
    if (vis < 1) vis = 1;
    static int top;
    int f = g_focus < n ? g_focus : n - 1;
    if (f < top) top = f;
    if (f >= top + vis) top = f - vis + 1;
    if (top > n - vis) top = n - vis;
    if (top < 0) top = 0;
    rp_canvas_clip(c, 0, (int)rows_top, pw, (int)rows_bot);
    if (g_focus < n) {
      float target_y = rows_top + (g_focus - top) * rh;
      g_hl_y = approach(g_hl_y, target_y, .40f);
      if (g_hl_y != target_y) g_animating = true;
      rp_fill_rrect(c, x - 12 * s, g_hl_y + 3 * s, w + 24 * s, rh - 6 * s, 14 * s, rp_alpha(C_ACCENT, .26f));
      rp_fill_rrect(c, x - 12 * s, g_hl_y + 14 * s, 4 * s, rh - 28 * s, 2 * s, C_ACCENT2);
    } else {
      g_hl_y = -1;
    }
    float ry = rows_top;
    for (int i = top; i < n && i < top + vis; i++, ry += rh) {
      Row *r = &g_rows[idx[i]];
      draw_row(c, r, x + 4 * s, ry, w - 8 * s, rh, s, i == g_focus);
      if (i + 1 < n && i + 1 < top + vis && i + 1 != g_focus && i != g_focus)
        rp_fill_rrect(c, x, ry + rh - .5f * s, w, 1.f, 0, 0x14FFFFFFu);
    }
    /* Tab notes below the rows */
    for (int i = 0; i < g_nrows; i++)
      if (g_rows[i].tab == g_tab && g_rows[i].type == R_NOTE && ry + rh <= rows_bot + 1) {
        rp_fill_rrect(c, x, ry + 8 * s, w, rh - 16 * s, 10 * s, rp_alpha(C_ACCENT2, .16f));
        rp_text(c, RP_REGULAR, 16 * s, x + 12 * s, ry + (rh - 16 * s) / 2, g_rows[i].label, 0xFFF0C9D2u, w - 24 * s);
        ry += rh;
      }
    rp_canvas_noclip(c);
    if (n > vis) {
      float sh = avail;
      float bh = sh * vis / n;
      float by = rows_top + (sh - bh) * top / (float)(n - vis);
      rp_fill_rrect(c, x + w + 12 * s, by, 4 * s, bh, 2 * s, 0x60FFFFFFu);
    }
    Row *fr = focused_row();
    const char *d = fr && fr->desc ? fr->desc
                  : pending_count() ? "Apply keeps your changes. Back discards them."
                                    : "Everything is applied.";
    wrap_text(c, RP_REGULAR, desc_fs, x, desc_y, w, 2, d, C_DIM);
  }
  /* Status line */
  {
    char st[300];
    int pc = pending_count();
    if (g_status[0] && now_s() - g_status_at < 6.0) snprintf(st, sizeof(st), "%s", g_status);
    else if (pc) snprintf(st, sizeof(st), "%d change%s not applied", pc, pc == 1 ? "" : "s");
    else st[0] = 0;
    if (st[0]) rp_text(c, RP_BOLD, desc_fs, x, desc_y + desc_fs * 1.3f * 2, st,
                       pc && !(g_status[0] && now_s() - g_status_at < 6.0) ? 0xFFE7A1FFu : 0xFFF0C9D2u, w);
  }
  /* Apply */
  {
    int idxn = tab_rows(g_tab, NULL);
    bool f = !g_list && g_focus == idxn;
    int pc = pending_count();
    uint32_t col = pc ? C_ACCENT : rp_alpha(C_ACCENT, .45f);
    if (f) col = rp_mix(col, 0xFFFFFFFFu, .12f);
    rp_fill_rrect(c, x, ab_y, w, ab_h, 14 * s, col);
    if (f) rp_stroke_rrect(c, x - 3 * s, ab_y - 3 * s, w + 6 * s, ab_h + 6 * s, 16 * s, 2 * s, 0xE0FFFFFFu);
    char t[32];
    if (pc) snprintf(t, sizeof(t), "Apply (%d)", pc);
    else snprintf(t, sizeof(t), "Apply");
    rp_text_center(c, RP_BOLD, 22 * s, x, ab_y, w, ab_h, t, 0xFFFFFFFFu);
  }
  draw_hints(c, x, hint_y, w, hint_h, s);
  rp_finish(c);
}

/* ---- host entry points --------------------------------------------------- */

static bool g_initialized;

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
  build_rows();
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
  g_preview = g_current;
}

static int feed(MenuIn in, int pressed) {
  if (pressed) menu_input(in);
  return 1;
}

int DcOptions_OnKey(int key, int mod, int pressed, int repeat) {
  DcOptions_Init();
  (void)repeat;
  bool plain = !(mod & (KMOD_SHIFT | KMOD_CTRL | KMOD_ALT));
  /* Plain F12 toggles the panel. Shift+F12 stays the host's Rewind. Escape
   * opens it too, and inside it goes back (closing at the top level). */
  if (key == SDLK_F12 && plain) {
    if (pressed && !repeat) { g_last_pad = 0; if (g_open) close_menu(true); else open_menu(); }
    return 1;
  }
  if (key == SDLK_ESCAPE && plain) {
    if (pressed && !repeat) {
      g_last_pad = 0;
      if (!g_open) open_menu();
      else menu_input(IN_BACK);
      g_dirty = 1;
    }
    return 1;
  }
  if (!g_open) return 0;   /* gameplay keys untouched while closed */
  if (pressed) { g_last_pad = 0; g_dirty = 1; }
  switch (key) {
  case SDLK_UP: return feed(IN_UP, pressed);
  case SDLK_DOWN: return feed(IN_DOWN, pressed);
  case SDLK_LEFT: return feed(IN_LEFT, pressed);
  case SDLK_RIGHT: return feed(IN_RIGHT, pressed);
  case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return feed(IN_OK, pressed);
  case SDLK_BACKSPACE: return feed(IN_BACK, pressed);
  case SDLK_F5: return feed(IN_APPLY, pressed);
  case SDLK_Q: return feed(IN_TAB_PREV, pressed);
  case SDLK_E: return feed(IN_TAB_NEXT, pressed);
  case SDLK_TAB: return feed((mod & KMOD_SHIFT) ? IN_TAB_PREV : IN_TAB_NEXT, pressed);
  case SDLK_PAGEUP: return feed(IN_PAGE_UP, pressed);
  case SDLK_PAGEDOWN: return feed(IN_PAGE_DOWN, pressed);
  default: return 1; /* swallow everything else while open */
  }
}

/* Held d-pad auto-repeat (controllers send no repeats). */
static int g_held = -1;
static double g_held_next;

int DcOptions_OnGamepadButton(int b, int pressed) {
  DcOptions_Init();
  if (b == SDL_CONTROLLER_BUTTON_GUIDE) {
    if (pressed) { g_last_pad = 1; if (g_open) close_menu(true); else open_menu(); }
    return 1;
  }
  /* Closed: every button goes to the game exactly as before. */
  if (!g_open) return 0;
  if (pressed) { g_last_pad = 1; g_dirty = 1; }
  MenuIn in;
  switch (b) {
  case SDL_CONTROLLER_BUTTON_DPAD_UP: in = IN_UP; break;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: in = IN_DOWN; break;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: in = IN_LEFT; break;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: in = IN_RIGHT; break;
  case SDL_CONTROLLER_BUTTON_A: in = IN_OK; break;
  case SDL_CONTROLLER_BUTTON_B: in = IN_BACK; break;
  case SDL_CONTROLLER_BUTTON_X: in = IN_APPLY; break;
  case SDL_CONTROLLER_BUTTON_START: in = IN_APPLY; break;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: in = IN_TAB_PREV; break;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: in = IN_TAB_NEXT; break;
  default: return 1;   /* swallowed while open */
  }
  if (in <= IN_RIGHT) {
    if (pressed) { g_held = (int)in; g_held_next = now_s() + 0.38; }
    else if (g_held == (int)in) g_held = -1;
  }
  return feed(in, pressed);
}

/* Test hooks (headless, no input device):
 *   DC_OPTIONS_OPEN_AT=<sim frame>  open the menu at that frame
 *   DC_OPTIONS_CLOSE_AFTER=<n>      close it again after n modal presents
 *   DC_OPTIONS_SELECT=<id>          while open, apply that shader preset
 *   DC_OPTIONS_KEYS=<list>          while open, feed menu inputs, one per
 *                                   10 presents: u d l r o(k) b(ack) a(pply)
 *                                   [ ] (tabs) L (open list)  P (pad hints) */
static long g_close_after = -1;
static long g_modal_presents;
static const char *g_test_keys;

void DcOptions_OnSimFrame(unsigned number) {
  static long open_at = -2;
  if (open_at == -2) {
    const char *e = getenv("DC_OPTIONS_OPEN_AT");
    open_at = e && *e ? atol(e) : -1;
    const char *c = getenv("DC_OPTIONS_CLOSE_AFTER");
    g_close_after = c && *c ? atol(c) : -1;
    g_test_keys = getenv("DC_OPTIONS_KEYS");
  }
  if (open_at >= 0 && (long)number == open_at && !g_open) {
    open_menu();
    g_modal_presents = 0;
    const char *sel = getenv("DC_OPTIONS_SELECT");
    int i = sel ? find_id(sel) : -1;
    if (i >= 0) apply_shader(i);
  }
}

int DcOptions_ModalActive(void) {
  poll_shader();
  if (!g_open) return 0;
  if (g_close_after >= 0 && g_modal_presents >= g_close_after) {
    close_menu(true);
    return 0;
  }
  double t = now_s();
  if (g_held >= 0 && t >= g_held_next) {
    menu_input((MenuIn)g_held);
    g_held_next = t + 0.085;
  }
  /* The shader list previews the highlighted preset once it rests. */
  if (g_list && g_list_moved_at > 0 && t - g_list_moved_at > 0.35) {
    g_list_moved_at = 0;
    preview_shader(g_list_sel);
  }
  if (g_test_keys && *g_test_keys && g_modal_presents > 0 && g_modal_presents % 10 == 0) {
    static long done = -1;
    if (done != g_modal_presents) {
      done = g_modal_presents;
      char k = *g_test_keys++;
      static const char *map = "udlrobaq[]";
      (void)map;
      switch (k) {
      case 'u': menu_input(IN_UP); break;
      case 'd': menu_input(IN_DOWN); break;
      case 'l': menu_input(IN_LEFT); break;
      case 'r': menu_input(IN_RIGHT); break;
      case 'o': menu_input(IN_OK); break;
      case 'b': menu_input(IN_BACK); break;
      case 'a': menu_input(IN_APPLY); break;
      case '[': menu_input(IN_TAB_PREV); break;
      case ']': menu_input(IN_TAB_NEXT); break;
      case 'P': g_last_pad = 1; g_dirty = 1; break;
      case 'K': g_last_pad = 0; g_dirty = 1; break;
      default: break;
      }
    }
  }
  return 1;
}

int DcOptions_DrawModalLayer(const SnesModalLayerView *v, SnesModalLayer *out) {
  g_modal_presents++;
  if (!g_open) return 0;
  static const uint32_t *last_frame_seen;
  static double last_open;
  if (g_opened_at != last_open || v->frame != last_frame_seen) {
    /* The frame is frozen while open: blur it once per opening. */
    make_blur(v);
    last_open = g_opened_at;
    last_frame_seen = v->frame;
    g_dirty = 1;
  }
  if (v->out_w != g_last_out_w || v->out_h != g_last_out_h) {
    g_last_out_w = v->out_w;
    g_last_out_h = v->out_h;
    g_dirty = 1;
  }
  /* Status and confirm prompts expire; re-render a few times a second. */
  static double last_tick;
  double t = now_s();
  if (t - last_tick > 0.25) { last_tick = t; g_dirty = 1; }
  if (g_dirty || g_animating || !g_layer) {
    g_dirty = 0;
    render_panel(v);
  }
  if (!g_layer) return 0;
  out->argb = g_layer;
  out->x = g_lx; out->y = g_ly; out->w = g_lw; out->h = g_lh;
  return 1;
}

/* The old 2x black/white path, unused while draw_modal_layer is wired. */
void DcOptions_DrawModal(uint8_t *dst, size_t pitch, int w, int h) {
  (void)dst; (void)pitch; (void)w; (void)h;
  g_modal_presents++;
}
