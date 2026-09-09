#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""汇总各档结果为对比表（相对 Base 的开销%）。
用法: 06-analyze.py [results/<arch>]   # 缺省自动挑 results/<当前arch>
     目录内含 base/ R/ RD/ RDP 子目录（由 05-run-suite.sh 生成）。
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
    if len(sys.argv) >= 2:
        root = sys.argv[1]
    else:
        import platform
        a = {"x86_64":"x86_64","aarch64":"arm64"}.get(platform.machine(), platform.machine())
        root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results", a)
    if not os.path.isdir(root):
        sys.exit(f"no results dir: {root}")
    print(f"results: {root}\n")
    cfgs = [c for c in ("base","R","RD","RDP") if os.path.isdir(f"{root}/{c}")]
    metrics = {
        "fio_randread_iops": lambda c: fio_iops(f"{root}/{c}/fio_randread-4k.json"),
        "fio_randwrite_iops": lambda c: fio_iops(f"{root}/{c}/fio_randwrite-4k.json"),
        "cyclictest_max_us": lambda c: num(f"{root}/{c}/cyclictest.txt", r"Max:\s*(\d+)"),
        "netperf_stream_mbps": lambda c: num(f"{root}/{c}/net_stream.txt", r"\n\s*\d+\s+\d+\s+\d+\s+[\d.]+\s+([\d.]+)"),
        "unixbench_syscall": lambda c: num(f"{root}/{c}/unixbench.txt", r"System Call Overhead\s+[\d.]+\s+([\d.]+)"),
        "unixbench_pipe": lambda c: num(f"{root}/{c}/unixbench.txt", r"Pipe Throughput\s+[\d.]+\s+([\d.]+)"),
        "lat_syscall_null_us": lambda c: num(f"{root}/{c}/lmbench.txt", r"Simple syscall:\s*([\d.]+)"),
        "lat_syscall_read_us": lambda c: num(f"{root}/{c}/lmbench.txt", r"Simple read:\s*([\d.]+)"),
        "sysbench_cpu_eps":    lambda c: num(f"{root}/{c}/sysbench_cpu.txt", r"events per second:\s*([\d.]+)"),
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
            ov = (worst-base)/base*100 if not any(k in name for k in ("iops","mbps","eps")) \
                 else (base-worst)/base*100
            row += f"{ov:>15.1f}%"
        print(row)
    print("\n注：每指标应取 ≥5 次中位数；此脚本汇总单次，多次时先各自取中位再喂入。")

if __name__ == "__main__":
    sys.exit(main())
