#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
I-KASLR 跨区域控制流静态扫描器（实验 E2 / 论文 §3.6.5、§6.4）。

论文安全论证中**唯一可被完全验证**的性质：
    编译产物中不存在绕过跳板函数的跨区域控制流转移。
本工具在内核映像（vmlinux）上机械地检验这一性质，产出 §6.4 表 6-3。

区域定义（由链接脚本符号给出，见 include/asm-generic/vmlinux.lds.h）：
    随机化区域 .rand.text  : [__rand_text_start,  __rand_text_end)
    跳板区域   .tramp.text : [__tramp_text_start, __tramp_text_end)
    非随机化区域           : 其余可执行代码

合法的跨区域转移只有两种：
    非随机化/跳板 --(fixed_in)-->  随机化      （进入必经 fixed_in 跳板）
    随机化       --(fixed_out)--> 非随机化/跳板（离开必经 fixed_out 跳板）
即：随机化区域的**唯一出入口是跳板区域**。任何源或目标落在 .rand.text、
而另一端既不在 .rand.text 也不在 .tramp.text 的**直接**转移，都是违规
（编译器本应把它重定向到跳板）。

直接转移（call/jmp/bl/b 到立即地址）可静态定目标，逐条判定。
间接转移（call *reg、blr、br 等）无法静态定目标：按设计，随机化区域内的
间接转移必须是跳板派发本身；工具把随机化区域内的间接转移单独列为“需人工
核对”项，而非直接判违规——因为它们的目标由运行时决定。

用法：
    scan_xregion.py vmlinux
    scan_xregion.py vmlinux --objdump aarch64-linux-gnu-objdump
    scan_xregion.py vmlinux --rand-start 0x... --rand-end 0x... \
                            --tramp-start 0x... --tramp-end 0x...
    scan_xregion.py vmlinux --json report.json

退出码：0 = 无违规（或区域为空）；2 = 发现违规；1 = 用法/环境错误。
"""
import argparse, json, re, subprocess, sys, shutil

# ---- 每架构的控制流指令规则 ------------------------------------------------
# direct: 目标为立即地址、可静态解析的转移助记符
# indirect: 目标为寄存器/内存、无法静态解析的转移助记符
ARCH_RULES = {
    "x86-64": {
        "objdump": "objdump",
        # objdump 直接分支行尾形如 "call 0xffffffff81234560 <sym>"；间接形如 "call *%rax"
        "direct": re.compile(r"^(call|jmp|je|jne|jg|jge|jl|jle|ja|jae|jb|jbe|jo|jno|js|jns|jp|jnp|jc|jnc|jz|jnz|loop|loope|loopne)\b"),
        "indirect": re.compile(r"^(call|jmp)\s+\*"),
    },
    "aarch64": {
        "objdump": "aarch64-linux-gnu-objdump",
        "direct": re.compile(r"^(bl|b|b\.[a-z]+|cbz|cbnz|tbz|tbnz)\b"),
        # blr/br 及其 PAC 变体 braa/brab/blraa/blrab/braaz/... 目标为寄存器
        "indirect": re.compile(r"^(blr|br|blra[a-z]*|bra[a-z]*|blrab|brab)\b"),
    },
}

# objdump -d 反汇编行：地址、字节、助记符/操作数
LINE = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2}\s?)+\s+(\S+)\s*(.*)$")
# 直接分支操作数里的目标地址（x86 与 arm64 objdump 都会把立即目标打成 0x... ）
# 分支目标地址。objdump 的 x86 输出把目标印成**裸十六进制**
# （`jle  ffffffff81d9101e <sym+0xe>`），而符号偏移才带 `0x`（`+0xe`）。
# 旧写法 r"\b0x([0-9a-f]+)\b" 只匹配带 0x 的，于是抓到的是**符号偏移**而非目标：
# 既产生假阳性（区域内分支被判成越界），更严重的是产生**假阴性**——
# 外部代码跳进随机化区域时 dst 永远解析不出 rand，违规会被漏报。
# 因此这里要求至少 6 位十六进制（内核地址 16 位），并取第一个匹配。
TARGET = re.compile(r"(?:^|\s)(?:0x)?([0-9a-f]{6,})\b")


def detect_arch(vmlinux):
    out = subprocess.check_output(["readelf", "-h", vmlinux], text=True)
    if "X86-64" in out:
        return "x86-64"
    if "AArch64" in out:
        return "aarch64"
    sys.exit("scan_xregion: 无法识别架构（仅支持 x86-64 / aarch64）")


def read_symbols(vmlinux):
    """返回 {符号名: 地址} 中我们关心的区域边界符号。"""
    want = ("__rand_text_start", "__rand_text_end",
            "__tramp_text_start", "__tramp_text_end")
    syms = {}
    out = subprocess.check_output(["nm", vmlinux], text=True, errors="replace")
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] in want:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def region_of(addr, rand, tramp):
    if rand and rand[0] <= addr < rand[1]:
        return "rand"
    if tramp and tramp[0] <= addr < tramp[1]:
        return "tramp"
    return "other"


def scan(vmlinux, objdump, rand, tramp, rules):
    """返回 (stats, violations, indirect_in_rand)。"""
    proc = subprocess.Popen(
        [objdump, "-d", "--no-show-raw-insn", "-M", "no-aliases", vmlinux]
        if rules is ARCH_RULES["aarch64"] else
        [objdump, "-d", vmlinux],
        stdout=subprocess.PIPE, text=True, errors="replace")
    stats = dict(total_cf=0, direct=0, indirect=0,
                 xregion=0, via_tramp=0, not_via_tramp=0)
    violations, indirect_in_rand = [], []
    direct_re, indirect_re = rules["direct"], rules["indirect"]

    for raw in proc.stdout:
        m = LINE.match(raw)
        if not m:
            continue
        addr = int(m.group(1), 16)
        mnem, ops = m.group(2), m.group(3).strip()
        insn = (mnem + " " + ops).strip()
        src = region_of(addr, rand, tramp)

        if indirect_re.match(insn):
            stats["total_cf"] += 1
            stats["indirect"] += 1
            # 间接转移无法静态定目标；随机化区域内的须为跳板派发，单列复核
            if src == "rand":
                indirect_in_rand.append((addr, insn))
            continue

        if not direct_re.match(mnem):
            continue
        stats["total_cf"] += 1
        stats["direct"] += 1
        tm = TARGET.search(ops)
        if not tm:
            continue                      # 目标未解析（极少数）——跳过
        dst_addr = int(tm.group(1), 16)
        dst = region_of(dst_addr, rand, tramp)

        # 判定跨区域：只要一端是 rand、另一端不是 rand，就是跨越随机化边界
        involves_rand = (src == "rand") ^ (dst == "rand")
        if not involves_rand:
            continue
        stats["xregion"] += 1
        # 合法当且仅当“另一端”是跳板：进入经 fixed_in（src=tramp→dst=rand），
        # 离开经 fixed_out（src=rand→dst=tramp）。
        other = dst if src == "rand" else src
        if other == "tramp":
            stats["via_tramp"] += 1
        else:
            stats["not_via_tramp"] += 1
            violations.append(dict(addr=hex(addr), insn=insn,
                                   src=src, dst=dst, target=hex(dst_addr)))
    proc.wait()
    return stats, violations, indirect_in_rand


def main():
    ap = argparse.ArgumentParser(description="I-KASLR 跨区域控制流静态扫描器 (E2)")
    ap.add_argument("vmlinux")
    ap.add_argument("--objdump", help="objdump 路径（默认按架构自动选）")
    for s in ("rand-start", "rand-end", "tramp-start", "tramp-end"):
        ap.add_argument("--" + s, type=lambda x: int(x, 0),
                        help="手动指定区域边界（覆盖符号）")
    ap.add_argument("--json", help="把完整报告写入该 JSON 文件")
    a = ap.parse_args()

    arch = detect_arch(a.vmlinux)
    rules = ARCH_RULES[arch]
    objdump = a.objdump or rules["objdump"]
    if not shutil.which(objdump):
        sys.exit(f"scan_xregion: 找不到 {objdump}（{arch} 需要它）")

    syms = read_symbols(a.vmlinux)
    rs = a.rand_start if a.rand_start is not None else syms.get("__rand_text_start")
    re_ = a.rand_end if a.rand_end is not None else syms.get("__rand_text_end")
    ts = a.tramp_start if a.tramp_start is not None else syms.get("__tramp_text_start")
    te = a.tramp_end if a.tramp_end is not None else syms.get("__tramp_text_end")
    rand = (rs, re_) if rs is not None and re_ is not None and re_ > rs else None
    tramp = (ts, te) if ts is not None and te is not None and te > ts else None

    print(f"# I-KASLR 跨区域控制流静态扫描 (E2)")
    print(f"# 映像: {a.vmlinux}  架构: {arch}  objdump: {objdump}")
    if not rand:
        print("# 随机化区域 .rand.text 为空或无边界符号 —— 无需扫描。")
        print("# （该内核未启用 IKASLR/CKASLR，或 LLVM pass 尚未填充随机化区域。）")
        if a.json:
            json.dump({"arch": arch, "rand_region": None}, open(a.json, "w"),
                      ensure_ascii=False, indent=2)
        return 0
    print(f"# 随机化区域: {hex(rand[0])}–{hex(rand[1])} ({rand[1]-rand[0]} 字节)")
    print(f"# 跳板区域:   {hex(tramp[0])}–{hex(tramp[1])}" if tramp else "# 跳板区域:   无")
    print()

    stats, violations, indirect = scan(a.vmlinux, objdump, rand, tramp, rules)

    print("表 6-3　跨区域控制流转移的静态验证结果")
    print(f"  扫描的控制流转移指令总数      : {stats['total_cf']:,}")
    print(f"    其中直接转移                : {stats['direct']:,}")
    print(f"    其中间接转移                : {stats['indirect']:,}")
    print(f"  跨随机化边界的（直接）转移    : {stats['xregion']:,}")
    print(f"    经由跳板                    : {stats['via_tramp']:,}")
    print(f"    未经跳板（应为 0）          : {stats['not_via_tramp']:,}")
    print(f"  随机化区域内的间接转移（复核）: {len(indirect):,}")
    print()

    if violations:
        print(f"!! 发现 {len(violations)} 处绕过跳板的直接跨区域转移：")
        for v in violations[:50]:
            print(f"   {v['addr']}: {v['insn']}  [{v['src']}→{v['dst']} @ {v['target']}]")
        if len(violations) > 50:
            print(f"   … 另有 {len(violations)-50} 处，完整清单见 --json")
    else:
        print("OK：不存在绕过跳板的直接跨区域转移。")

    if indirect:
        print(f"\n需人工核对：随机化区域内 {len(indirect)} 处间接转移"
              f"（应全部为跳板派发；按设计编译器已将其重定向到 fixed_out）：")
        for addr, insn in indirect[:20]:
            print(f"   {hex(addr)}: {insn}")
        if len(indirect) > 20:
            print(f"   … 另有 {len(indirect)-20} 处，完整清单见 --json")

    print("\n注意（论文 §6.4 W-64 要求单独交代，本工具不覆盖）：")
    print("  · 手写汇编中的跳转/调用不经编译器 Pass，需另行扫描其目标文件；")
    print("  · 运行时生成的代码（BPF JIT、alternative、static key、text_poke）")
    print("    在编译期不存在，其是否产生跨区域转移需运行时约束，不在本工具范围。")

    if a.json:
        json.dump(dict(arch=arch, rand_region=[hex(rand[0]), hex(rand[1])],
                       tramp_region=[hex(tramp[0]), hex(tramp[1])] if tramp else None,
                       stats=stats, violations=violations,
                       indirect_in_rand=[[hex(a2), i] for a2, i in indirect]),
                  open(a.json, "w"), ensure_ascii=False, indent=2)
        print(f"\n完整报告 -> {a.json}")

    return 2 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
