#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""校验随机化区域内的函数体是否**真的可搬移**，并列出不合格的函数。

判据（见 Documentation/crr/实现/16-position-independence.md §8）：
函数体内不得存在**指向区域外的 PC 相对引用**，否则一搬就错。

  x86-64 : 任何 `(%rip)`；任何 `call/jmp <绝对地址>` 且目标在区域外
  AArch64: 任何 `adrp`；任何 `bl/b <绝对地址>` 且目标在区域外
           （`ldr xN, <区域内地址>` 是随函数搬移的字面量池，合法）

为什么需要它：编译器插件能改写的只是 IR 里看得见的引用。有几类**改不到**：
  · per-CPU 访问（`%gs:`／`tpidr` 形式，偏移须为常量）
  · 内联汇编里的全局内存操作数（如 `lock bts` 位操作）——改了会破坏约束
  · 后端自行生成的调用
`gen_funcs.py` 会尽力按重定位排除，但**静态启发式必然有漏网**。因此正确的做法是
**以本校验为准**：编一次 → 扫出不合格函数 → 从名单里剔除 → 重编，直到为 0。

用法：
  verify_movable.py <vmlinux> [--arch x86_64|arm64] [--exclude-list out.txt]
退出码：0 = 全部合格；1 = 存在不合格函数。
"""
import argparse
import re
import subprocess
import sys


def nm_syms(vmlinux, nm):
    out = {}
    for ln in subprocess.run([nm, vmlinux], capture_output=True, text=True).stdout.splitlines():
        p = ln.split()
        if len(p) == 3:
            out.setdefault(p[2], int(p[0], 16))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vmlinux")
    ap.add_argument("--arch", default="x86_64", choices=["x86_64", "arm64"])
    ap.add_argument("--exclude-list", help="把不合格的函数名写到这个文件")
    args = ap.parse_args()

    if args.arch == "arm64":
        nm, objdump = "aarch64-linux-gnu-nm", "aarch64-linux-gnu-objdump"
    else:
        nm, objdump = "nm", "objdump"

    syms = nm_syms(args.vmlinux, nm)
    if "__rand_text_start" not in syms:
        sys.exit("没有 __rand_text_start：这个映像没启用 I-KASLR？")
    lo, hi = syms["__rand_text_start"], syms["__rand_text_end"]

    out = subprocess.run([objdump, "-d", "--start-address", hex(lo),
                          "--stop-address", hex(hi), args.vmlinux],
                         capture_output=True, text=True).stdout

    cur = None
    bad = {}          # 函数 -> [(原因, 指令)]
    nfunc = 0
    for ln in out.splitlines():
        m = re.match(r"^([0-9a-f]+) <(\S+)>:", ln)
        if m:
            cur = m.group(2)
            nfunc += 1
            continue
        if not cur or "\t" not in ln:
            continue
        insn = ln.split("\t")[-1].strip()

        why = None
        if args.arch == "x86_64":
            if "(%rip)" in insn:
                why = "RIP 相对寻址"
            else:
                m2 = re.match(r"^(call|jmp)\s+([0-9a-f]{6,})", insn)
                if m2 and not (lo <= int(m2.group(2), 16) < hi):
                    why = "到区域外的直接调用/跳转"
        else:
            if re.match(r"^adrp\b", insn):
                why = "adrp（PC 相对页寻址）"
            else:
                m2 = re.match(r"^(bl|b)\s+([0-9a-f]{6,})", insn)
                if m2 and not (lo <= int(m2.group(2), 16) < hi):
                    why = "到区域外的直接分支"
        if why:
            bad.setdefault(cur, []).append((why, insn))

    print(f"随机化区域 [{lo:#x},{hi:#x})  {hi - lo} B，函数体 {nfunc} 个")
    if not bad:
        print("✅ 全部可搬移：无指向区域外的 PC 相对引用")
        return 0

    print(f"❌ 不可搬移的函数：{len(bad)} / {nfunc}\n")
    for f, v in sorted(bad.items()):
        print(f"  {f}")
        print(f"      {v[0][0]}: {v[0][1]}")
    # 函数体符号名是 <fn>_body，剔除时要还原成原名
    names = sorted(f[:-5] if f.endswith("_body") else f for f in bad)
    if args.exclude_list:
        with open(args.exclude_list, "w") as fh:
            fh.write("\n".join(names) + "\n")
        print(f"\n已写出待剔除函数名 -> {args.exclude_list}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
