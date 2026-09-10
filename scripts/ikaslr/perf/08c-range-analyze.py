#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""汇总 E10 范围敏感性（+ E5-B 的降幅曲线）。

输入：08b-range-collect.sh 产出的 results/<arch>/range-sweep/*.{env,rounds.csv,g}.txt
用法：./08c-range-analyze.py [results/<arch>/range-sweep]

清单 §6.7.2 要的曲线：随机化区域函数数 → 单次随机化耗时 / 跨区域调用占比 /
PA 验证降幅 / 内存开销。本脚本给前三项与内存；PA 降幅需要 ARM（第 5 章）。
"""
import csv, os, re, statistics as st, sys
from pathlib import Path


def med(xs):
    xs = [x for x in xs if x is not None]
    return st.median(xs) if xs else None


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    if root is None:
        arch = os.uname().machine.replace("aarch64", "arm64").replace("amd64", "x86_64")
        root = Path(__file__).resolve().parent / "results" / arch / "range-sweep"
    if not root.is_dir():
        sys.exit(f"找不到 {root}（先跑 08b-range-collect.sh）")

    rows = []
    for env in sorted(root.glob("*.env.txt")):
        tag = env.name[:-8]
        d = dict(l.split("=", 1) for l in env.read_text().splitlines() if "=" in l)
        pool = re.search(r"(\d+) variants x (\d+) B", env.read_text())
        r = {"tag": tag,
             "n": int(d.get("functions", 0)),
             "rand": int(d.get("rand_region_bytes", 0)),
             "tramp": int(d.get("tramp_region_bytes", 0)),
             "wl": int(d.get("whitelist_entries", 0)),
             "pool": (int(pool.group(1)) * int(pool.group(2))) if pool else None}
        rc = root / f"{tag}.rounds.csv"
        if rc.exists():
            done = [x for x in csv.DictReader(open(rc)) if x["done"] == "1"]
            r["nrounds"] = len(done)
            for k, col in (("cp", "cp_ns"), ("upd", "update_ns"),
                           ("wait", "wait_ns"), ("prep", "prep_ns")):
                r[k] = med([int(x[col]) for x in done if x[col].isdigit()])
        g = root / f"{tag}.g.txt"
        if g.exists():
            m = dict(re.findall(r"(\w+)=([\d.]+)", g.read_text()))
            r["g_in"] = float(m.get("g_in", 0))
            r["g_out"] = float(m.get("g_out", 0))
        rows.append(r)
    rows.sort(key=lambda r: r["n"])
    if not rows:
        sys.exit("没有采到任何规模的数据")

    print("=" * 104)
    print("E10　随机化范围敏感性（+ E5-B 的跨区域调用占比）")
    print("=" * 104)
    print(f"{'规模':<18}{'函数数':>7}{'区域(KB)':>10}{'白名单':>8}"
          f"{'停顿窗口(µs)':>14}{'更新(µs)':>11}{'准备(µs)':>11}"
          f"{'常驻内存(KB)':>13}{'G 合计(次/秒)':>15}")
    print("-" * 104)
    for r in rows:
        def f(k, div=1000.0, w=11, fmt=".2f"):
            v = r.get(k)
            return format(v / div, fmt).rjust(w) if v else "—".rjust(w)
        mem = (r["pool"] * 2 / 1024) if r.get("pool") else None   # 变体池 + 陷阱影像
        g = (r.get("g_in", 0) + r.get("g_out", 0)) or None
        print(f"{r['tag']:<18}{r['n']:>7}{r['rand']/1024:>10.1f}{r['wl']:>8}"
              f"{f('cp', 1000.0, 14)}{f('upd')}{f('prep')}"
              f"{(format(mem, '.0f').rjust(13) if mem else '—'.rjust(13))}"
              f"{(format(g, ',.0f').rjust(15) if g else '—'.rjust(15))}")
    print()
    print("  读法：")
    print("   · 停顿窗口应与函数数**近似成正比**（每函数一个 target 槽，E3-1 的判据）；")
    print("   · 更新耗时是**缓存受限**的，函数一多每函数成本会上升，见 E3-A §1(二)；")
    print("   · 常驻内存 = 4 份变体 + 4 份陷阱影像，随区域线性增长；")
    print("   · G **随范围饱和而非线性增长**——再多函数只是把同一条系统调用路径切得更碎，")
    print("     而路径长度是固定的（E3-A §1(三) 实测 21→485 个函数，G 只从 44M 涨到 59M）。")
    print("     正文必须写清这一点，否则读者会线性外推。")
    print()
    print("  PA 验证降幅（E5-B）= 进入随机化区域的间接调用占全部间接调用的比例，")
    print("  需要 ARM + 第 5 章的 PA 插桩；本机 x86 只能给出上表的跨区域调用频度。")
    print(f"\n原始数据: {root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
