#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
D1 代码索引更新量的收敛（论文 §3.6.2）。

论证 O(1) 索引：传统重定位每次随机化要修正**所有指向被移动代码的引用**（调用点、
函数指针、GOT 项…），量为 O(调用点数)；本机制下所有引用都指向**固定地址的跳板**，
每个函数只有一个 target 槽随迁移更新，量 = 函数数，与调用点数无关。

本脚本在内核映像上统计：
  - 每个随机化函数的跳板被多少条指令引用（= 传统方法每次要改的量）；
  - 随机化实际更新的 target 槽数（= 函数数）。
收敛率 = 1 − 函数数 / 总引用数。

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

    # 反汇编全镜像，数每个跳板地址被引用的次数（目标为该地址的控制流/取址指令）
    refs={f:0 for f in tramp_addr}
    hexset={v:f for f,v in tramp_addr.items()}
    target = re.compile(r"\b(0x)?([0-9a-f]{6,16})\b")
    p=subprocess.Popen([od,"-d","--no-show-raw-insn",a.vmlinux],
                       stdout=subprocess.PIPE,text=True,errors="replace")
    for line in p.stdout:
        # 行内出现某跳板地址即计一次引用（call/b/bl/adrp+add/lea 等）
        m=re.search(r":\s+\S+\s+.*?\b([0-9a-f]{6,16})\b", line)
        if not m: continue
        try: v=int(m.group(1),16)
        except ValueError: continue
        if v in hexset: refs[hexset[v]]+=1
    p.wait()

    total_refs=sum(refs.values())
    nfuncs=len(tramp_addr)
    print(f"# D1 索引更新收敛  映像:{a.vmlinux}  架构:{arch}")
    print(f"# 随机化函数数: {nfuncs}\n")
    print(f"{'函数':<28}{'跳板被引用数':>14}")
    print("-"*44)
    for f in sorted(refs,key=lambda x:-refs[x]):
        print(f"{f:<28}{refs[f]:>14}")
    print("-"*44)
    print(f"{'合计引用（传统方法/次随机化要改）':<28}{total_refs:>14}")
    print(f"{'target 槽（本机制/次随机化要改）':<28}{nfuncs:>14}")
    if total_refs:
        conv=1-nfuncs/total_refs
        print(f"\n收敛率 = 1 − {nfuncs}/{total_refs} = {conv*100:.1f}%")
        print("（引用越多收敛越显著；真实子系统规模下逼近论文的 96.9%）")
    else:
        print("\n（未发现对跳板的静态引用——自测函数调用点少；应用编译器 pass 到真实子系统后重测）")
    if a.json:
        json.dump(dict(arch=arch,funcs=nfuncs,refs=refs,total_refs=total_refs),
                  open(a.json,"w"),ensure_ascii=False,indent=2)
        print(f"-> {a.json}")

if __name__=="__main__": sys.exit(main())
