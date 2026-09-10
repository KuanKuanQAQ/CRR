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

对比用法（第 6 章）：对部署 I-KASLR 前后的映像各跑一次。

三桶模式（E4-C，`--buckets`）
----------------------------
E4-C 的口径不是"随机化前后各有多少 gadget"。原口径隐含假设"随机化区域内的代码
攻击者用不了"，**这个假设不成立**：fixed_in 跳板位于固定地址，知道跳板地址就能
经它调用区域内的函数。因此残余攻击面要按**地址是否稳定**与**可用粒度**分三桶：

  桶 1  非随机化区域          地址稳定  任意 mid-function gadget，可直接串 ROP
  桶 2  跳板区域自身          地址稳定  任意 mid-function gadget
  桶 3  经跳板可达的区域代码  入口稳定  **只有函数入口**，即整函数复用，不是 ROP

桶 3 的粒度区分是关键：ROP 需要以 ret 结尾的短序列（mid-function gadget），
而跳板只暴露函数入口。所以桶 3 对应 return-to-libc 式的整函数复用。
本脚本因此对桶 3 同时报两个数：入口数（攻击者真正能用的）与区域内的
mid-function gadget 数（攻击者**用不到**的，报出来是为了让这个区分可量化）。
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

# 桶 3 里若存在这些函数的入口，攻击者经跳板就能整函数复用它们。
# 只列**单独调用即可推进提权或绕过**的，不列"配合别的东西才有用"的。
DANGEROUS = {
    "commit_creds":        "提交任意 cred，配合 prepare_kernel_cred 即为完整提权",
    "prepare_kernel_cred": "构造 root cred",
    "__x64_sys_setuid":    "系统调用入口，可直接改 uid",
    "call_usermodehelper": "以内核态发起用户态程序执行",
    "run_cmd":             "同上，参数更简单",
    "set_memory_x":        "把任意内核页改成可执行",
    "set_memory_rw":       "把只读内核页改成可写",
    "kernel_write":        "任意文件写",
    "vfs_write":           "任意文件写（需先有 file*）",
    "security_bprm_creds_for_exec": "LSM 挂钩，可用于绕过",
    "__request_module":    "触发模块加载",
    "msleep":              "无害，仅用于自检本清单确实生效",
}


def region_bounds(vmlinux, nm="nm"):
    """从符号表取各区域边界。返回 {名: (lo, hi)}，缺失的键不出现。"""
    syms = {}
    for ln in subprocess.run([nm, vmlinux], capture_output=True,
                             text=True, errors="replace").stdout.splitlines():
        p = ln.split()
        if len(p) == 3:
            syms.setdefault(p[2], int(p[0], 16))
    out = {}
    for name, a, b in (("text", "_stext", "_etext"),
                       ("tramp", "__tramp_text_start", "__tramp_text_end"),
                       ("rand", "__rand_text_start", "__rand_text_end")):
        if a in syms and b in syms:
            out[name] = (syms[a], syms[b])
    return out, syms


def randomized_funcs(vmlinux, nm="nm"):
    """区域内的函数体符号（<fn>_body）。返回 {原函数名: 地址}。"""
    _, syms = region_bounds(vmlinux, nm)
    lo, hi = syms.get("__rand_text_start", 0), syms.get("__rand_text_end", 0)
    return {n[:-5]: a for n, a in syms.items()
            if n.endswith("_body") and lo <= a < hi}


def in_range(addr, rng):
    return rng and rng[0] <= addr < rng[1]


def report_buckets(items, vmlinux, nm):
    """items: [(地址, gadget 文本, 大类, 细类)]"""
    regions, syms = region_bounds(vmlinux, nm)
    if "rand" not in regions:
        sys.exit("这个映像没有 __rand_text_start：不是启用了 I-KASLR 的内核？")

    buckets = {1: [], 2: [], 3: [], 0: []}   # 0 = 内核代码段之外（数据段等）
    for addr, g, c, sub_ in items:
        if in_range(addr, regions.get("tramp")):
            buckets[2].append((addr, g, c))
        elif in_range(addr, regions.get("rand")):
            buckets[3].append((addr, g, c))
        elif in_range(addr, regions.get("text")):
            buckets[1].append((addr, g, c))
        else:
            buckets[0].append((addr, g, c))

    def usable(lst):
        return [x for x in lst if x[2] in ("ROP", "JOP", "COP", "SYS")]

    print("\n" + "=" * 74)
    print("E4-C　攻击者可用 gadget 的残余集合（三桶）")
    print("=" * 74)
    for name, rng in (("非随机化代码段", regions.get("text")),
                      ("跳板区域 .tramp.text", regions.get("tramp")),
                      ("随机化区域 .rand.text", regions.get("rand"))):
        if rng:
            print(f"  {name:<24} [{rng[0]:#x}, {rng[1]:#x})  {rng[1]-rng[0]:>9,} B")

    rf = randomized_funcs(vmlinux, nm)
    print(f"\n{'桶':<4}{'内容':<26}{'地址稳定':<10}{'可用粒度':<26}{'可利用 gadget':>14}")
    print("-" * 84)
    print(f"{'1':<4}{'非随机化区域':<26}{'稳定':<10}{'任意 mid-function gadget':<26}"
          f"{len(usable(buckets[1])):>14,}")
    print(f"{'2':<4}{'跳板区域自身':<26}{'稳定':<10}{'任意 mid-function gadget':<26}"
          f"{len(usable(buckets[2])):>14,}")
    print(f"{'3':<4}{'经跳板可达的区域代码':<26}{'入口稳定':<10}{'仅函数入口（整函数复用）':<26}"
          f"{len(rf):>14,}")
    print("-" * 84)
    print(f"{'':<4}{'（桶 3 区域内的 mid-function gadget，攻击者用不到）':<62}"
          f"{len(usable(buckets[3])):>14,}")
    if buckets[0]:
        print(f"{'':<4}{'（落在代码段之外，非本实验对象）':<62}"
              f"{len(usable(buckets[0])):>14,}")

    # 桶 2 的密度：跳板体短且形式单一，但数量要如实数
    ntramp = len(rf)
    if ntramp and regions.get("tramp"):
        tb = regions["tramp"][1] - regions["tramp"][0]
        print(f"\n  桶 2 密度：{len(usable(buckets[2])):,} 个 gadget / {ntramp} 个跳板"
              f" = {len(usable(buckets[2]))/ntramp:.2f} 个/跳板"
              f"（跳板区共 {tb:,} B，{tb/ntramp:.0f} B/跳板）")

    # 桶 3 里的危险整函数入口
    hit = {n: why for n, why in DANGEROUS.items() if n in rf}
    print(f"\n  桶 3 中的危险整函数入口：{len(hit)} 个")
    for n, why in sorted(hit.items()):
        print(f"    {n:<32} {why}")
    if not hit:
        print("    （无。但这取决于随机化范围的选取，换范围要重测）")

    # 仅凭桶 1 + 桶 2 能拼出什么：ROP 链的必要件
    stable = usable(buckets[1]) + usable(buckets[2])
    need = {
        "pop rdi ; ret":  re.compile(r"^pop rdi ; ret$"),
        "pop rsi ; ret":  re.compile(r"^pop rsi ; ret$"),
        "pop rdx ; ret":  re.compile(r"^pop rdx ; ret$"),
        "pop rax ; ret":  re.compile(r"^pop rax ; ret$"),
        "任意内存写":     re.compile(r"^mov (qword |dword )?ptr \[r\w+\][^;]*, r\w+ ; ret$"),
        "间接调用":       re.compile(r"^(jmp|call) r\w+$"),
        "返回用户态":     re.compile(r"\b(swapgs|iretq|sysretq)\b"),
    }
    print("\n  仅凭稳定地址（桶 1 + 桶 2）能否拼出可用链：")
    for label, rx in need.items():
        n = sum(1 for _, g, _ in stable if rx.search(g))
        print(f"    {label:<16}{'有' if n else '无':<4}{n:>8,} 处")
    print("\n  结论口径：本文**不声称**随机化后攻击者无 gadget 可用。上表说明的是"
          "\n  残余集合有多大、粒度是什么；桶 3 只暴露函数入口，因此对应的是整函数"
          "\n  复用而非 ROP。经跳板的整函数调用由白名单与第 4 章审计约束。")

def main():
    ap = argparse.ArgumentParser(description="ROP/JOP/COP gadget 普查")
    ap.add_argument("vmlinux", nargs="?")
    ap.add_argument("--gadgets", help="复用已有的 ROPgadget 输出文件")
    ap.add_argument("--ropgadget", default="ROPgadget")
    ap.add_argument("--json")
    ap.add_argument("--buckets", action="store_true",
                    help="按区域分三桶报告（E4-C）；需要 vmlinux 以读取符号表")
    ap.add_argument("--nm", default="nm", help="交叉编译时给 aarch64-linux-gnu-nm")
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
    items = []          # 分桶用：(地址, gadget, 大类, 细类)

    for raw in lines:
        m = LINE.match(raw.strip())
        if not m:
            continue
        g = m.group(2).strip()
        c, s = classify(g)
        if c is None:
            continue
        total += 1
        items.append((int(m.group(1), 16), g, c, s))
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

    if a.buckets:
        if not a.vmlinux:
            sys.exit("--buckets 需要 vmlinux（要读符号表定区域边界）")
        report_buckets(items, a.vmlinux, a.nm)

    if a.json:
        json.dump(dict(total=total, usable=usable, categories=dict(cat),
                       subtypes=dict(sub), rop_lead=dict(lead),
                       pop_reg=dict(popreg), lengths=dict(length)),
                  open(a.json, "w"), ensure_ascii=False, indent=2)
        print(f"\n-> {a.json}")

if __name__ == "__main__":
    sys.exit(main())
