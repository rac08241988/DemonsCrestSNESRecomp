# Demon's Crest SNES Recomp: shaders

The shader setup copies **N64 Recomp Magical Tetris** (its `SHADERS.md` and `shaders-release/package-external-shaders.py`) as closely as our presenter allows:

- **Same preset pack.** `libretro/slang-shaders` @ `e1d75632a205c70f14a4cc947c46d5abb7b3f7f1`, the same checkout Magical Tetris audited (`magical-tetris/shader-audit/slang-shaders`).
- **Same staging.** `scripts/stage_slang_shaders.py` is that packaging script. It uses the same `#reference` / `shaderN` / texture / recursive `#include` resolution and the same per-directory `LICENSE`/`COPYING` copies. It writes the same `LICENSES/` folder (GPL-2, GPL-3, LGPL-2.1), plus `manifest.txt` (`id | path | display name | license`) and `README.txt`. It warns if the checkout is not at the audited commit.
- **Same audited list.** The script's `PRESETS` are Tetris's 12 "ship beside" presets, with the names and licenses from its audit (table below). They come first in the in-game list. On top of them, for this port's "all slang presets" rule, every one of the 246 presets in `presets/` is staged too, so 258 entries. Their license is "per file headers (not individually audited)".
- **Different runtime.** Tetris compiles the presets inside RT64 (glslang, SPIRV-Cross). That loader is part of RT64's renderer and cannot be dropped into this port's SDL/OpenGL presenter. Here the same files are compiled at runtime by **librashader 0.12.0** (`librashader.dll`, MPL-2.0, loaded dynamically). Its OpenGL runtime runs on the OpenGL renderer and its Vulkan runtime on the Vulkan renderer. **Shaders work on Vulkan and OpenGL renderers only.**
- **The 88 `.glslp` presets** (`libretro/glsl-shaders` @ `cba01a48`, `shaders/presets/`) are kept for the OpenGL renderer.

Staging: `SLANG_SHADERS_DIR=<slang-shaders checkout> bash scripts/fetch_shaders.sh`. In-game: Esc / F12 / pad Guide, then Video, then Shader. Left/Right picks a preset and Enter applies it. The choice is saved and defaults to Off.

## License audit (from Magical Tetris, same commit)

A preset is only as permissive as its **least** permissive file.

| Shader | License found | Class |
|---|---|---|
| crt-lottes | Public domain | PD |
| sharp-shimmerless | "License: Public domain" | PD |
| smootheststep | No license header | **Unclear** |
| scanline (simple) | No license header | **Unclear** |
| lcd-grid-v2 | No license header; cgwg's original Cg lcd-grid was GPL | **Unclear / likely GPL** |
| super-xBR | MIT passes, plus `custom-jinc2-sharper.slang` (GPL-2.0+) | **GPL** (mixed) |
| HQ2x | LGPL-2.1+ | LGPL |
| xBRZ freescale / multipass | MIT header, but "uses code and/or concepts from xBRZ" (GPL-3.0) | **GPL-3** (mixed) |
| crt-easymode | "License: GPL", no version | GPL |
| crt-geom | GPL-2.0+ | GPL |
| crt-royale | GPL-2.0+, `LICENSE.TXT` | GPL |

### Legal uncertainty (not legal advice, same caveats as Magical Tetris)

- **GPL shaders beside a non-GPL exe.** The shaders are unmodified source files in a separate `shaders/` folder, and librashader reads and compiles them at runtime. Nothing is linked, so this is the usual "mere aggregation" / data-file argument. It is widely relied on but not settled law. The exe runs without them. The files are their own corresponding source, and the GPL/LGPL texts are included.
- **Mixed-license files** (xBRZ, super-xBR): treat them as the most restrictive license involved.
- **Files without headers.** The default is "all rights reserved", and shipping them beside the exe still counts as distributing them. To drop them, remove their entries from `PRESETS` and/or filter `presets/` in the script.
- **Re-audit if the pinned commit changes.**
