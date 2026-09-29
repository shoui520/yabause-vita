#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Split pgo.bin (src/vita/pgo_dump.c) into one .gcda tree per run and merge
them with gcov-tool into OUT_DIR, laid out for -fprofile-use=OUT_DIR.

usage: tools/pgo_split.py pgo.bin OUT_DIR [--gcov-tool PATH] [--build-dir GEN USE]
File names embed the generating build directory; --build-dir renames it to the
directory that will build with -fprofile-use.
"""
import argparse, os, shutil, struct, subprocess, sys, tempfile

p = argparse.ArgumentParser()
p.add_argument("blob"); p.add_argument("out")
p.add_argument("--build-dir", nargs=2, metavar=("GEN", "USE"))
p.add_argument("--gcov-tool", default=os.path.join(os.environ.get("VITASDK", ""), "bin", "arm-vita-eabi-gcov-tool"))
a = p.parse_args()
data = open(a.blob, "rb").read()
runs, pos = [], 0
while pos < len(data):
    (n,) = struct.unpack_from("<I", data, pos); pos += 4
    if n == 0:
        runs.append({}); continue
    name = data[pos:pos + n].decode(); pos += n
    (m,) = struct.unpack_from("<I", data, pos); pos += 4
    runs[-1][name] = data[pos:pos + m]; pos += m
if not runs:
    sys.exit("no runs")
prefix = "ux0:data/yabause-vita/pgo/"
tmp = tempfile.mkdtemp()
dirs = []
for k, files in enumerate(runs):
    d = os.path.join(tmp, str(k)); dirs.append(d)
    for name, blob in files.items():
        rel = name[len(prefix):] if name.startswith(prefix) else os.path.basename(name)
        if a.build_dir:
            gen, use = (os.path.abspath(x).replace("/", "#") + "#" for x in a.build_dir)
            rel = rel.replace(gen, use)
        path = os.path.join(d, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "wb").write(blob)
merged = dirs[0]
for k, d in enumerate(dirs[1:], 1):
    out = os.path.join(tmp, f"m{k}")
    subprocess.run([a.gcov_tool, "merge", merged, d, "-o", out], check=True)
    merged = out
if os.path.exists(a.out):
    sys.exit(f"{a.out} exists")
shutil.copytree(merged, a.out)
print(f"{len(runs)} runs, {len(runs[-1])} objects -> {a.out}")
