/*
 * Demon's Crest widescreen mod plugin.
 *
 * The package (mods/preloaded/packages/demons-crest.enhancement.widescreen)
 * declares the feature "widescreen" with the "aspect" choice; this file is
 * the trusted activation half. The runtime runs the reset callback before
 * activating plugins, so a disabled feature restores authentic 4:3 on every
 * launch even if a legacy config.ini stored Widescreen = 1.
 *
 * Mirrors MegaManXSNESRecomp's src/mods/mmx_widescreen_plugin.c.
 */

#include "mod_runtime.h"

#include <string.h>

#include "dc_display.h"
#include "dc_widescreen.h"

#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#endif

/* Must match the manifest: [[target]] applies to the package id, [[feature]]
 * and [[plugin]] carry these ids. */
#define DC_WIDESCREEN_PACKAGE "demons-crest.enhancement.widescreen"
#define DC_WIDESCREEN_FEATURE "widescreen"
#define DC_WIDESCREEN_PLUGIN  "demons-crest.widescreen"

static void DcWidescreenResetMod(void) {
    DcWidescreenReset();
    DcDisplay_SetWidescreenEnabled(false);
}

static void DcWidescreenActivateMod(void) {
    DcRenderAspect aspect = DC_ASPECT_ADAPTIVE;

#if defined(RECOMP_LAUNCHER)
    const RecompLauncherCModProvider *provider =
        snes_mod_runtime_launcher_provider_c();
    RecompLauncherCModOption option;
    for (int i = 0; provider && provider->feature_option_get &&
                    provider->feature_option_get(provider->ctx,
                                                 DC_WIDESCREEN_PACKAGE,
                                                 DC_WIDESCREEN_FEATURE, i,
                                                 &option);
         ++i) {
        if (strcmp(option.id, "aspect") != 0)
            continue;
        if (strcmp(option.value, "16:9") == 0)
            aspect = DC_ASPECT_16_9;
        else if (strcmp(option.value, "21:9") == 0)
            aspect = DC_ASPECT_21_9;
        else if (strcmp(option.value, "32:9") == 0)
            aspect = DC_ASPECT_32_9;
        else
            aspect = DC_ASPECT_ADAPTIVE;
    }
#endif

    DcWidescreenReset();
    g_dc_aspect = aspect;
    DcDisplay_SetWidescreenEnabled(true);
}

SNES_MOD_CONSTRUCTOR(DcRegisterWidescreenPlugin) {
    (void)snes_mod_register_reset_callback(DcWidescreenResetMod);
    (void)snes_mod_register_activation_plugin(DC_WIDESCREEN_PLUGIN,
                                              DcWidescreenActivateMod);
}

/* ---- The Widescreen setting as one row (in-game Options > Video and the
 * launcher's Settings > Video): 0 = Off, 1 + DcRenderAspect otherwise. It is
 * the same mod feature the Mods screen edits -- state.toml stays the one
 * record -- so both places always agree. */
static const char *const kDcAspectValues[] = {"adaptive", "16:9", "21:9", "32:9"};

int DcWidescreenSetting_Get(void) {
#if defined(RECOMP_LAUNCHER)
    const RecompLauncherCModProvider *p = snes_mod_runtime_launcher_provider_c();
    if (!p || !p->feature_count || !p->feature_get) return DcDisplay_IsWidescreenEnabled() ? 1 + (int)g_dc_aspect : 0;
    int n = p->feature_count(p->ctx);
    for (int i = 0; i < n; ++i) {
        RecompLauncherCModFeature f;
        memset(&f, 0, sizeof(f));
        if (!p->feature_get(p->ctx, i, &f)) continue;
        if (strcmp(f.package_id, DC_WIDESCREEN_PACKAGE) != 0 ||
            strcmp(f.id, DC_WIDESCREEN_FEATURE) != 0)
            continue;
        if (!f.enabled) return 0;
        RecompLauncherCModOption o;
        for (int k = 0; p->feature_option_get &&
                        p->feature_option_get(p->ctx, DC_WIDESCREEN_PACKAGE,
                                              DC_WIDESCREEN_FEATURE, k, &o);
             ++k) {
            if (strcmp(o.id, "aspect") != 0) continue;
            for (int a = 0; a < 4; ++a)
                if (strcmp(o.value, kDcAspectValues[a]) == 0) return 1 + a;
        }
        return 1;
    }
#endif
    return DcDisplay_IsWidescreenEnabled() ? 1 + (int)g_dc_aspect : 0;
}

int DcWidescreenSetting_Set(int index) {
    if (index < 0 || index > 4) return 0;
    int ok = 1;
#if defined(RECOMP_LAUNCHER)
    const RecompLauncherCModProvider *p = snes_mod_runtime_launcher_provider_c();
    if (p && p->feature_enable && p->commit) {
        if (index > 0 && p->feature_set_option)
            p->feature_set_option(p->ctx, DC_WIDESCREEN_PACKAGE, DC_WIDESCREEN_FEATURE,
                                  "aspect", kDcAspectValues[index - 1]);
        ok = p->feature_enable(p->ctx, DC_WIDESCREEN_PACKAGE, DC_WIDESCREEN_FEATURE,
                               index > 0) != 0;
        if (ok) ok = p->commit(p->ctx, "") != 0;
    }
#endif
    /* Live: the same steps as the plugin's reset/activation. */
    if (index == 0) {
        DcWidescreenResetMod();
    } else {
        DcWidescreenReset();
        g_dc_aspect = (DcRenderAspect)(index - 1);
        DcDisplay_SetWidescreenEnabled(true);
    }
    return ok;
}
