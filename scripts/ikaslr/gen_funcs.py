#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""生成随机化函数名单（funcs.txt），按**实测的可搬移性**筛选并按引用数排序。

为什么要筛：本仓库的实测（见 `实现/12-got-codemodel-results.md` §2.1、§3）表明，
把一个内建函数改成"可整体搬移"并非对任意函数都成立，有两类硬约束：

  A. 与 64 位绝对寻址不相容的内联汇编（编不过）
     · per-CPU 访问（`current`/`this_cpu_*`）—— `%gs:<常量偏移>`，**不可回避**
     · `BUG()`/`WARN()`（DEBUG_BUGVERBOSE 用 `"i"(__FILE__)`）
     · 静态键（`arch_static_branch` 用 `"i"(key)`）
  B. 以代码地址为键、且**必须写回代码**的旁表
     · `.static_call_sites` —— 写进去的是指向区域外的 `call rel32`，偏移随函数
       位置而变，与"函数体内不得有指向区域外的 PC 相对引用"这条不变式直接冲突，
       **无解**，只能排除。
       （`__mcount_loc` 起初也列在这里，实测后发现判错了，见下面 HARD 的注释。）

  已不再是约束的（2026-09-10 起，见 `实现/17-implementation-current.md` §3.7）：
     · `__ex_table` / `__bug_table` —— 改为在**查表时换算地址**（变体地址 ->
       映像链接期地址），表本身一个字节都不用改。
     · `__jump_table` —— 静态键的 JMP 偏移是"目标标号 − 本指令"，两者同属一个
       函数、随函数整体搬移，差值不变；运行期开关由 jump_label 钩子同时打到
       映像母本与各变体上。
     · `.altinstructions` / `.smp_locks` —— 在启动时一次性施加到映像母本上，
       而变体**始终从母本复制**，因此变体天然带着已打好的补丁。

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

# 硬约束：命中即排除。
# 只剩下"必须把指向区域外的 PC 相对调用写回代码里"的那两个——那与位置无关性
# 不相容，没有办法。其余地址键旁表已由运行时的地址换算/母本传播处理，见模块注释。
HARD = {
    ".static_call_sites": "静态调用点（写回的是指向区域外的 call rel32）",
}
# __mcount_loc **不在硬约束里**——这一条起初判错了，实测更正如下：
#
#   CONFIG_DYNAMIC_FTRACE 在构建期就把 `call __fentry__` 换成了 5 字节 NOP
#   （实测 dput_body 入口是 `endbr64` + `0f 1f 44 00 00`），因此静态映像里
#   **没有**指向区域外的 PC 相对调用，函数体照样可搬移——开着 FUNCTION_TRACER
#   编 S3 范围，verify_movable.py 依然报"全部可搬移"。
#
#   冲突发生在**运行期启用 tracer 的那一刻**：ftrace 按 __mcount_loc 记录的
#   **映像地址**去打补丁，而执行发生在变体里，于是补丁打在一份首次随机化后就被
#   置 NX、再也不会执行的代码上——**静默失效**（与 kprobes 探函数体同一失效模式，
#   见 实现/实验/E3-C）。若反过来把补丁翻译到当前变体，写进去的 `call __fentry__`
#   是指向区域外的 call rel32，下一轮搬移就会失效——那才是真正不相容的地方。
#
#   因此这不是"编不出来"的硬约束，而是"能用但功能会静默丢失"的共存问题：
#   排除掉它会让开着 ftrace 的内核一个函数都不能随机化（53,281 个插桩点覆盖几乎
#   全部函数），代价远大于收益。
# 软约束：默认放行，可用 --exclude-soft 重新排除（用于对照实验）
SOFT = {
    "__ex_table": "异常修正表（已由 fixup_exception 的地址换算处理）",
    "__bug_table": "BUG()/WARN()（已由 report_bug 的地址换算处理）",
    "__jump_table": "静态键（已由 jump_label 钩子处理）",
    ".altinstructions": "alternatives（启动时打在母本上，变体从母本复制）",
    ".smp_locks": "SMP 锁前缀（同上）",
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


def analyze_scope(obj, exclude_soft):
    """返回 (本文件定义的函数集, 被排除的函数->原因, percpu 命中集)。"""
    secs = sections(obj)
    per_sec, sym_sec = symbols(obj)
    defined = {n for lst in per_sec.values() for _, _, n in lst}
    bad = {}
    cur = None
    bad_secs = {**HARD, **SOFT} if exclude_soft else dict(HARD)

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
    ap.add_argument("--dump-excluded", metavar="目录",
                    help="把被排除的函数按原因分别写成 <目录>/<原因>.txt，"
                         "用于给出完整清单（实现/18-不可随机化清单.md）")
    ap.add_argument("--exclude-soft", action="store_true",
                    help="连同 __ex_table/__bug_table/__jump_table/alternatives "
                         "一起排除（这些默认已放行，运行时有对应处理；"
                         "本开关用于做\"若不处理旁表能随机化多少\"的对照）")
    args = ap.parse_args()

    scope_files = expand(args.build_dir, args.scope)
    if not scope_files:
        sys.exit("scope 没匹配到 .o")
    ref_files = expand(args.build_dir, args.refs or ["**/*.o"])

    defined, bad = set(), {}
    for o in scope_files:
        d, b = analyze_scope(o, args.exclude_soft)
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
    if args.dump_excluded:
        os.makedirs(args.dump_excluded, exist_ok=True)
        by_reason = {}
        for fn, why in bad.items():
            by_reason.setdefault(why, []).append(fn)
        for why, fns in by_reason.items():
            slug = re.sub(r"[^A-Za-z0-9_.-]+", "-", why)[:60].strip("-")
            path = os.path.join(args.dump_excluded, slug + ".txt")
            with open(path, "w") as fh:
                fh.write(f"# 排除原因: {why}\n# 共 {len(fns)} 个\n")
                fh.write("\n".join(sorted(fns)) + "\n")
            print(f"    -> {path}  ({len(fns)} 个)")
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
