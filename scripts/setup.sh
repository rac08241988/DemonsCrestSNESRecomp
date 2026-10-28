#!/usr/bin/env bash
# Demon's Crest recomp (shaders + 120 Hz fork) - source setup.
#
# 1. Fetches the pinned git submodules (snesrecomp framework, recomp-ui).
# 2. Applies patches/snesrecomp-dc.patch to the framework (options-menu and
#    interpolation host hooks, RetroArch multi-pass shader loader fixes,
#    runtime shader switching). Safe to re-run.
# 3. Optionally fetches the libretro GLSL shaders (GPL; see fetch_shaders.sh).
# 4. Optionally verifies YOUR OWN ROM (sha256 from rom_identity.txt, a 512-byte
#    copier header is stripped) and regenerates the recompiled C into src/gen.
#
# No ROM, generated C, or ROM-derived data ships with this repository.
#
# Usage:
#   scripts/setup.sh [--rom PATH] [--no-shaders]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROM=""; SHADERS=1
while [ $# -gt 0 ]; do
  case "$1" in
    --rom) ROM="$2"; shift 2 ;;
    --no-shaders) SHADERS=0; shift ;;
    -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "setup.sh: unknown argument $1" >&2; exit 2 ;;
  esac
done
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
WANT=$(sed -n 's/^expected_sha256 *= *//p' "$ROOT/rom_identity.txt" | tr -d '" ')

cd "$ROOT"
echo "=== Fetching submodules ==="
git submodule update --init --recursive
bash "$ROOT/scripts/apply_framework_patch.sh"

if [ "$SHADERS" = 1 ]; then
  echo "=== Fetching libretro GLSL shaders ==="
  bash "$ROOT/scripts/fetch_shaders.sh"
fi

if [ -n "$ROM" ]; then
  TMP="$(mktemp)"; trap 'rm -f "$TMP"' EXIT
  SIZE=$(wc -c < "$ROM")
  if [ $((SIZE % 1024)) = 512 ]; then tail -c +513 "$ROM" > "$TMP"; else cp "$ROM" "$TMP"; fi
  [ "$(sha "$TMP")" = "$WANT" ] || { echo "ERROR: $ROM is not Demon's Crest (USA) (sha256 mismatch, want $WANT)" >&2; exit 1; }
  mkdir -p "$ROOT/rom"; cp "$TMP" "$ROOT/rom/Demon's Crest (USA).sfc"
  echo "=== Regenerating recompiled C from your ROM ==="
  bash "$ROOT/tools/regen.sh" --rom "$ROOT/rom/Demon's Crest (USA).sfc"
fi
echo "=== Setup done. See README.md (Shaders + 120 Hz fork) for build steps. ==="
