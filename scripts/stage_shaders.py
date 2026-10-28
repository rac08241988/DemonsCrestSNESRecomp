#!/usr/bin/env python3
"""Stage libretro glsl-shaders root presets (presets/**.glslp) plus every file
they reference into <dest>/shaders, preserving repo-relative layout."""
import os,re,sys,shutil
src=os.path.abspath(sys.argv[1]); dest=os.path.join(os.path.abspath(sys.argv[2]),'shaders')
need=set(); missing=[]
def add(path):
    path=os.path.normpath(path)
    if path in need: return
    if not os.path.isfile(path): missing.append(path); return
    need.add(path)
    if path.endswith(('.glsl','.h','.inc')):
        for line in open(path,errors='replace'):
            m=re.match(r'\s*#include\s+"([^"]+)"',line)
            if m: add(os.path.join(os.path.dirname(path),m.group(1)))
presets=[]
for root,_,files in os.walk(os.path.join(src,'presets')):
    for f in files:
        if f.endswith('.glslp'): presets.append(os.path.join(root,f))
for p in sorted(presets):
    add(p); d=os.path.dirname(p)
    tex=set()
    for line in open(p,errors='replace'):
        line=line.split('#')[0].strip()
        m=re.match(r'(\w+)\s*=\s*"?([^"]*)"?',line)
        if not m: continue
        k,v=m.group(1),m.group(2).strip()
        if k=='textures': tex.update(t for t in v.split(';') if t)
        elif re.fullmatch(r'shader\d+',k): add(os.path.join(d,v))
    for line in open(p,errors='replace'):
        line=line.split('#')[0].strip()
        m=re.match(r'(\w+)\s*=\s*"?([^"]*)"?',line)
        if m and m.group(1) in tex: add(os.path.join(d,m.group(2).strip()))
shutil.rmtree(dest,ignore_errors=True)
for f in sorted(need):
    rel=os.path.relpath(f,src); o=os.path.join(dest,rel)
    os.makedirs(os.path.dirname(o),exist_ok=True); shutil.copy2(f,o)
print('presets',len(presets),'files',len(need),'missing',len(missing))
for m in missing: print('MISSING',os.path.relpath(m,src))
