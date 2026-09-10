#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""汇总 E3-A（跳板机制的完整开销剖面）的采集结果。

对应 Documentation/crr/设计文档/草稿/实验总清单.md §第1章 E3-A。
输入是 11-e3a-profile.sh 产出目录里的 *.e3a 文件（init_e3a.c 的机器可读行）。

**只做汇总与分位数，不做任何外推**。开销模型的代入与结论写在实验文档里，
因为那里才有条件写清每个数字的口径。

用法: e3a_collect.py <结果目录>
"""
import glob
import os
import re
import statistics as st
import sys


def quart(xs):
    """返回 (中位数, 下四分位, 上四分位)。样本太少时退化为 min/max。"""
    xs = sorted(xs)
    if not xs:
        return (None, None, None)
    if len(xs) < 4:
        return (st.median(xs), xs[0], xs[-1])
    q = st.quantiles(xs, n=4, method="inclusive")
    return (st.median(xs), q[0], q[2])


def fmt_ps(v):
    """皮秒 -> 纳秒，保留两位。"""
    return "—" if v is None else f"{v / 1000:.2f}"


def parse(path):
    d = {"bench": {}, "rounds": [], "g": {}, "cfg": {}}
    for ln in open(path):
        ln = ln.rstrip("\n")
        if ln.startswith("E3A-CFG "):
            d["cfg"] = dict(re.findall(r"(\w+)=(-?\d+)", ln))
        elif ln.startswith("E3A-BENCH "):
            m = re.match(r"E3A-BENCH rep=(\d+) (\S+)\s+(-?\d+)", ln)
            if m:
                d["bench"].setdefault(m.group(2), []).append(int(m.group(3)))
        elif ln.startswith("E3A-ROUND "):
            r = dict(re.findall(r"(\w+)=(-?\d+)", ln))
            d["rounds"].append({k: int(v) for k, v in r.items()})
        elif ln.startswith("E3A-G "):
            tag = ln.split()[1]
            r = dict(re.findall(r"(\w+)=([-\d.]+)", ln))
            d["g"][tag] = r
    return d


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    files = sorted(glob.glob(os.path.join(root, "*.e3a")))
    if not files:
        sys.exit(f"{root} 里没有 *.e3a")
    cases = {os.path.basename(f)[:-4]: parse(f) for f in files}

    # ---------- 一、常驻税 A–E ----------
    print("=" * 78)
    print("一、常驻税（每次跨区域调用都付）　单位 ns/次，中位数 [下四分位, 上四分位]")
    print("=" * 78)
    KEYS = [
        ("A_direct_ps", "A 直接调用等价函数（基线）"),
        ("B_indirect_ps", "B 经 target 槽的间接转移"),
        ("C_fixed_in_ps", "C 完整 fixed_in（计数+间接转移）"),
        ("D_whitelist_hit_ps", "D 白名单查询（命中）"),
        ("D_whitelist_miss_ps", "D' 白名单查询（未命中）"),
        ("E_fixed_out_ps", "E 完整 fixed_out"),
    ]
    DERIVED = [
        ("indirect_transfer_ps", "  间接转移净开销 = B−A"),
        ("enter_leave_count_ps", "  进出计数净开销 = C−B"),
        ("fixed_in_total_ps", "  fixed_in 合计 = C−A"),
        ("fixed_out_total_ps", "  fixed_out 合计 = E−B"),
        ("out_count_only_ps", "  其中计数部分 = (E−B)−D"),
    ]
    names = [c for c in cases if cases[c]["bench"]]
    if names:
        w = max(len(n) for n in names)
        print(f"{'项':<40}" + "".join(f"{n:>{max(w,18)+2}}" for n in names))
        for key, label in KEYS + [("", "")] + DERIVED:
            if not key:
                print("-" * 78)
                continue
            row = f"{label:<40}"
            for n in names:
                xs = cases[n]["bench"].get(key, [])
                m, lo, hi = quart(xs)
                row += f"{fmt_ps(m) + ' [' + fmt_ps(lo) + ',' + fmt_ps(hi) + ']':>{max(w,18)+2}}"
            print(row)

    # ---------- 二、随机化代价 H–M ----------
    print()
    print("=" * 78)
    print("二、随机化代价（每次随机化付一次）　单位 µs，中位数 [下四分位, 上四分位]")
    print("=" * 78)
    print(f"{'范围':<14}{'函数数':>7}{'成功/触发':>11}"
          f"{'H 等待':>18}{'I 更新':>18}{'J 改映射':>18}{'K 合计':>18}{'L 准备':>18}")
    scope_rows = []
    for n in sorted(cases, key=lambda x: int(cases[x]["cfg"].get("functions", 0))):
        d = cases[n]
        done = [r for r in d["rounds"] if r.get("done")]
        if not done:
            continue
        nf = int(d["cfg"].get("functions", 0))
        cols = []
        for k in ("wait_ns", "update_ns", "remap_ns", "cp_ns", "prep_ns"):
            m, lo, hi = quart([r[k] for r in done if k in r])
            cols.append((m, lo, hi))
        scope_rows.append((n, nf, cols[1][0]))
        ratio = f"{len(done)}/{len(d['rounds'])}"
        s = f"{n:<14}{nf:>7}{ratio:>11}"
        for m, lo, hi in cols:
            s += f"{f'{m/1000:.2f} [{lo/1000:.2f},{hi/1000:.2f}]':>18}" if m is not None else f"{'—':>18}"
        print(s)

    # I 项的正比性：更新耗时 / 函数数 应大致为常数
    scope_rows = [r for r in scope_rows if r[1] and r[2]]
    if len(scope_rows) >= 2:
        print()
        print("  I 项的正比性检验（更新耗时 ÷ 函数数，应大致为常数）：")
        for n, nf, upd in scope_rows:
            print(f"    {n:<14} {nf:>5} 个函数  {upd/1000:>8.2f} µs  "
                  f"→ {upd/nf:>7.1f} ns/函数")

    # ---------- 三、跨区域调用频度 G ----------
    print()
    print("=" * 78)
    print("三、跨区域调用频度 G（次/秒）")
    print("=" * 78)
    print(f"{'档':<14}{'场景':<8}{'fixed_in 进入':>16}{'fixed_out 离开':>16}{'合计':>14}")
    for n in sorted(cases):
        for tag in ("idle", "load"):
            g = cases[n]["g"].get(tag)
            if not g:
                continue
            secs = float(g.get("elapsed_s", 0)) or 1
            ein, eout = int(g.get("d_enters", 0)), int(g.get("d_outs", 0))
            print(f"{n:<14}{tag:<8}{ein/secs:>16,.0f}{eout/secs:>16,.0f}"
                  f"{(ein+eout)/secs:>14,.0f}")
    print()
    print("注：idle 一行是采集程序自身的底噪（它读 /proc 也要跨区域），")
    print("    真实负载下的 G 应取 load 减去 idle。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
