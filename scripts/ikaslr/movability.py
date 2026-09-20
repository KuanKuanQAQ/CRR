#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""随机化候选函数的「可搬移性」统计。

把一个内建函数搬到随机化区域，会使**所有以代码地址为键的旁表**中指向它的表项失效：

  __ex_table        异常修正表（缺页/用户态拷贝的 fixup）—— 失配即**错误行为**
  __bug_table       BUG()/WARN() 表（GENERIC_BUG_RELATIVE_POINTERS 存的是相对偏移）
  __jump_table      静态键（jump label）
  .altinstructions  alternatives 运行时改写
  .orc_unwind_ip    ORC 栈回溯
  __mcount_loc      ftrace 桩点
  .static_call_sites / .retpoline_sites / .return_sites

本脚本统计随机化候选范围内，有多少函数被这些旁表引用——即「不能直接搬、需要编译器
插件额外处理旁表」的函数比例。这是把持续随机化推广到 built-in 内核代码的真实工程面，
也是 Adelie 一类只做模块的方案得以回避的部分。

用法：
  movability.py --build-dir <build> --objs 'fs/*.o' 'net/core/*.o'
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys
from collections import defaultdict

# 以代码地址为键的旁表（节名 -> 报告用短名）
ADDR_KEYED = {
    "__ex_table": "异常修正表",
    "__bug_table": "BUG/WARN 表",
    "__jump_table": "静态键",
    ".altinstructions": "alternatives",
    ".orc_unwind_ip": "ORC 回溯",
    "__mcount_loc": "ftrace 桩点",
    ".static_call_sites": "静态调用",
    ".retpoline_sites": "retpoline 点",
    ".return_sites": "return 点",
    ".smp_locks": "SMP 锁前缀",
}


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True,
                          errors="replace").stdout


def sections(obj):
    secs = {}
    for line in run(["readelf", "-SW", obj]).splitlines():
        m = re.match(r"\s*\[\s*(\d+)\]\s+(\S+)", line)
        if m:
            secs[int(m.group(1))] = m.group(2)
    return secs


def functions(obj, secs):
    """节索引 -> 按地址排序的函数符号列表。"""
    per_sec = defaultdict(list)
    for line in run(["readelf", "-sW", obj]).splitlines():
        m = re.match(r"\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+FUNC\s+\S+\s+\S+\s+(\d+)\s+(\S+)",
                     line)
        if not m:
            continue
        val, size, ndx, name = (int(m.group(1), 16), int(m.group(2)),
                                int(m.group(3)), m.group(4))
        if size > 0:
            per_sec[ndx].append((val, size, name))
    for v in per_sec.values():
        v.sort()
    return per_sec


def owner_of(per_sec, secs, target_sec_name, off):
    """把 (节名, 偏移) 落到包含它的函数符号。"""
    idx = None
    for i, n in secs.items():
        if n == target_sec_name:
            idx = i
            break
    if idx is None:
        return None
    lst = per_sec.get(idx, [])
    for val, size, name in lst:
        if val <= off < val + size:
            return name
    if len(lst) == 1:
        return lst[0][2]
    return None


def analyze(obj):
    secs = sections(obj)
    per_sec = functions(obj, secs)
    all_funcs = {name for lst in per_sec.values() for _, _, name in lst}

    touched = defaultdict(set)     # 旁表 -> 被引用的函数集合
    cur = None
    for line in run(["readelf", "-rW", obj]).splitlines():
        m = re.match(r"Relocation section '(\S+)'", line)
        if m:
            rel = m.group(1)
            cur = rel[5:] if rel.startswith(".rela") else (
                rel[4:] if rel.startswith(".rel") else None)
            continue
        if cur not in ADDR_KEYED:
            continue
        # 旁表里的表项指向 .text+addend
        m = re.match(r"\s*[0-9a-f]+\s+[0-9a-f]+\s+(\S+)\s+([0-9a-f]+)"
                     r"(?:\s+(\S+))?(?:\s+([+-])\s+(\S+))?\s*$", line)
        if not m:
            continue
        symname, sign, add = m.group(3), m.group(4), m.group(5)
        if not symname or not symname.startswith(".text"):
            continue
        try:
            addend = int(add, 16 if add and add.startswith("0x") else 10) if add else 0
        except (ValueError, TypeError):
            addend = 0
        if sign == "-":
            addend = -addend
        fn = owner_of(per_sec, secs, symname, addend)
        if fn:
            touched[cur].add(fn)
    return all_funcs, touched


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objs", nargs="+", required=True)
    ap.add_argument("--build-dir", default=".")
    ap.add_argument("--json")
    args = ap.parse_args()

    files = []
    for pat in args.objs:
        files += sorted(glob.glob(os.path.join(args.build_dir, pat), recursive=True))
    files = [f for f in files if f.endswith(".o") and not f.endswith(".mod.o")]
    if not files:
        sys.exit("没有匹配到 .o")

    total_funcs = 0
    per_table = defaultdict(int)
    blocked = set()          # 被任意旁表引用的函数（obj, name）
    for obj in files:
        funcs, touched = analyze(obj)
        total_funcs += len(funcs)
        for tbl, s in touched.items():
            per_table[tbl] += len(s)
            for fn in s:
                blocked.add((obj, fn))

    print(f"目标文件数        : {len(files)}")
    print(f"函数符号总数      : {total_funcs}")
    print()
    print(f"{'旁表':<22}{'被引用的函数数':>14}{'占比':>10}")
    for sec, label in ADDR_KEYED.items():
        n = per_table.get(sec, 0)
        if n:
            print(f"{label:<22}{n:>14}{n/total_funcs*100:>9.1f}%")
    nb = len(blocked)
    print()
    print(f"被至少一个旁表引用: {nb} / {total_funcs} = {nb/total_funcs*100:.1f}%")
    print(f"可直接整体搬移    : {total_funcs-nb} / {total_funcs} = "
          f"{(total_funcs-nb)/total_funcs*100:.1f}%")

    if args.json:
        json.dump(dict(files=len(files), total_funcs=total_funcs,
                       per_table={ADDR_KEYED[k]: v for k, v in per_table.items()},
                       blocked=nb,
                       movable_pct=(total_funcs - nb) / total_funcs * 100),
                  open(args.json, "w"), indent=2, ensure_ascii=False)


if __name__ == "__main__":
    main()
