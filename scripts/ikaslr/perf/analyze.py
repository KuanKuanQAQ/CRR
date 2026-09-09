#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""汇总 run-suite.sh 各档结果为对比表（相对 Base 的开销%）。
用法: analyze.py <results-dir>   # 内含 base/ R/ RD/ RDP 子目录
"""
import json, os, re, sys, glob

def fio_iops(path):
    try:
        d = json.load(open(path))
        j = d["jobs"][0]
        return j["read"]["iops"] + j["write"]["iops"]
    except Exception:
        return None

def num(path, pat):
    try:
        m = re.search(pat, open(path, errors="replace").read())
        return float(m.group(1)) if m else None
    except OSError:
        return None

def main():
    if len(sys.argv) < 2:
        sys.exit("usage: analyze.py <results-dir>")
    root = sys.argv[1]
    cfgs = [c for c in ("base","R","RD","RDP") if os.path.isdir(f"{root}/{c}")]
    metrics = {
        "fio_randread_iops": lambda c: fio_iops(f"{root}/{c}/fio_randread-4k.json"),
        "fio_randwrite_iops": lambda c: fio_iops(f"{root}/{c}/fio_randwrite-4k.json"),
        "cyclictest_max_us": lambda c: num(f"{root}/{c}/cyclictest.txt", r"Max:\s*(\d+)"),
        "netperf_stream_mbps": lambda c: num(f"{root}/{c}/net_stream.txt", r"\n\s*\d+\s+\d+\s+\d+\s+[\d.]+\s+([\d.]+)"),
        "unixbench_syscall": lambda c: num(f"{root}/{c}/unixbench.txt", r"System Call Overhead\s+[\d.]+\s+([\d.]+)"),
        "unixbench_pipe": lambda c: num(f"{root}/{c}/unixbench.txt", r"Pipe Throughput\s+[\d.]+\s+([\d.]+)"),
    }
    print(f"{'metric':<26}" + "".join(f"{c:>12}" for c in cfgs) + f"{'ovhd% vs base':>16}")
    print("-"*(26+12*len(cfgs)+16))
    for name, fn in metrics.items():
        vals = {c: fn(c) for c in cfgs}
        base = vals.get("base")
        row = f"{name:<26}"
        for c in cfgs:
            v = vals[c]
            row += f"{(f'{v:.1f}' if v is not None else '-'):>12}"
        if base and vals.get(cfgs[-1]) is not None:
            worst = vals[cfgs[-1]]
            # for latency higher=worse; for throughput lower=worse
            ov = (worst-base)/base*100 if "iops" not in name and "mbps" not in name \
                 else (base-worst)/base*100
            row += f"{ov:>15.1f}%"
        print(row)
    print("\n注：每指标应取 ≥5 次中位数；此脚本汇总单次，多次时先各自取中位再喂入。")

if __name__ == "__main__":
    sys.exit(main())
