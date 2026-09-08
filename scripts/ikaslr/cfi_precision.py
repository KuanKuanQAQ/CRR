#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
CFI 精度基线测量（论文 W-14 / W-15、§2.5、§5.6.2）。

产出三组数字：

  1. **IBT 等价类大小**（x86）：CET-IBT 只检查间接分支目标处有无 endbr64，因此
     所有带 endbr64 的位置构成**单一等价类**。报告编译器插入的总数、objtool
     封印（改写为 nop）的数量、以及封印后剩余的数量——剩余数就是等价类大小。
     ARM 上的对应物是 BTI 指令。
  2. **kCFI 等价类分布**：按函数类型哈希分组。优先用 __kcfi_typeid_ 符号
     （编译器实际使用的划分）；没有则回退到 DWARF 原型推断。
  3. 供论文表格直接填写的汇总。

用法:
    cfi_precision.py vmlinux [--json out.json]

注意口径：DWARF 途径统计的是"所有有实体的函数"，而 kCFI 的等价类只应包含
**可能被间接调用**的函数，分母不同，数字会偏大。正文须写明用的是哪一种。
"""
import argparse, json, re, subprocess, sys, shutil, os

def sh(*a):
    return subprocess.check_output(a, text=True, errors="replace")

def detect_arch(vmlinux):
    out = sh("readelf", "-h", vmlinux)
    if "X86-64" in out:
        return "x86-64"
    if "AArch64" in out:
        return "aarch64"
    sys.exit("cfi_precision: 仅支持 x86-64 / aarch64")

def section_size(vmlinux, name):
    """返回节的字节数，找不到返回 None。"""
    for line in sh("readelf", "-SW", vmlinux).splitlines():
        # [25] .ibt_endbr_seal   PROGBITS  addr off size ...
        m = re.search(r"\]\s+(\S+)\s+\S+\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)", line)
        if m and m.group(1) == name:
            return int(m.group(2), 16)
    return None

def count_insn(vmlinux, objdump, mnemonic):
    """数反汇编中某助记符出现的次数。"""
    p = subprocess.Popen([objdump, "-d", "--no-show-raw-insn", vmlinux],
                         stdout=subprocess.PIPE, text=True, errors="replace")
    pat = re.compile(r"\b" + mnemonic + r"\b")
    n = 0
    for line in p.stdout:
        if pat.search(line):
            n += 1
    p.wait()
    return n

def kcfi_from_symbols(vmlinux):
    """途径 B：__kcfi_typeid_ 符号的值即类型哈希，按哈希分组得等价类。"""
    hist = {}
    try:
        out = sh("nm", vmlinux)
    except subprocess.CalledProcessError:
        return None
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[2].startswith("__kcfi_typeid_"):
            hist[p[0]] = hist.get(p[0], 0) + 1
    return hist or None

def kcfi_from_dwarf(vmlinux):
    """途径 A：由 DWARF 原型分组。需要 pyelftools 与 CONFIG_DEBUG_INFO。"""
    try:
        from elftools.elf.elffile import ELFFile
    except ImportError:
        return None, "pyelftools 未安装"
    from collections import defaultdict

    def type_name(die, cu):
        if 'DW_AT_type' not in die.attributes:
            return 'void'
        try:
            t = cu.get_DIE_from_refaddr(die.attributes['DW_AT_type'].value + cu.cu_offset)
        except Exception:
            return '?'
        tag = t.tag
        if tag == 'DW_TAG_base_type':
            n = t.attributes.get('DW_AT_name')
            return n.value.decode() if n else 'base'
        if tag == 'DW_TAG_pointer_type':
            return type_name(t, cu) + '*'
        if tag in ('DW_TAG_const_type', 'DW_TAG_volatile_type', 'DW_TAG_typedef'):
            return type_name(t, cu)
        if tag in ('DW_TAG_structure_type', 'DW_TAG_union_type'):
            n = t.attributes.get('DW_AT_name')
            return ('struct ' + n.value.decode()) if n else 'struct <anon>'
        if tag == 'DW_TAG_enumeration_type':
            return 'enum'
        return tag

    sig2fn = defaultdict(list)
    with open(vmlinux, 'rb') as f:
        elf = ELFFile(f)
        if not elf.has_dwarf_info():
            return None, "映像不含 DWARF（需 CONFIG_DEBUG_INFO=y）"
        dw = elf.get_dwarf_info()
        for cu in dw.iter_CUs():
            for die in cu.iter_DIEs():
                if die.tag != 'DW_TAG_subprogram':
                    continue
                if 'DW_AT_low_pc' not in die.attributes:
                    continue
                name = die.attributes.get('DW_AT_name')
                if not name:
                    continue
                ret = type_name(die, cu)
                args = [type_name(c, cu) for c in die.iter_children()
                        if c.tag == 'DW_TAG_formal_parameter']
                sig2fn['%s (*)(%s)' % (ret, ', '.join(args) or 'void')].append(
                    name.value.decode())
    return sig2fn, None

def main():
    ap = argparse.ArgumentParser(description="CFI 精度基线测量")
    ap.add_argument("vmlinux")
    ap.add_argument("--objdump")
    ap.add_argument("--json")
    a = ap.parse_args()

    arch = detect_arch(a.vmlinux)
    objdump = a.objdump or ("objdump" if arch == "x86-64"
                            else "aarch64-linux-gnu-objdump")
    if not shutil.which(objdump):
        sys.exit(f"cfi_precision: 找不到 {objdump}")

    res = {"arch": arch, "image": a.vmlinux}
    print(f"# CFI 精度基线   映像: {a.vmlinux}   架构: {arch}\n")

    # ---- 1. 落地点标记类（IBT / BTI）----
    if arch == "x86-64":
        total = count_insn(a.vmlinux, objdump, "endbr64")
        seal_bytes = section_size(a.vmlinux, ".ibt_endbr_seal")
        # objtool 把每个待封印位点记为一个 4 字节偏移
        sealed = seal_bytes // 4 if seal_bytes else 0
        remaining = total - sealed
        res["ibt"] = dict(total=total, sealed=sealed, remaining=remaining)
        print("表 2-x　IBT 的等价类规模")
        print(f"  编译器插入的 endbr64 总数        : {total:>8,}")
        print(f"  objtool 封印（.ibt_endbr_seal）  : {sealed:>8,}")
        print(f"  剩余 endbr64 = **IBT 等价类大小**: {remaining:>8,}")
        if not seal_bytes:
            print("  （未找到 .ibt_endbr_seal：内核可能未开 CONFIG_X86_KERNEL_IBT）")
    else:
        total = count_insn(a.vmlinux, objdump, "bti")
        res["bti"] = dict(total=total)
        print("表 2-x　BTI 的等价类规模")
        print(f"  bti 指令总数 = **BTI 等价类大小**: {total:>8,}")
    print()

    # ---- 2. kCFI 等价类 ----
    hist = kcfi_from_symbols(a.vmlinux)
    if hist:
        sizes = sorted(hist.values(), reverse=True)
        res["kcfi"] = dict(source="__kcfi_typeid_", classes=len(sizes),
                           total=sum(sizes), largest=sizes[0],
                           mean=sum(sizes) / len(sizes))
        print("kCFI 等价类（途径 B：__kcfi_typeid_ 符号，编译器实际划分）")
        print(f"  等价类个数      : {len(sizes):,}")
        print(f"  受保护函数总数  : {sum(sizes):,}")
        print(f"  **最大等价类**  : {sizes[0]:,}")
        print(f"  平均等价类      : {sum(sizes)/len(sizes):.2f}")
    else:
        sig2fn, err = kcfi_from_dwarf(a.vmlinux)
        if sig2fn:
            sizes = sorted((len(v) for v in sig2fn.values()), reverse=True)
            res["kcfi"] = dict(source="DWARF", classes=len(sizes),
                               total=sum(sizes), largest=sizes[0],
                               mean=sum(sizes) / len(sizes))
            print("kCFI 等价类（途径 A：DWARF 原型推断；口径偏宽，见文件头说明）")
            print(f"  函数总数        : {sum(sizes):,}")
            print(f"  不同类型签名数  : {len(sizes):,}")
            print(f"  **最大等价类**  : {sizes[0]:,}")
            print(f"  平均等价类      : {sum(sizes)/len(sizes):.2f}")
            print("\n  最大的 5 个等价类:")
            for sig, fns in sorted(sig2fn.items(), key=lambda kv: -len(kv[1]))[:5]:
                print(f"    {len(fns):>6,}  {sig[:90]}")
        else:
            res["kcfi"] = None
            print(f"kCFI 等价类：无法测量 —— {err}")
            print("  需要 CONFIG_CFI_CLANG=y（LLVM=1 构建）或 CONFIG_DEBUG_INFO=y")
    print()

    print("对照本文机制（第 5 章填）：调用点唯一上下文 -> 等价类大小 1")
    if a.json:
        json.dump(res, open(a.json, "w"), ensure_ascii=False, indent=2)
        print(f"\n-> {a.json}")

if __name__ == "__main__":
    sys.exit(main())
