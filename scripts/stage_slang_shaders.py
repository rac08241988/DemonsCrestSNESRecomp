#!/usr/bin/env python3
"""Stage every preset in libretro/slang-shaders' presets/ folder (all of them,
CRT, scalefx, xsoft, retro-v2, ...) plus exactly the files each one needs
(#reference chains, shaderN, textures, recursive #include) into
<dest>/shaders/slang/, preserving their relative paths, with each contributing
directory's LICENSE/COPYING files, the GPL-2/GPL-3/LGPL-2.1 texts and a
README.txt. The files are copied unmodified: librashader compiles them at
runtime, so the shipped files ARE the corresponding source.

Mirrors N64 Recomp's Magical Tetris setup (shaders-release/
package-external-shaders.py and SHADERS.md): same pinned libretro/slang-shaders
checkout, same dependency resolution, same LICENSES/, manifest.txt and
README.txt layout, and its 12 license-audited presets (PRESETS below, names and
licenses as audited there) listed first. On top of those, every preset of the
presets/ folder is staged, per the "all slang presets" rule for this port.
Usage: stage_slang_shaders.py <slang-shaders checkout> <dest root>
"""
import os, re, shutil, subprocess, sys

PINNED_COMMIT = "e1d75632a205c70f14a4cc947c46d5abb7b3f7f1"

# Magical Tetris' audited ship-beside list, verbatim (see docs/SHADERS.md):
# relative preset path | display name | license
PRESETS = [
    ("crt/crt-royale.slangp", "CRT-Royale", "GPL-2.0-or-later"),
    ("crt/crt-geom.slangp", "CRT-Geom", "GPL-2.0-or-later"),
    ("crt/crt-easymode.slangp", "CRT-Easymode", "GPL (version unstated)"),
    ("crt/crt-lottes.slangp", "CRT-Lottes", "Public domain"),
    ("edge-smoothing/xbrz/xbrz-freescale.slangp", "xBRZ Freescale", "MIT header + GPL-3.0 xBRZ code (treat as GPL-3.0)"),
    ("edge-smoothing/xbrz/xbrz-freescale-multipass.slangp", "xBRZ Freescale (multipass)", "MIT header + GPL-3.0 xBRZ code (treat as GPL-3.0)"),
    ("edge-smoothing/hqx/hq2x.slangp", "HQ2x", "LGPL-2.1-or-later"),
    ("edge-smoothing/xbr/super-xbr.slangp", "Super-xBR", "MIT + GPL-2.0-or-later (jinc2 pass)"),
    ("handheld/lcd-grid-v2.slangp", "LCD Grid v2", "Unclear (no header; cgwg original GPL)"),
    ("pixel-art-scaling/sharp-shimmerless.slangp", "Sharp Shimmerless", "Public domain"),
    ("pixel-art-scaling/smootheststep.slangp", "Smootheststep", "Unclear (no header)"),
    ("scanlines/scanline.slangp", "Scanline (simple)", "Unclear (no header)"),
]

def norm(p):
    return os.path.normpath(p).replace("\\", "/")

def strip_comment(line):
    out, q = [], False
    for i, ch in enumerate(line):
        if ch == '"':
            q = not q
        if not q and (ch == '#' or line.startswith("//", i)):
            break
        out.append(ch)
    return "".join(out)

def preset_deps(root, rel, seen, files):
    if rel in seen:
        return
    seen.add(rel); files.add(rel)
    base = os.path.dirname(rel)
    text = open(os.path.join(root, rel), encoding="utf-8", errors="replace").read()
    textures = []
    values = {}
    for raw in text.splitlines():
        m = re.match(r'\s*#reference\s+"?([^"]+)"?', raw)
        if m:
            preset_deps(root, norm(os.path.join(base, m.group(1).strip())), seen, files)
            continue
        line = strip_comment(raw)
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        values[k.strip()] = v.strip().strip('"').strip()
    for k, v in values.items():
        if re.fullmatch(r"shader\d+", k):
            shader_deps(root, norm(os.path.join(base, v)), files, 0)
    if "textures" in values:
        textures = [t.strip() for t in values["textures"].split(";") if t.strip()]
    for t in textures:
        if t in values:
            files.add(norm(os.path.join(base, values[t])))

def shader_deps(root, rel, files, depth):
    if rel in files or depth > 32:
        return
    files.add(rel)
    base = os.path.dirname(rel)
    for raw in open(os.path.join(root, rel), encoding="utf-8", errors="replace").read().splitlines():
        m = re.match(r'\s*#include\s+"([^"]+)"', raw)
        if m:
            shader_deps(root, norm(os.path.join(base, m.group(1))), files, depth + 1)

def main():
    if len(sys.argv) != 3:
        print(__doc__); return 2
    root = os.path.abspath(sys.argv[1])
    out = os.path.join(os.path.abspath(sys.argv[2]), "shaders", "slang")
    try:
        commit = subprocess.check_output(["git", "-C", root, "rev-parse", "HEAD"], text=True).strip()
    except Exception:
        commit = "unknown"
    if commit != PINNED_COMMIT:
        print(f"warning: checkout is at {commit}, audited commit is {PINNED_COMMIT}", file=sys.stderr)
    presets = []
    for d, _, names in os.walk(os.path.join(root, "presets")):
        for n in names:
            if n.endswith(".slangp"):
                presets.append(norm(os.path.relpath(os.path.join(d, n), root)))
    presets.sort()
    entries = [(rel, name, lic) for rel, name, lic in PRESETS]
    entries += [(rel, rel[len("presets/"):-len(".slangp")], "per file headers (not individually audited)")
                for rel in presets]
    presets = [rel for rel, _, _ in PRESETS] + presets
    files = set()
    for rel in presets:
        preset_deps(root, rel, set(), files)
    dirs = set()
    for f in files:
        d = os.path.dirname(f)
        while d:
            dirs.add(d); d = os.path.dirname(d)
    dirs.add("")
    for d in dirs:
        for name in os.listdir(os.path.join(root, d) if d else root):
            if re.match(r"(license|copying)", name, re.I) and os.path.isfile(os.path.join(root, d, name)):
                files.add(norm(os.path.join(d, name)) if d else name)
    missing = sorted(f for f in files if not os.path.isfile(os.path.join(root, f)))
    if missing:
        print("missing files (presets referencing them are still staged):\n  " + "\n  ".join(missing), file=sys.stderr)
        files -= set(missing)
    if os.path.exists(out):
        shutil.rmtree(out)
    lib = os.path.join(out, "libretro")
    for f in sorted(files):
        os.makedirs(os.path.dirname(os.path.join(lib, f)), exist_ok=True)
        shutil.copy2(os.path.join(root, f), os.path.join(lib, f))
    lic = os.path.join(out, "LICENSES")
    os.makedirs(lic, exist_ok=True)
    for name in ("GPL-2", "GPL-3", "LGPL-2.1"):
        src = os.path.join("/usr/share/common-licenses", name)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(lic, name + ".txt"))
        else:
            print(f"warning: {src} not found; add the {name} text manually", file=sys.stderr)
    with open(os.path.join(out, "manifest.txt"), "w", newline="\n") as m:
        m.write("# External post-processing presets, loaded at runtime by librashader (not linked into the executable).\n")
        m.write(f"# Source: https://github.com/libretro/slang-shaders @ {commit}\n")
        m.write("# Format: id | preset path (relative to this folder) | display name | license\n")
        for rel, name, license_ in entries:
            ident = rel[len("presets/"):-len(".slangp")] if rel.startswith("presets/") else os.path.splitext(rel)[0]
            m.write(f"{ident} | libretro/{rel} | {name} | {license_}\n")
    with open(os.path.join(out, "README.txt"), "w", newline="\n") as r:
        r.write("These shader presets are unmodified files from libretro/slang-shaders\n"
                f"(https://github.com/libretro/slang-shaders, commit {commit}).\n"
                f"{len(PRESETS)} license-audited presets (as in N64 Recomp Magical Tetris) plus all {len(presets) - len(PRESETS)}\n"
                "presets of its presets/ folder are included, with every file they use (manifest.txt).\n"
                "They are separate data files compiled at runtime by librashader (librashader.dll,\n"
                "MPL-2.0); they are not part of the game executable. Each keeps its own license as\n"
                "stated in its file header or LICENSE/COPYING file; the GPL/LGPL texts are in LICENSES/.\n"
                "The corresponding source is these files themselves.\n"
                "Pick a preset in-game: Escape / F12 / gamepad Guide > Video > Shader.\n"
                "Shaders work on Vulkan and OpenGL renderers only.\n")
    print(f"{len(files)} files, {len(presets)} presets -> {out}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
