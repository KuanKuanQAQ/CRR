#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
ROP/JOP/COP gadget 普查（论文第 2 章基线、§4.6.6、§6.8）。

把 ROPgadget 的输出按**终止指令**分类，区分真正可利用的 gadget 与噪声。

⚠ 关键口径问题：ROPgadget 7.x 默认把 `jmp <立即地址>` 也算作 gadget。这类跳转
   目标在编译期定死，攻击者无法借它转移控制流，对利用毫无价值——却能占到原始
   计数的近八成。直接引用 ROPgadget 的 "Unique gadgets found" 会把结论放大数倍。
   本脚本因此分开报告「可利用」与「噪声」，论文应引用前者。

用法:
    gadget_census.py vmlinux [--ropgadget /path/to/ROPgadget] [--json out.json]
    gadget_census.py --gadgets dump.txt          # 复用已有的 ROPgadget 输出

对比用法（第 6 章）：对部署 I-KASLR 前后的映像各跑一次，并把跳板区域单列
（§3.8 要求如实报告跳板本身贡献的 gadget）。
"""
import argparse, json, re, subprocess, sys, shutil
from collections import Counter

LINE = re.compile(r"^0x([0-9a-f]+)\s*:\s*(.+)$")

def classify(gadget):
    """按终止指令归类，返回 (大类, 细类)。"""
    insns = [i.strip() for i in gadget.split(';') if i.strip()]
    if not insns:
        return None, None
    last = insns[-1]

    if last == 'ret':
        return 'ROP', 'ret'
    if re.match(r'^ret\s+0x', last):
        return 'ROP', 'ret imm (retn)'
    if last.startswith('retf'):
        return 'NOISE', 'retf (far ret)'
    if re.match(r'^(notrack )?jmp\s+(r[a-z0-9]+|e[a-z]+)$', last):
        return 'JOP', 'jmp reg'
    if re.match(r'^(notrack )?jmp\s+(qword |dword )?ptr \[', last):
        return 'JOP', 'jmp [mem]'
    if re.match(r'^call\s+(r[a-z0-9]+|e[a-z]+)$', last):
        return 'COP', 'call reg'
    if re.match(r'^call\s+(qword |dword )?ptr \[', last):
        return 'COP', 'call [mem]'
    if last in ('syscall', 'sysenter') or last.startswith('int '):
        return 'SYS', 'syscall/sysenter/int'
    if re.match(r'^jmp\s+0x', last):
        return 'NOISE', 'jmp imm (fixed target)'
    if re.match(r'^call\s+0x', last):
        return 'NOISE', 'call imm (fixed target)'
    if last.startswith('iret'):
        return 'NOISE', 'iret'
    return 'NOISE', 'other'

def lead_family(gadget):
    """ROP gadget 的首指令族，用于「按类型分列」。"""
    first = gadget.split(';')[0].strip()
    op = first.split()[0] if first else '?'
    fam = {'pop': 'pop', 'push': 'push', 'mov': 'mov', 'movabs': 'mov',
           'add': 'add/sub', 'sub': 'add/sub', 'adc': 'add/sub', 'sbb': 'add/sub',
           'xor': 'xor/and/or', 'and': 'xor/and/or', 'or': 'xor/and/or',
           'lea': 'lea', 'xchg': 'xchg', 'inc': 'inc/dec', 'dec': 'inc/dec',
           'leave': 'leave', 'nop': 'nop', 'ret': 'ret-only',
           'test': 'test/cmp', 'cmp': 'test/cmp',
           'shl': 'shift', 'shr': 'shift', 'sar': 'shift', 'sal': 'shift',
           'rol': 'shift', 'ror': 'shift',
           'neg': 'neg/not', 'not': 'neg/not'}
    return fam.get(op, op)

POPREG = re.compile(r'^pop (rax|rbx|rcx|rdx|rsi|rdi|rbp|rsp|r8|r9|r1[0-5]) ; ret$')

def main():
    ap = argparse.ArgumentParser(description="ROP/JOP/COP gadget 普查")
    ap.add_argument("vmlinux", nargs="?")
    ap.add_argument("--gadgets", help="复用已有的 ROPgadget 输出文件")
    ap.add_argument("--ropgadget", default="ROPgadget")
    ap.add_argument("--json")
    a = ap.parse_args()

    if a.gadgets:
        lines = open(a.gadgets, errors="replace").read().splitlines()
    else:
        if not a.vmlinux:
            sys.exit("用法: gadget_census.py vmlinux | --gadgets dump.txt")
        if not shutil.which(a.ropgadget):
            sys.exit(f"找不到 {a.ropgadget}（pip install ROPgadget）")
        print(f"# 正在扫描 {a.vmlinux} …", file=sys.stderr)
        lines = subprocess.check_output([a.ropgadget, "--binary", a.vmlinux],
                                        text=True, errors="replace").splitlines()

    cat = Counter(); sub = Counter(); lead = Counter()
    popreg = Counter(); length = Counter(); total = 0

    for raw in lines:
        m = LINE.match(raw.strip())
        if not m:
            continue
        g = m.group(2).strip()
        c, s = classify(g)
        if c is None:
            continue
        total += 1
        cat[c] += 1; sub[s] += 1
        length[len([i for i in g.split(';') if i.strip()])] += 1
        if c == 'ROP':
            lead[lead_family(g)] += 1
            pm = POPREG.match(g)
            if pm:
                popreg[g] += 1

    usable = cat['ROP'] + cat['JOP'] + cat['COP'] + cat['SYS']
    print(f"\n原始 unique gadget 总数 : {total:,}")
    print(f"可利用小计               : {usable:,}  ({100.0*usable/total:.1f}%)")
    print(f"噪声（固定目标/远返回等） : {cat['NOISE']:,}  ({100.0*cat['NOISE']/total:.1f}%)")

    print("\n按 gadget 类型分列")
    print(f"  {'类型':<28}{'数量':>10} {'占全部':>8}")
    for k in ('ret', 'ret imm (retn)', 'jmp reg', 'jmp [mem]',
              'call reg', 'call [mem]', 'syscall/sysenter/int',
              'retf (far ret)', 'jmp imm (fixed target)',
              'call imm (fixed target)', 'iret', 'other'):
        if sub[k]:
            print(f"  {k:<28}{sub[k]:>10,} {100.0*sub[k]/total:>7.1f}%")
    print(f"\n  归并: ROP={cat['ROP']:,}  JOP={cat['JOP']:,}  "
          f"COP={cat['COP']:,}  SYS={cat['SYS']:,}")

    if lead:
        print("\n可利用 ROP gadget 按首指令族")
        for k, v in lead.most_common(12):
            print(f"  {k:<20}{v:>10,} {100.0*v/cat['ROP']:>7.1f}%")

    if popreg:
        print("\n最高价值：pop <reg> ; ret")
        for k, v in popreg.most_common():
            print(f"  {k:<24}{v:>6,}")

    if a.json:
        json.dump(dict(total=total, usable=usable, categories=dict(cat),
                       subtypes=dict(sub), rop_lead=dict(lead),
                       pop_reg=dict(popreg), lengths=dict(length)),
                  open(a.json, "w"), ensure_ascii=False, indent=2)
        print(f"\n-> {a.json}")

if __name__ == "__main__":
    sys.exit(main())
