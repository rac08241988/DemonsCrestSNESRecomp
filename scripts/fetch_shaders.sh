#!/usr/bin/env bash
# Fetch libretro/glsl-shaders at a pinned commit and stage the root-level
# presets (presets/**.glslp) plus every file they reference into
#   <DEST>/shaders/   (default DEST = repository root)
# The game looks for shaders/presets/ next to its working directory, so copy
# (or re-run with DEST=<build dir>) the staged shaders/ folder beside the exe.
#
# The shaders are NOT part of this repository. They are third-party works by
# the libretro contributors; each shader file carries its own license header
# (GPL, MIT, public domain, ...). See https://github.com/libretro/glsl-shaders
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$ROOT}"
COMMIT=cba01a48b0974a1232bea68d999a7c21bc4c233e
SRC="$ROOT/third_party/glsl-shaders"
if [ ! -d "$SRC/.git" ]; then
  mkdir -p "$ROOT/third_party"
  git clone --filter=blob:none --no-checkout https://github.com/libretro/glsl-shaders.git "$SRC"
fi
git -C "$SRC" fetch --quiet origin "$COMMIT" 2>/dev/null || git -C "$SRC" fetch --quiet origin
git -C "$SRC" checkout --quiet "$COMMIT"
python3 "$ROOT/scripts/stage_shaders.py" "$SRC" "$DEST"
echo "shaders staged in $DEST/shaders (libretro/glsl-shaders @ ${COMMIT:0:8})"

# libretro/slang-shaders: every preset of presets/ (librashader, OpenGL and
# Vulkan renderers) -> $DEST/shaders/slang, with license texts.
SLANG_COMMIT=e1d75632a205c70f14a4cc947c46d5abb7b3f7f1
# Default: the checkout N64 Recomp Magical Tetris audited, when present on this machine.
TETRIS_SLANG=/workspace/n64recomp/magical-tetris/shader-audit/slang-shaders
[ -z "${SLANG_SHADERS_DIR:-}" ] && [ -d "$TETRIS_SLANG/.git" ] && SLANG_SHADERS_DIR=$TETRIS_SLANG
SLANG_SRC="${SLANG_SHADERS_DIR:-$ROOT/third_party/slang-shaders}"
if [ ! -d "$SLANG_SRC/.git" ]; then
  mkdir -p "$(dirname "$SLANG_SRC")"
  git clone --filter=blob:none --no-checkout https://github.com/libretro/slang-shaders.git "$SLANG_SRC"
fi
git -C "$SLANG_SRC" fetch --quiet origin "$SLANG_COMMIT" 2>/dev/null || git -C "$SLANG_SRC" fetch --quiet origin
git -C "$SLANG_SRC" checkout --quiet "$SLANG_COMMIT"
python3 "$ROOT/scripts/stage_slang_shaders.py" "$SLANG_SRC" "$DEST"
