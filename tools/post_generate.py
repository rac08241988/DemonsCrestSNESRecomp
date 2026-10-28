#!/usr/bin/env python3
"""Post-generate step run by the one-click launcher builder.

The snesrecomp codegen host runs this after `snesrecomp generate`, so the
launcher produces the same tree as tools/regen.sh (which injects the
widescreen hooks right after generating).
"""
import argparse
import pathlib
import subprocess
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--project-root", default=".")
    ap.add_argument("--gen-dir", default="src/gen")
    args = ap.parse_args()
    root = pathlib.Path(args.project_root).resolve()
    gen = pathlib.Path(args.gen_dir)
    if not gen.is_absolute():
        gen = root / gen
    hooks = root / "tools" / "dc_apply_widescreen_hooks.py"
    print(f"post_generate: injecting widescreen hooks into {gen}", flush=True)
    return subprocess.call([sys.executable, str(hooks), "--gen-dir", str(gen)],
                           cwd=str(root))


if __name__ == "__main__":
    raise SystemExit(main())
