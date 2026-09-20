#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
D1 代码索引更新量的收敛（论文 §3.6.2）。

论证 O(1) 索引：传统重定位每次随机化要修正**所有指向被移动代码的引用**（调用点、
函数指针、GOT 项…），量为 O(调用点数)；本机制下所有引用都指向**固定地址的跳板**，
每个函数只有一个 target 槽随迁移更新，量 = 函数数，与调用点数无关。

本脚本在内核映像上统计,并**按索引类型分列**(清单 §E3-1 的硬要求:只报总百分比
会被追问"剩下的 3.1% 是什么")：

  代码中的直接转移   call/jmp/bl/b <跳板地址>
  代码中的取地址     lea <跳板>(%rip) / movabs $<跳板> / adrp+add
  数据中的函数指针   ops 表等,8 字节值等于跳板地址(反汇编看不到,单独扫数据段)
  栈上的返回地址     **运行时量,静态数不出来**——如实说明,不编数字
  GOT 项             x86-64 用 movabs 绝对寻址,**无 GOT**;arm64 是随函数搬移的
                     字面量池,也不需要在随机化时更新

收敛率 = 1 − 函数数 / 需改写的引用总数。

用法: index_convergence.py vmlinux [--objdump ...] [--json out.json]
"""
import argparse, json, re, subprocess, sys, shutil

def sh(*a): return subprocess.check_output(a, text=True, errors="replace")

def detect(vmlinux):
    o = sh("readelf","-h",vmlinux)
    return ("x86-64","objdump") if "X86-64" in o else ("aarch64","aarch64-linux-gnu-objdump")

def rand_funcs(vmlinux):
    """从跳板表符号找出随机化函数名（__ikaslr_target_<fn> / <fn>_body）。"""
    names=set()
    for line in sh("nm",vmlinux).splitlines():
        p=line.split()
        if len(p)>=3 and p[2].endswith("_body") and p[1] in "tT":
            names.add(p[2][:-5])
    return sorted(names)

def sym_addrs(vmlinux):
    a={}
    for line in sh("nm",vmlinux).splitlines():
        p=line.split()
        if len(p)>=3:
            try: a[p[2]]=int(p[0],16)
            except ValueError: pass
    return a


def scan_code(vmlinux, od, tramp_by_addr):
    """反汇编全映像，按指令类型统计对跳板地址的引用。

    返回 {类型: {函数名: 次数}}。
    """
    from collections import defaultdict
    out = {"direct": defaultdict(int), "lea": defaultdict(int),
           "other": defaultdict(int)}
    # 目标地址在 x86 objdump 里裸写（call ffffffff81eb0000），
    # 而符号偏移带 0x 前缀，因此两种都要认，且要求至少 6 位十六进制。
    DIRECT = re.compile(r"^\s*(call|callq|jmp|jmpq|bl|b|b\.\w+)\s+(?:0x)?([0-9a-f]{6,16})\b")
    LEA    = re.compile(r"^\s*(lea|leaq|movabs|movabsq|mov|adrp|add)\b.*?(?:\$?0x)?([0-9a-f]{6,16})\b")
    p = subprocess.Popen([od, "-d", "--no-show-raw-insn", vmlinux],
                         stdout=subprocess.PIPE, text=True, errors="replace")
    for line in p.stdout:
        if "\t" not in line:
            continue
        insn = line.split("\t", 1)[1]
        m = DIRECT.match(insn)
        if m:
            v = int(m.group(2), 16)
            if v in tramp_by_addr:
                out["direct"][tramp_by_addr[v]] += 1
            continue
        m = LEA.match(insn)
        if m:
            v = int(m.group(2), 16)
            if v in tramp_by_addr:
                out["lea"][tramp_by_addr[v]] += 1
            continue
        # 兜底：任何其他指令里出现跳板地址
        for tok in re.findall(r"(?:0x)?([0-9a-f]{6,16})\b", insn):
            v = int(tok, 16)
            if v in tramp_by_addr:
                out["other"][tramp_by_addr[v]] += 1
                break
    p.wait()
    return out


def scan_data(vmlinux, tramp_by_addr):
    """扫可写/只读数据段里的 8 字节函数指针（ops 表等）。

    反汇编看不到这些——它们是数据。传统重定位方案每次随机化都要改写它们，
    因此必须计入"传统方法要改的量"。

    直接用 pyelftools 读节内容：走 objcopy + 文本管道会把二进制当字符串解码，
    非 latin-1 的字节直接抛异常（踩过）。
    """
    from collections import defaultdict
    try:
        from elftools.elf.elffile import ELFFile
    except ImportError:
        print("  （缺 pyelftools，跳过数据段扫描；pip install pyelftools）")
        return defaultdict(int)

    hits = defaultdict(int)
    with open(vmlinux, "rb") as f:
        elf = ELFFile(f)
        for sec in elf.iter_sections():
            # 只看已分配、非可执行、有内容的节：函数指针表都在这里
            fl = sec["sh_flags"]
            if not (fl & 0x2) or (fl & 0x4):        # SHF_ALLOC / SHF_EXECINSTR
                continue
            if sec["sh_type"] == "SHT_NOBITS":      # .bss 没有文件内容
                continue
            b = sec.data()
            for i in range(0, len(b) - 7, 8):
                v = int.from_bytes(b[i:i + 8], "little")
                if v in tramp_by_addr:
                    hits[tramp_by_addr[v]] += 1
    return hits


def main():
    ap=argparse.ArgumentParser(description="D1 索引更新收敛 (§3.6.2)")
    ap.add_argument("vmlinux"); ap.add_argument("--objdump"); ap.add_argument("--json")
    a=ap.parse_args()
    arch,od=detect(a.vmlinux); od=a.objdump or od
    if not shutil.which(od): sys.exit(f"找不到 {od}")

    funcs=rand_funcs(a.vmlinux)
    if not funcs:
        print("# 未找到随机化函数（内核未启用 IKASLR 或无注解函数）"); return 0
    addrs=sym_addrs(a.vmlinux)
    # 跳板符号就是原函数名 fn；统计对每个 fn 的引用（call/branch/lea 到该地址）
    tramp_addr={f:addrs[f] for f in funcs if f in addrs}

    tramp_by_addr = {v: f for f, v in tramp_addr.items()}
    code = scan_code(a.vmlinux, od, tramp_by_addr)
    data = scan_data(a.vmlinux, tramp_by_addr)

    n_direct = sum(code["direct"].values())
    n_lea = sum(code["lea"].values())
    n_other = sum(code["other"].values())
    n_data = sum(data.values())
    nfuncs = len(tramp_addr)
    total = n_direct + n_lea + n_other + n_data

    print(f"# D1 索引更新收敛  映像:{a.vmlinux}  架构:{arch}")
    print(f"# 随机化函数数: {nfuncs}\n")
    print(f"{'索引类型':<34}{'传统方法要改':>14}{'本机制要改':>12}")
    print("-" * 60)
    print(f"{'代码中的直接转移 call/jmp/bl':<34}{n_direct:>14,}{0:>12}")
    print(f"{'代码中的取地址 lea/movabs/adrp':<34}{n_lea:>14,}{0:>12}")
    print(f"{'其他指令中出现的跳板地址':<34}{n_other:>14,}{0:>12}")
    print(f"{'数据中的函数指针（ops 表等）':<34}{n_data:>14,}{0:>12}")
    print(f"{'GOT 项':<34}{0:>14}{0:>12}")
    print(f"{'target 槽':<34}{'—':>14}{nfuncs:>12,}")
    print("-" * 60)
    print(f"{'合计':<34}{total:>14,}{nfuncs:>12,}")
    print()
    print("  栈上的返回地址：**静态数不出来**，它是运行时量，与当时正在执行的")
    print("  调用点数同阶。本机制不把它算作索引更新——退役变体填陷阱指令，")
    print("  陈旧返回由 ikaslr_fixup_addr() 重定向（§3.5.5），不需要遍历栈。")
    print()
    if total:
        conv = 1 - nfuncs / total
        print(f"收敛率 = 1 − {nfuncs}/{total:,} = {conv * 100:.1f}%")
    print()
    print(f"{'引用最多的 10 个函数':<34}{'直接转移':>10}{'取地址':>10}{'数据指针':>10}")
    allf = sorted(tramp_addr, key=lambda f: -(code['direct'][f] + code['lea'][f]
                                              + data[f]))
    for f in allf[:10]:
        print(f"  {f:<32}{code['direct'][f]:>10,}{code['lea'][f]:>10,}"
              f"{data[f]:>10,}")
    print()

    if a.json:
        json.dump(dict(arch=arch, funcs=nfuncs, direct=n_direct, lea=n_lea,
                       other=n_other, data=n_data, total_refs=total),
                  open(a.json,"w"),ensure_ascii=False,indent=2)
        print(f"-> {a.json}")

if __name__=="__main__": sys.exit(main())
