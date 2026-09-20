#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""T1：共享全局偏移表(GOT)的规模统计 —— 回填论文 §3.4.3 的 R-31。

方案见 Documentation/crr/设计文档/草稿/got-codemodel-howto.md §2。

核心洞察（方案 §2）：两种 GOT 方案都能从**同一份构建**的重定位信息算出来：
  per_func_total = Σ_函数f (f 引用到的不同全局数据符号数)   —— 每函数一张表
  shared_total   = 随机化范围内所有函数引用到的不同全局数据符号数（跨函数去重）
  dedup_ratio    = 1 - shared_total / per_func_total

口径说明（正文必须写明，见方案 §2「坑」）：
  * 统计的是**链接前 .o 的重定位**，不受链接器松弛(GOTPCRELX relaxation)影响。
  * 只计**全局数据符号**；外部函数符号排除在外——§3.4.3 优化一已把它们移出 GOT，
    交由 fixed_out + 白名单处理。
  * static(LOCAL) 符号按「目标文件 + 符号名」去重，避免头文件里的 static 变量
    在多个编译单元中被误当成同一项（方案 §2「坑」第 3 条）。

用法：
  got_stats.py --objs 'fs/*.o' 'net/core/*.o' [--build-dir DIR] [--json out.json]
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys
from collections import defaultdict

# 明确是函数调用的重定位类型（用于 UND/NOTYPE 符号的兜底判别）
CALL_RELOCS = {
    "R_X86_64_PLT32", "R_X86_64_GOTPCRELX", "R_X86_64_REX_GOTPCRELX",
    "R_AARCH64_CALL26", "R_AARCH64_JUMP26",
}
# 明确是 GOT 访问的重定位类型（C3 配置下才会出现）
GOT_RELOCS = {
    "R_X86_64_GOTPCREL", "R_X86_64_GOTPCRELX", "R_X86_64_REX_GOTPCRELX",
    "R_AARCH64_ADR_GOT_PAGE", "R_AARCH64_LD64_GOT_LO12_NC",
}


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True,
                          errors="replace").stdout


def read_sections(obj):
    """节索引 -> 节名。"""
    secs = {}
    for line in run(["readelf", "-SW", obj]).splitlines():
        m = re.match(r"\s*\[\s*(\d+)\]\s+(\S+)", line)
        if m:
            secs[int(m.group(1))] = m.group(2)
    return secs


def read_symbols(obj):
    """返回 (符号列表, 名字->符号)。符号: dict(name,value,size,type,bind,ndx)"""
    syms, by_name = [], {}
    for line in run(["readelf", "-sW", obj]).splitlines():
        # Num: Value Size Type Bind Vis Ndx Name
        m = re.match(r"\s*(\d+):\s+([0-9a-f]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+\S+\s+(\S+)\s+(\S+)",
                     line)
        if not m:
            continue
        s = dict(value=int(m.group(2), 16), size=int(m.group(3)),
                 type=m.group(4), bind=m.group(5), ndx=m.group(6),
                 name=m.group(7))
        syms.append(s)
        by_name.setdefault(s["name"], s)
    return syms, by_name


def classify(name, sym, reloc_type, secs):
    """判定被引用符号是 'data' 还是 'func'（见文件头口径说明）。"""
    if sym:
        if sym["type"] == "OBJECT":
            return "data"
        if sym["type"] == "FUNC":
            return "func"
        # 已定义但无类型：看它定义在哪个节
        if sym["ndx"].isdigit():
            sname = secs.get(int(sym["ndx"]), "")
            return "func" if sname.startswith(".text") else "data"
    # UND / NOTYPE：靠重定位类型兜底
    return "func" if reloc_type in CALL_RELOCS else "data"


def analyze(obj, include_funcs=False):
    """返回 (func -> set(dedup_key)), got_reloc_count, func_count。"""
    secs = read_sections(obj)
    syms, by_name = read_symbols(obj)

    # 每个节内的函数符号区间，用于把重定位归属到函数（不依赖 -ffunction-sections）
    funcs_in_sec = defaultdict(list)
    for s in syms:
        if s["type"] == "FUNC" and s["ndx"].isdigit() and s["size"] > 0:
            funcs_in_sec[int(s["ndx"])].append(s)
    for lst in funcs_in_sec.values():
        lst.sort(key=lambda x: x["value"])

    # 节名 -> 节索引
    name2idx = {v: k for k, v in secs.items()}

    refs = defaultdict(set)
    got_relocs = 0
    data_sites = 0
    call_sites = 0
    cur_sec = None
    for line in run(["readelf", "-rW", obj]).splitlines():
        m = re.match(r"Relocation section '(\S+)'", line)
        if m:
            rel = m.group(1)
            # .rela.text.foo -> .text.foo
            cur_sec = rel[5:] if rel.startswith(".rela") else (
                rel[4:] if rel.startswith(".rel") else None)
            continue
        if cur_sec is None or not cur_sec.startswith(".text"):
            continue
        # Offset Info Type Sym.Value Sym.Name [+|- Addend]
        m = re.match(r"\s*([0-9a-f]+)\s+[0-9a-f]+\s+(\S+)\s+([0-9a-f]+)"
                     r"(?:\s+(\S+))?(?:\s+([+-])\s+(\S+))?\s*$", line)
        if not m:
            continue
        off, rtype, symname = int(m.group(1), 16), m.group(2), m.group(4)
        if not symname:
            continue
        addend = 0
        if m.group(6):
            try:
                addend = int(m.group(6), 16 if m.group(6).startswith("0x") else 10)
            except ValueError:
                addend = 0
            if m.group(5) == "-":
                addend = -addend
        if rtype in GOT_RELOCS:
            got_relocs += 1

        sym = by_name.get(symname)
        kind = classify(symname, sym, rtype, secs)
        # 站点计数：arm64 上一次全局数据引用 = adrp+add 对，只按 adrp(PG_HI21)/x86 记一次
        if kind == "data" and rtype in ("R_AARCH64_ADR_PREL_PG_HI21",
                                        "R_X86_64_PC32", "R_X86_64_32S", "R_X86_64_64"):
            data_sites += 1
        if rtype in ("R_AARCH64_CALL26", "R_AARCH64_JUMP26", "R_X86_64_PLT32"):
            call_sites += 1
        if not include_funcs and kind != "data":
            continue  # 外部函数不进 GOT（§3.4.3 优化一）

        # 归属到包含该 offset 的函数
        sidx = name2idx.get(cur_sec)
        owner = None
        if sidx is not None:
            for f in funcs_in_sec.get(sidx, []):
                if f["value"] <= off < f["value"] + f["size"]:
                    owner = f["name"]
                    break
            if owner is None and len(funcs_in_sec.get(sidx, [])) == 1:
                owner = funcs_in_sec[sidx][0]["name"]
        if owner is None:
            owner = cur_sec  # 落不到具体函数时按节记，保持可归属
        refs[owner].add(dedup_key(symname, sym, obj, addend))

    return refs, got_relocs, sum(len(v) for v in funcs_in_sec.values()), data_sites, call_sites


def dedup_key(name, sym, obj, addend=0):
    """去重键（方案 §2 坑 3）。

    * 全局符号        -> 名字
    * static(LOCAL)   -> (目标文件, 名字)，避免头文件里的 static 被跨文件误合并
    * 节符号(.bss 等) -> (目标文件, 节名, addend)：static 变量常以「节符号+偏移」
      的形式重定位，不带名字；同一节的不同 addend 是**不同的变量**，必须分开计。
    """
    if name.startswith(".") or (sym and sym["type"] == "SECTION"):
        return (obj, name, addend)
    if sym and sym["bind"] == "LOCAL":
        return (obj, name)
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objs", nargs="+", required=True,
                    help="目标文件 glob，可多个，如 'fs/*.o' 'net/core/*.o'")
    ap.add_argument("--build-dir", default=".", help="glob 的相对根目录")
    ap.add_argument("--json", help="把结果写成 JSON")
    ap.add_argument("--include-funcs", action="store_true",
                    help="把外部函数符号也计入 GOT（§3.4.3 优化一之前的朴素方案）")
    args = ap.parse_args()

    files = []
    for pat in args.objs:
        files += sorted(glob.glob(os.path.join(args.build_dir, pat), recursive=True))
    files = [f for f in files if f.endswith(".o") and not f.endswith(".mod.o")
             and "built-in.a" not in f]
    if not files:
        sys.exit("没有匹配到 .o 文件")

    all_refs, per_func_total, nfunc, got_relocs = {}, 0, 0, 0
    tot_data_sites = tot_call_sites = 0
    shared = set()
    for obj in files:
        refs, g, nf, ds, cs = analyze(obj, args.include_funcs)
        tot_data_sites += ds; tot_call_sites += cs
        got_relocs += g
        nfunc += nf
        for fn, s in refs.items():
            key = (obj, fn)
            all_refs[key] = s
            per_func_total += len(s)
            shared |= s

    shared_total = len(shared)
    ratio = (1 - shared_total / per_func_total) if per_func_total else 0.0
    fns_with_refs = len(all_refs)

    print(f"目标文件数              : {len(files)}")
    print(f"函数符号总数            : {nfunc}")
    print(f"引用到全局数据的函数数  : {fns_with_refs}")
    print(f"GOT 类重定位数(C3 才有) : {got_relocs}")
    print()
    print(f"per_func_total (每函数一张): {per_func_total:>8} 项  "
          f"= {per_func_total*8/1024:.1f} KB")
    print(f"shared_total   (全局共享)  : {shared_total:>8} 项  "
          f"= {shared_total*8/1024:.1f} KB")
    print(f"dedup_ratio    (去重率)    : {ratio*100:.2f}%")
    print()
    print(f"全局数据引用**站点**数     : {tot_data_sites:>8}  "
          f"（共享 GOT 下每站点多一次 load）")
    print(f"外部调用站点数             : {tot_call_sites:>8}  "
          f"（arm64 上须改为经跳板/间接跳转）")
    if fns_with_refs:
        print(f"每函数平均引用不同全局数据 : {per_func_total/fns_with_refs:.2f}")

    if args.json:
        json.dump(dict(files=len(files), funcs=nfunc,
                       funcs_with_refs=fns_with_refs,
                       per_func_total=per_func_total,
                       shared_total=shared_total, dedup_ratio=ratio,
                       got_relocs=got_relocs), open(args.json, "w"), indent=2)


if __name__ == "__main__":
    main()
