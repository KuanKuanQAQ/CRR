#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""E3-1／W-33：真实规模下的代码索引更新收敛，**按索引类型分列**。

现有 `index_convergence.py` 只能量当前原型里那 4 个手工标注的函数（19 处引用、
78.9%），清单已指出这个小样本不能写进正文。本脚本在**不需要编译器 pass 生效**的
前提下给出真实规模的数字：

  传统方法（指针追踪类）一次随机化必须改写的索引数
      = 静态上指向「随机化范围内函数」的**全部引用**
  本机制一次随机化必须改写的索引数
      = 随机化范围内的**函数个数**（每函数一个 target 槽）
  收敛率 = 1 − 函数数 / 引用数

引用按类型分列（清单 W-33 的硬要求）：
  · 直接调用      —— .text 里的 PLT32/CALL26，指向范围内函数
  · 代码内取地址  —— .text 里的 PC32/32S/ADR_PREL 等，指向范围内函数
  · 函数指针表    —— **数据节**里的 64 位绝对重定位，指向范围内函数
                     （内核的 file_operations 一类 ops 表，是最大的一类）

说明：返回地址是运行期产物，静态数不出来；它正是本机制**不需要改写**的一类
（陈旧返回由陷阱兜底重定向，见 §3.4.7），在表中单列说明而不计入。

用法：
  index_convergence_scale.py --build-dir <build> --objs 'fs/*.o' 'net/core/*.o'
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys
from collections import defaultdict

CALL = {"R_X86_64_PLT32", "R_AARCH64_CALL26", "R_AARCH64_JUMP26"}
CODEREF = {"R_X86_64_PC32", "R_X86_64_32S", "R_X86_64_32",
           "R_AARCH64_ADR_PREL_PG_HI21", "R_AARCH64_ADD_ABS_LO12_NC"}
PTR = {"R_X86_64_64", "R_AARCH64_ABS64"}


def run(c):
    return subprocess.run(c, capture_output=True, text=True, errors="replace").stdout


def scope_functions(files):
    """范围内定义的函数符号名集合。"""
    defined = set()
    for o in files:
        for ln in run(["readelf", "-sW", o]).splitlines():
            m = re.match(r"\s*\d+:\s+[0-9a-f]+\s+(\d+)\s+FUNC\s+(\S+)\s+\S+\s+(\S+)\s+(\S+)", ln)
            if m and int(m.group(1)) > 0 and m.group(3) != "UND":
                defined.add(m.group(4))
    return defined


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objs", nargs="+", required=True)
    ap.add_argument("--build-dir", default=".")
    ap.add_argument("--ref-objs", nargs="+",
                    help="到哪里去找引用（默认与 --objs 相同）。传统方法必须改写"
                         "**全内核**指向随机化函数的引用，故正确口径是全树。")
    ap.add_argument("--json")
    args = ap.parse_args()

    files = []
    for p in args.objs:
        files += sorted(glob.glob(os.path.join(args.build_dir, p)))
    files = [f for f in files if f.endswith(".o") and not f.endswith(".mod.o")]
    if not files:
        sys.exit("没有匹配到 .o")

    scope = scope_functions(files)
    ref_files = files
    if args.ref_objs:
        ref_files = []
        for p in args.ref_objs:
            ref_files += sorted(glob.glob(os.path.join(args.build_dir, p), recursive=True))
        # 按 realpath 去重：内核构建目录里有 `source`/`build` 一类符号链接指回源码树，
        # `**` 会顺着它重复遍历（实测把 2 962 个 .o 放大成 124 695 个），
        # 不去重会把引用数放大数十倍。
        # 内核构建目录里有 `source` 符号链接指回源码树，`**` 会顺着它走出去：
        # 既重复遍历（实测 2 962 个 .o 被放大成 124 695 个），又会把**源码树里与
        # 本次构建无关的陈旧 .o**（如 boot/ 下的 rootfs 镜像）算进来。
        # 因此按 realpath 去重，并**只保留真正位于构建目录内**的目标文件。
        root = os.path.realpath(args.build_dir)
        seen, uniq = set(), []
        for f in ref_files:
            if not f.endswith(".o") or f.endswith(".mod.o") or "vmlinux.o" in f:
                continue
            rp = os.path.realpath(f)
            if rp in seen or not rp.startswith(root + os.sep):
                continue
            seen.add(rp)
            uniq.append(f)
        ref_files = uniq
    kinds = defaultdict(int)
    per_target = defaultdict(int)

    for o in ref_files:
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
            if not sym or sym not in scope:
                continue        # 只关心指向随机化范围内函数的引用
            in_text = cur.startswith(".text")
            if rtype in CALL and in_text:
                kinds["直接调用"] += 1
            elif rtype in CODEREF and in_text:
                kinds["代码内取地址"] += 1
            elif rtype in PTR and not in_text:
                kinds["函数指针表（数据节）"] += 1
            elif rtype in PTR and in_text:
                kinds["代码内 64 位指针"] += 1
            else:
                kinds["其他"] += 1
            per_target[sym] += 1

    total_refs = sum(kinds.values())
    nfunc = len(per_target)          # 实际被引用到的范围内函数
    conv = (1 - nfunc / total_refs) if total_refs else 0.0

    print(f"范围目标文件数                  : {len(files)}")
    print(f"扫描引用的目标文件数            : {len(ref_files)}")
    print(f"范围内定义的函数符号            : {len(scope)}")
    print(f"其中被引用到的（需要 target 槽）: {nfunc}")
    print()
    print(f"{'索引类型':<26}{'条目数':>10}{'占比':>9}")
    for k, v in sorted(kinds.items(), key=lambda x: -x[1]):
        print(f"{k:<26}{v:>10,}{v*100/total_refs:>8.1f}%")
    print(f"{'合计（传统方法要改）':<26}{total_refs:>10,}")
    print(f"{'target 槽（本机制要改）':<26}{nfunc:>10,}")
    print()
    print(f"收敛率 = 1 − {nfunc}/{total_refs} = {conv*100:.2f}%")
    print()
    print("注：返回地址是运行期产物，静态不可数；本机制不改写它（陈旧返回由陷阱兜底），")
    print("    故未计入分母。若计入，传统方法的分母只会更大、收敛率更高。")
    top = sorted(per_target.items(), key=lambda x: -x[1])[:8]
    print("引用最多的范围内函数:", ", ".join(f"{k}({v})" for k, v in top))

    if args.json:
        json.dump(dict(files=len(files), scope_funcs=len(scope), referenced=nfunc,
                       kinds=dict(kinds), total_refs=total_refs, convergence=conv),
                  open(args.json, "w"), indent=2, ensure_ascii=False)


if __name__ == "__main__":
    main()
