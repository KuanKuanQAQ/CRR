#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""生成随机化函数名单（funcs.txt），按**实测的可搬移性**筛选并按引用数排序。

为什么要筛：本仓库的实测（见 `实现/12-got-codemodel-results.md` §2.1、§3）表明，
把一个内建函数改成"可整体搬移"并非对任意函数都成立，有两类硬约束：

  A. 与 64 位绝对寻址不相容的内联汇编（编不过）
     · per-CPU 访问（`current`/`this_cpu_*`）—— `%gs:<常量偏移>`，**不可回避**
     · `BUG()`/`WARN()`（DEBUG_BUGVERBOSE 用 `"i"(__FILE__)`）
     · 静态键（`arch_static_branch` 用 `"i"(key)`）
  B. 以代码地址为键的旁表（搬移后表项失效）
     · `__ex_table` —— 失配是**正确性错误**（本该 fixup 的异常变成 oops）
     · `__bug_table` / `__jump_table` / `.altinstructions` / `.smp_locks` —— 功能退化

因此本工具从目标文件的重定位里**实测**每个函数是否触碰上述构造，只输出干净的，
并按"被引用次数"降序（引用越多，D1 索引收敛越显著，见 E3-1）。

用法：
  gen_funcs.py --build-dir <build> --scope 'fs/*.o' 'net/core/*.o' \\
               --refs '**/*.o' --top 256 --out funcs.txt
"""
import argparse
import glob
import os
import re
import subprocess
import sys
from collections import defaultdict

# 硬约束：命中即排除
HARD = {
    "__ex_table": "异常修正表（搬移后 fixup 失配＝正确性错误）",
    "__bug_table": "BUG()/WARN()（\"i\"(__FILE__) 与绝对寻址不容）",
    "__jump_table": "静态键（\"i\"(key) 与绝对寻址不容）",
}
# 软约束：默认也排除，可用 --allow-soft 放行
SOFT = {
    ".altinstructions": "alternatives 运行时改写",
    ".smp_locks": "SMP 锁前缀改写",
}
CALL = {"R_X86_64_PLT32", "R_AARCH64_CALL26", "R_AARCH64_JUMP26"}
PTR = {"R_X86_64_64", "R_AARCH64_ABS64"}
CODEREF = {"R_X86_64_PC32", "R_X86_64_32S", "R_X86_64_32"}


def run(c):
    return subprocess.run(c, capture_output=True, text=True, errors="replace").stdout


def sections(obj):
    s = {}
    for ln in run(["readelf", "-SW", obj]).splitlines():
        m = re.match(r"\s*\[\s*(\d+)\]\s+(\S+)", ln)
        if m:
            s[int(m.group(1))] = m.group(2)
    return s


def symbols(obj):
    """返回 (函数区间 per 节, 名字->节名)。"""
    per_sec, sym_sec = defaultdict(list), {}
    for ln in run(["readelf", "-sW", obj]).splitlines():
        m = re.match(r"\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+\S+\s+(\S+)\s+(\S+)", ln)
        if not m:
            continue
        val, size, typ, ndx, name = (int(m.group(1), 16), int(m.group(2)),
                                     m.group(3), m.group(5), m.group(6))
        sym_sec[name] = ndx
        if typ == "FUNC" and size > 0 and ndx.isdigit():
            per_sec[int(ndx)].append((val, size, name))
    for v in per_sec.values():
        v.sort()
    return per_sec, sym_sec


def owner(per_sec, secs, sec_name, off):
    idx = next((i for i, n in secs.items() if n == sec_name), None)
    if idx is None:
        return None
    lst = per_sec.get(idx, [])
    for val, size, name in lst:
        if val <= off < val + size:
            return name
    return lst[0][2] if len(lst) == 1 else None


def analyze_scope(obj, allow_soft):
    """返回 (本文件定义的函数集, 被排除的函数->原因, percpu 命中集)。"""
    secs = sections(obj)
    per_sec, sym_sec = symbols(obj)
    defined = {n for lst in per_sec.values() for _, _, n in lst}
    bad = {}
    cur = None
    bad_secs = dict(HARD) if allow_soft else {**HARD, **SOFT}

    for ln in run(["readelf", "-rW", obj]).splitlines():
        m = re.match(r"Relocation section '(\S+)'", ln)
        if m:
            r = m.group(1)
            cur = r[5:] if r.startswith(".rela") else (r[4:] if r.startswith(".rel") else None)
            continue
        if cur is None:
            continue
        m = re.match(r"\s*([0-9a-f]+)\s+[0-9a-f]+\s+(\S+)\s+[0-9a-f]+"
                     r"(?:\s+(\S+))?(?:\s+([+-])\s+(\S+))?\s*$", ln)
        if not m:
            continue
        off_in_sec, rtype, sym, sign, add = (int(m.group(1), 16), m.group(2),
                                             m.group(3), m.group(4), m.group(5))
        try:
            addend = int(add, 16 if add and add.startswith("0x") else 10) if add else 0
        except (ValueError, TypeError):
            addend = 0
        if sign == "-":
            addend = -addend

        # (1) 旁表：表项指向 .text+addend，落到哪个函数就排除哪个
        if cur in bad_secs and sym and sym.startswith(".text"):
            fn = owner(per_sec, secs, sym, addend)
            if fn:
                bad.setdefault(fn, bad_secs[cur])
            continue

        # (2) per-CPU 访问：.text 里引用 percpu 符号
        if cur.startswith(".text") and sym:
            tgt_sec = secs.get(int(sym_sec[sym]), "") if sym_sec.get(sym, "").isdigit() else ""
            if sym in ("pcpu_hot",) or sym.startswith("__per_cpu") or \
               tgt_sec.startswith(".data..percpu"):
                fn = owner(per_sec, secs, cur, off_in_sec)
                if fn:
                    bad.setdefault(fn, "per-CPU 访问（%gs:常量偏移，不可回避）")
    return defined, bad


def count_refs(files, scope_names):
    refs = defaultdict(int)
    ops = defaultdict(int)
    for o in files:
        cur = None
        for ln in run(["readelf", "-rW", o]).splitlines():
            m = re.match(r"Relocation section '(\S+)'", ln)
            if m:
                r = m.group(1)
                cur = r[5:] if r.startswith(".rela") else (r[4:] if r.startswith(".rel") else None)
                continue
            if cur is None:
                continue
            m = re.match(r"\s*[0-9a-f]+\s+[0-9a-f]+\s+(\S+)\s+[0-9a-f]+(?:\s+(\S+))?", ln)
            if not m:
                continue
            rtype, sym = m.group(1), m.group(2)
            if not sym or sym not in scope_names:
                continue
            if rtype in CALL or rtype in PTR or rtype in CODEREF:
                refs[sym] += 1
                if rtype in PTR and not cur.startswith(".text"):
                    ops[sym] += 1        # 函数指针表（ops 结构）
    return refs, ops


def expand(build, pats, restrict_to_build=True):
    out, seen = [], set()
    root = os.path.realpath(build)
    for p in pats:
        for f in glob.glob(os.path.join(build, p), recursive=True):
            if not f.endswith(".o") or f.endswith(".mod.o") or "vmlinux.o" in f:
                continue
            rp = os.path.realpath(f)
            if rp in seen or (restrict_to_build and not rp.startswith(root + os.sep)):
                continue
            seen.add(rp)
            out.append(f)
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--scope", nargs="+", required=True, help="候选函数来自哪些 .o")
    ap.add_argument("--refs", nargs="+", help="到哪里数引用（默认全树 '**/*.o'）")
    ap.add_argument("--top", type=int, default=0, help="只取引用数最高的 N 个（0=全部）")
    ap.add_argument("--out", help="写出 funcs.txt")
    ap.add_argument("--allow-soft", action="store_true",
                    help="放行 alternatives/smp_locks 这类软约束函数")
    args = ap.parse_args()

    scope_files = expand(args.build_dir, args.scope)
    if not scope_files:
        sys.exit("scope 没匹配到 .o")
    ref_files = expand(args.build_dir, args.refs or ["**/*.o"])

    defined, bad = set(), {}
    for o in scope_files:
        d, b = analyze_scope(o, args.allow_soft)
        defined |= d
        bad.update(b)

    eligible = defined - set(bad)
    refs, ops = count_refs(ref_files, eligible)

    ranked = sorted(eligible, key=lambda f: (-refs.get(f, 0), f))
    if args.top:
        ranked = ranked[:args.top]

    reasons = defaultdict(int)
    for r in bad.values():
        reasons[r] += 1

    print(f"候选目标文件      : {len(scope_files)}")
    print(f"引用扫描目标文件  : {len(ref_files)}")
    print(f"定义的函数        : {len(defined)}")
    print(f"因不可搬移被排除  : {len(bad)}  ({len(bad)*100/max(len(defined),1):.1f}%)")
    for r, n in sorted(reasons.items(), key=lambda x: -x[1]):
        print(f"    {n:>5}  {r}")
    print(f"可搬移（合格）    : {len(eligible)}")
    print(f"本次输出          : {len(ranked)}")
    total = sum(refs.get(f, 0) for f in ranked)
    if ranked:
        print(f"覆盖引用数        : {total}  (平均每函数 {total/len(ranked):.1f})")
        print("引用最多的前 10:")
        for f in ranked[:10]:
            print(f"    {f:<34}{refs.get(f,0):>7}" +
                  (f"   (ops 表 {ops[f]})" if ops.get(f) else ""))

    if args.out:
        with open(args.out, "w") as fh:
            fh.write("# I-KASLR 随机化函数名单（gen_funcs.py 生成）\n")
            fh.write(f"# 候选范围: {' '.join(args.scope)}\n")
            fh.write(f"# 已按实测可搬移性筛选，按被引用次数降序；共 {len(ranked)} 个\n")
            for f in ranked:
                fh.write(f"{f}\n")
        print(f"\n已写出: {args.out}")


if __name__ == "__main__":
    main()
