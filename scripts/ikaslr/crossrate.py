#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""统计随机化范围内：调用目标落在范围内(区域内) vs 范围外(须经 fixed_out)。"""
import glob, re, subprocess, sys, os
from collections import defaultdict
build, pats = sys.argv[1], sys.argv[2:]
files=[]
for p in pats: files += sorted(glob.glob(os.path.join(build,p)))
files=[f for f in files if f.endswith('.o')]
def run(c): return subprocess.run(c,capture_output=True,text=True,errors='replace').stdout

# 1) 范围内定义的所有函数符号（全局的才可能被跨文件调用）
defined=set()
for o in files:
    for ln in run(["readelf","-sW",o]).splitlines():
        m=re.match(r"\s*\d+:\s+[0-9a-f]+\s+(\d+)\s+FUNC\s+(\S+)\s+\S+\s+(\d+)\s+(\S+)",ln)
        if m and int(m.group(1))>0 and m.group(3)!='UND':
            defined.add(m.group(4))
# 2) 统计调用站点：目标在 defined 内=区域内，否则=跨区域
inside=outside=0
outsym=defaultdict(int)
CALL={"R_AARCH64_CALL26","R_AARCH64_JUMP26","R_X86_64_PLT32"}
for o in files:
    cur=None
    for ln in run(["readelf","-rW",o]).splitlines():
        m=re.match(r"Relocation section '(\S+)'",ln)
        if m:
            r=m.group(1); cur=r[5:] if r.startswith('.rela') else (r[4:] if r.startswith('.rel') else None); continue
        if not cur or not cur.startswith('.text'): continue
        m=re.match(r"\s*[0-9a-f]+\s+[0-9a-f]+\s+(\S+)\s+[0-9a-f]+(?:\s+(\S+))?",ln)
        if not m: continue
        rt,sym=m.group(1),m.group(2)
        if rt not in CALL or not sym: continue
        if sym in defined: inside+=1
        else: outside+=1; outsym[sym]+=1
tot=inside+outside
print(f"调用站点总数        : {tot}")
print(f"  区域内调用        : {inside} ({inside*100/tot:.1f}%)  —— 不经 fixed_out")
print(f"  跨区域调用        : {outside} ({outside*100/tot:.1f}%)  —— 每次付 fixed_out 代价")
print(f"不同的跨区域被调函数: {len(outsym)}  （即白名单规模下界）")
print("最热的跨区域目标:", ", ".join(f"{k}({v})" for k,v in sorted(outsym.items(),key=lambda x:-x[1])[:8]))
