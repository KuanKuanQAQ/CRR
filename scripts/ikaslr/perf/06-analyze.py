#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""汇总四档基准结果（E3 / E6 / E7），并把端到端结果接回 E3-A 的开销模型。

输入：05-run-suite.sh 产出的 results/<arch>/<档>/<负载>/rep<N>.txt
用法：./06-analyze.py [results/<arch>]   # 省略则用 env.sh 里的默认路径

产出三张表：
  一、逐项对比    行=指标，列=四档，值=中位数 [下四分位, 上四分位] 与相对 Base 的 %
  二、跨区域调用频度 G   逐个负载（需要 Rg 档，见 env.sh）
  三、模型核对    E3-A 的 `总开销 ≈ G × 常驻税` 对每个负载的预测 vs 实测

**不做任何外推、不猜缺失值**：解析不出来的项如实标"—"，而不是填 0。
"""
import json
import os
import re
import statistics as st
import sys
from pathlib import Path

# E3-A 实测的常驻税（生产配置 STATS=n），单位秒。见 实现/实验/E3-A。
TAX_IN, TAX_OUT = 13.02e-9, 5.00e-9

LOWER_IS_BETTER = True   # 多数指标是延迟；吞吐类在 METRICS 里单独标


def q(xs):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    if len(xs) < 4:
        return (st.median(xs), xs[0], xs[-1])
    qq = st.quantiles(xs, n=4, method="inclusive")
    return (st.median(xs), qq[0], qq[2])


# ---------------------------------------------------------------- 解析器
# 每个解析器返回 {指标名: 数值}；解析不出来返回 {}。

def p_lat_syscall(t):
    out = {}
    for m in re.finditer(r"^Simple (\w+):\s+([\d.]+) microseconds", t, re.M):
        out[f"lat_syscall {m.group(1)} (µs)"] = float(m.group(2))
    return out


def p_lat_ctx(t):
    out = {}
    for m in re.finditer(r"^(\d+)\s+([\d.]+)$", t, re.M):
        out[f"lat_ctx {m.group(1)}p (µs)"] = float(m.group(2))
    return out


def p_lat_proc(t):
    out = {}
    for m in re.finditer(r"^Process ([\w+]+):\s+([\d.]+) microseconds", t, re.M):
        out[f"lat_proc {m.group(1)} (µs)"] = float(m.group(2))
    return out


def p_lat_pagefault(t):
    m = re.search(r"Pagefaults on .*?:\s+([\d.]+)", t)
    return {"lat_pagefault (µs)": float(m.group(1))} if m else {}


def p_bw_mem(t):
    out = {}
    lines = [l.split() for l in t.splitlines() if re.match(r"^[\d.]+\s+[\d.]+$", l)]
    for i, kind in zip(range(len(lines)), ("rd", "wr")):
        out[f"bw_mem {kind} (MB/s)↑"] = float(lines[i][1])
    return out


def p_unixbench(t):
    out = {}
    for m in re.finditer(r"^\s*(.+?)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*$", t, re.M):
        name = m.group(1).strip()
        if len(name) > 4 and not name[0].isdigit():
            out[f"unixbench {name} (index)↑"] = float(m.group(4))
    return out


def p_cyclictest(t):
    """从 -h 直方图算分位；-q 模式下还有 Min/Avg/Max 行。"""
    out = {}
    m = re.search(r"# Max Latencies:\s*([\d\s]+)", t)
    if m:
        out["cyclictest max (µs)"] = max(int(x) for x in m.group(1).split())
    hist = {}
    for line in t.splitlines():
        mm = re.match(r"^(\d{5})\s+(.+)$", line)
        if mm:
            hist[int(mm.group(1))] = sum(int(x) for x in mm.group(2).split())
    if hist:
        total = sum(hist.values())
        acc = 0
        for us in sorted(hist):
            acc += hist[us]
            for p in (99.0, 99.9):
                k = f"cyclictest p{p} (µs)"
                if k not in out and acc >= total * p / 100:
                    out[k] = us
    return out


def p_7z(t):
    m = re.search(r"^Tot:\s+\d+\s+\d+\s+(\d+)", t, re.M)
    return {"7z total (MIPS)↑": float(m.group(1))} if m else {}


def p_openssl(t):
    """openssl 3.x 的行是 `AES-256-GCM  670753.22k  ... 4859587.24k`（大写、多列）。"""
    m = re.search(r"^AES-256-GCM\s+(.*)$", t, re.M | re.I)
    if not m:
        return {}
    vals = re.findall(r"([\d.]+)k", m.group(1))
    if not vals:
        return {}
    out = {"openssl aes-256-gcm 16KB块 (k/s)↑": float(vals[-1])}
    if len(vals) > 1:
        out["openssl aes-256-gcm 16B块 (k/s)↑"] = float(vals[0])
    return out


def p_sysbench(t):
    m = re.search(r"events per second:\s+([\d.]+)", t)
    return {"sysbench cpu (ev/s)↑": float(m.group(1))} if m else {}


def p_fio(t):
    try:
        j = json.loads(t[t.index("{"):])
    except Exception:
        return {}
    out = {}
    for job in j.get("jobs", []):
        nm = job.get("jobname", "job")
        for rw in ("read", "write"):
            d = job.get(rw, {})
            if d.get("iops", 0):
                out[f"fio {nm} {rw} IOPS↑"] = d["iops"]
                out[f"fio {nm} {rw} p99 (µs)"] = \
                    d.get("clat_ns", {}).get("percentile", {}).get("99.000000", 0) / 1000
    return out


def p_netperf(t):
    nums = re.findall(r"^\s*\d+\s+\d+\s+\d+\s+[\d.]+\s+([\d.]+)\s*$", t, re.M)
    if nums:
        return {"netperf 吞吐/事务率↑": float(nums[-1])}
    return {}


def p_wrk(t):
    m = re.search(r"Requests/sec:\s+([\d.]+)", t)
    return {"wrk (req/s)↑": float(m.group(1))} if m else {}


PARSERS = {
    "lat_syscall": p_lat_syscall, "lat_ctx": p_lat_ctx, "lat_proc": p_lat_proc,
    "lat_pagefault": p_lat_pagefault, "bw_mem": p_bw_mem,
    "unixbench": p_unixbench, "cyclictest": p_cyclictest,
    "7z": p_7z, "openssl": p_openssl, "sysbench": p_sysbench,
    "wrk": p_wrk,
}


def parse_dir(d: Path):
    """返回 {指标: [每次重复的值]}。"""
    name = d.name
    fn = PARSERS.get(name)
    if fn is None:
        if name.startswith("fio_"):
            fn = p_fio
        elif name.startswith("netperf"):
            fn = p_netperf
        else:
            return {}
    acc = {}
    for f in sorted(d.glob("rep*.txt")):
        for k, v in fn(f.read_text(errors="replace")).items():
            acc.setdefault(k, []).append(v)
    return acc


def read_g(d: Path):
    """返回 (每秒 fixed_in, 每秒 fixed_out, 秒数)，取各次重复的中位。"""
    f = d / "g.csv"
    if not f.exists():
        return None
    ins, outs, secs = [], [], []
    for line in f.read_text().splitlines()[1:]:
        p = line.split(",")
        if len(p) == 4 and float(p[1]) > 0:
            secs.append(float(p[1]))
            ins.append(int(p[2]) / float(p[1]))
            outs.append(int(p[3]) / float(p[1]))
    if not ins:
        return None
    return (st.median(ins), st.median(outs), st.median(secs))


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    if root is None:
        here = Path(__file__).resolve().parent
        arch = os.uname().machine.replace("aarch64", "arm64").replace("amd64", "x86_64")
        root = here / "results" / arch
    if not root.is_dir():
        sys.exit(f"找不到结果目录 {root}（先跑 05-run-suite.sh）")

    tiers = [p.name for p in sorted(root.iterdir()) if p.is_dir()]
    if "base" not in tiers:
        print(f"!! 缺 base 档，无法给出相对开销。现有：{tiers}\n")
    order = [t for t in ("base", "R", "RD", "RDP", "Rg") if t in tiers] + \
            [t for t in tiers if t not in ("base", "R", "RD", "RDP", "Rg")]

    data = {}       # {档: {指标: [值...]}}
    gdata = {}      # {档: {负载: (g_in, g_out, secs)}}
    for t in order:
        data[t] = {}
        gdata[t] = {}
        for d in sorted((root / t).iterdir()):
            if not d.is_dir():
                continue
            for k, v in parse_dir(d).items():
                data[t][k] = v
            g = read_g(d)
            if g:
                gdata[t][d.name] = g

    # ---------------- 一、逐项对比 ----------------
    print("=" * 100)
    print("表一　四档逐项对比　　值 = 中位数 [下四分位, 上四分位]；括号内为相对 Base 的变化")
    print("        指标名带 ↑ 表示越大越好（吞吐），其余是延迟，越小越好")
    print("=" * 100)
    metrics = sorted({k for t in order for k in data[t]})
    if not metrics:
        print("  （没有解析出任何指标——检查 05 是否真的跑了，或基准是否装齐）")
    w = 34
    hdr = f"{'指标':<{w}}"
    for t in order:
        hdr += f"{t:>22}"
    print(hdr)
    print("-" * 100)
    for m in metrics:
        up = m.endswith("↑")
        row = f"{m:<{w}}"
        b = q(data.get("base", {}).get(m, []))
        for t in order:
            s = q(data[t].get(m, []))
            if s is None:
                row += f"{'—':>22}"
                continue
            cell = f"{s[0]:.4g}"
            if b and t != "base" and b[0]:
                d = (s[0] / b[0] - 1) * 100
                if up:
                    d = -d          # 吞吐下降也记成正的"开销"
                cell += f" ({d:+.1f}%)"
            row += f"{cell:>22}"
        print(row)

    # ---------------- 二、跨区域调用频度 G ----------------
    gt = next((t for t in order if gdata.get(t)), None)
    print()
    print("=" * 100)
    print("表二　跨区域调用频度 G（取自开着 STATS 的档；没有则本表为空）")
    print("=" * 100)
    if not gt:
        print("  （没有任何档带 g.csv。G 需要 CONFIG_IKASLR_STATS=y —— 见 env.sh 的 Rg 档说明）")
    else:
        print(f"  数据来自档 {gt}")
        print(f"  {'负载':<28}{'fixed_in/秒':>16}{'fixed_out/秒':>16}{'合计/秒':>16}")
        for wl, (gi, go, _s) in sorted(gdata[gt].items()):
            print(f"  {wl:<28}{gi:>16,.0f}{go:>16,.0f}{gi+go:>16,.0f}")

    # ---------------- 三、模型核对 ----------------
    print()
    print("=" * 100)
    print("表三　E3-A 开销模型核对：预测的常驻税 CPU 占用 vs 实测端到端开销")
    print("=" * 100)
    if not gt or "base" not in data:
        print("  （需要同时有 base 档与带 G 的档才能核对）")
    else:
        print(f"  常驻税取自 实现/实验/E3-A：fixed_in {TAX_IN*1e9:.2f} ns、"
              f"fixed_out {TAX_OUT*1e9:.2f} ns（生产配置 STATS=n）")
        print(f"  {'负载':<28}{'模型预测占用':>16}{'实测开销':>16}  说明")
        for wl, (gi, go, _s) in sorted(gdata[gt].items()):
            pred = gi * TAX_IN + go * TAX_OUT      # 每秒消耗的 CPU 秒数
            # 找这个负载在 R 档的实测开销：取该负载解析出的第一个指标
            # 指标名的第一个词即负载名（各解析器都按这个约定造名字）；
            # 取该负载下第一个能同时在 base 与 R 找到的指标。
            meas = None
            for m in metrics:
                first = m.split()[0]
                if not (wl == first or wl.startswith(first) or first.startswith(wl)):
                    continue
                b = q(data["base"].get(m, []))
                r = q(data.get("R", {}).get(m, []))
                if b and r and b[0]:
                    d = (r[0] / b[0] - 1)
                    if m.endswith("↑"):
                        d = -d
                    meas = d
                    break
            ms = f"{meas*100:+.1f}%" if meas is not None else "—"
            note = "" if meas is not None else "（没找到对应指标，需手工对齐）"
            print(f"  {wl:<28}{pred*100:>15.1f}%{ms:>16}  {note}")
        print()
        print("  口径：模型预测的是「常驻税占用的 CPU 比例」；实测是「相对 Base 的变慢幅度」。")
        print("  两者在 CPU 饱和的负载上同口径，在 I/O 受限的负载上不同口径——后者的")
        print("  实测开销会明显小于预测，因为大量时间花在等 I/O 而不是执行内核代码。")
        print("  差额交给 E12 归因（09-attribution.sh）。")

    print()
    print(f"原始数据: {root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
