#!/usr/bin/env bash
# Apply patches/snesrecomp-dc.patch to the snesrecomp submodule (idempotent).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
P="$ROOT/patches/snesrecomp-dc.patch"
if git -C "$ROOT/snesrecomp" apply --reverse --check "$P" >/dev/null 2>&1; then
  echo "already applied: $(basename "$P")"
else
  git -C "$ROOT/snesrecomp" apply --whitespace=nowarn "$P"; echo "applied: $(basename "$P")"
fi
