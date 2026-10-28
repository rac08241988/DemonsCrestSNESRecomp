#!/usr/bin/env bash
# Build the GitHub Release asset (Quiver Launcher layout):
#   dist/DemonsCrest-SNESRecomp-windows-x64-v<VERSION>.zip
# Flat zip root (exe + DLLs + shaders/ + configs + README + LICENSE, no
# wrapper folder), plus the prebuilt native analyzer the launcher's
# "Generate & rebuild" needs (Windows players have no Rust toolchain).
#
# Usage: scripts/make_release_zip.sh <setup-host-build-dir> [version]
#   The build dir is a mingw build with -DSNESRECOMP_SETUP_HOST=ON and an
#   empty src/gen. ANALYZER_EXE may name a prebuilt snesrecomp-analyze.exe;
#   otherwise it is cross-built with cargo (target x86_64-pc-windows-gnu).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUILD_DIR="${1:?setup-host build dir}"
VERSION="${2:-1.0.0}"

bash scripts/package_release.sh "$BUILD_DIR" windows-x64
PACK="$(ls -t dist/demonscrest-*-windows-x64.zip | head -1)"

if [ -z "${ANALYZER_EXE:-}" ]; then
  ( cd snesrecomp/recompiler-rs &&
    CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER="${CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER:-x86_64-w64-mingw32-gcc}" \
    cargo --locked build --release --bin snesrecomp-analyze --target x86_64-pc-windows-gnu )
  ANALYZER_EXE="snesrecomp/recompiler-rs/target/x86_64-pc-windows-gnu/release/snesrecomp-analyze.exe"
fi

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
python3 - "$PACK" "$TMP/x" <<'PY'
import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
PY
TOP=("$TMP"/x/*)
if [ "${#TOP[@]}" -eq 1 ] && [ -d "${TOP[0]}" ]; then SRC="${TOP[0]}"; else SRC="$TMP/x"; fi
mkdir -p "$SRC/snesrecomp/recompiler-rs/target/release"
cp "$ANALYZER_EXE" "$SRC/snesrecomp/recompiler-rs/target/release/snesrecomp-analyze.exe"

# Hard guards: nothing ROM-derived may ship.
if find "$SRC" \( -iname '*.sfc' -o -iname '*.smc' -o -iname '*.fig' -o -iname '*.swc' \) | grep -q .; then
  echo "make_release_zip: ROM-like file in pack" >&2; exit 1; fi
if [ -n "$(ls -A "$SRC/src/gen" 2>/dev/null | grep -v '^\.gitkeep$' || true)" ]; then
  echo "make_release_zip: src/gen is not empty" >&2; exit 1; fi
if [ "$(find "$SRC" -maxdepth 1 -iname '*.exe' | wc -l)" -ne 1 ]; then
  echo "make_release_zip: expected exactly one top-level exe" >&2; exit 1; fi

OUT="$ROOT/dist/DemonsCrest-SNESRecomp-windows-x64-v${VERSION}.zip"
rm -f "$OUT"
( cd "$SRC" && zip -qr -X "$OUT" . )
echo "wrote $OUT ($(du -h "$OUT" | cut -f1))"
