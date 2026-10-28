#!/usr/bin/env bash
# Apply patches/<submodule>-dc.patch to each framework submodule (idempotent).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
for sub in snesrecomp recomp-ui; do
  P="$ROOT/patches/$sub-dc.patch"
  [ -f "$P" ] || continue
  if git -C "$ROOT/$sub" apply --reverse --check "$P" >/dev/null 2>&1; then
    echo "already applied: $(basename "$P")"
  else
    git -C "$ROOT/$sub" apply --whitespace=nowarn "$P"; echo "applied: $(basename "$P")"
  fi
done
