# Notices (shaders-120hz fork)

This fork of magnopaz/DemonsCrestSNESRecomp (MIT, Copyright (c) 2026 Magno Paz)
adds an in-game options menu, RetroArch GLSL shader presets and 120 Hz
presentation. Licensing of the pieces:

- `src/dc_options.c/.h`, `src/dc_interp.c/.h`, `src/dc_default_config.c`,
  `scripts/*.sh`, `scripts/stage_shaders.py`, `cmake/mingw-w64.cmake`: this
  fork's additions. The options menu and shader-staging scripts are ported from
  a Super Ryu World 1 port built on SuperMarioWorldRecomp/snesrecomp
  (Required Notice: Copyright (c) 2026 Matthew Stanley), which are under the
  PolyForm Noncommercial License 1.0.0; treat these ported parts as PolyForm
  Noncommercial 1.0.0 (non-commercial use only). The rest of the project stays
  MIT (see `LICENSE`).
- `patches/snesrecomp-dc.patch` modifies snesrecomp (PolyForm Noncommercial
  1.0.0, Copyright (c) 2026 Matthew Stanley); the patch is under the same
  license. Part of it (the multi-pass `glsl_shader.c` fixes) comes from the same
  SRW1 port.
- Submodules keep their own terms: snesrecomp (PolyForm Noncommercial 1.0.0,
  see its `THIRD_PARTY_ATTRIBUTION.md`), recomp-ui (MIT).
- libretro/glsl-shaders (commit cba01a48) is fetched at setup by
  `scripts/fetch_shaders.sh` and is not stored in this repository. Most
  shaders are GPL (v2 or later / v3); each file carries its own license header.
  Binary packages ship them unmodified in `shaders/` with that header intact.
- Demon's Crest is (c) Capcom. No ROM, generated C, or other ROM-derived data
  is distributed in this repository; everyone generates code from their own
  legally-owned cartridge dump.

Because the snesrecomp framework is PolyForm Noncommercial, any binary built
from this repository is for non-commercial use only.
