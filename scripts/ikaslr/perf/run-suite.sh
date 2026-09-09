#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 在被测内核（已启动的 guest 内）跑一档配置的全部基准，结果落 <outdir>。
#   run-suite.sh <config-name> <outdir> [server-ip]
# 网络项需另一台/进程跑 netserver / nginx。
set -eu
CFG="${1:?}"; OUT="${2:?}"; SRV="${3:-}"
D="$OUT/$CFG"; mkdir -p "$D"
HERE="$(cd "$(dirname "$0")" && pwd)"
run(){ echo "== $1"; }

# LMBench 微基准
run "lmbench lat_syscall"
for op in null read write open stat; do
    lat_syscall $op 2>>"$D/lat_syscall.txt" || true
done
run "lmbench lat_ctx"; lat_ctx -s 0 2 8 2>>"$D/lat_ctx.txt" || true
run "lmbench lat_proc"; for k in fork exec shell; do lat_proc $k 2>>"$D/lat_proc.txt"||true; done

# 中断/调度延迟
run "cyclictest"; cyclictest -q -l 200000 -m -p 90 -i 200 -h 400 >"$D/cyclictest.txt" 2>&1 || true

# 计算负载（SPEC 替代）
run "7z bench"; 7z b >"$D/7z.txt" 2>&1 || true
run "openssl speed"; openssl speed -evp aes-256-gcm >"$D/openssl.txt" 2>&1 || true
run "sysbench cpu"; sysbench cpu --threads="$(nproc)" --time=20 run >"$D/sysbench_cpu.txt" 2>&1 || true

# 存储
for j in randread-4k randwrite-4k seqread-1m; do
    run "fio $j"; fio --output-format=json "$HERE/fio/$j.fio" >"$D/fio_$j.json" 2>&1 || true
done

# 网络（需 SRV）
if [ -n "$SRV" ]; then
    run "netperf STREAM"; netperf -H "$SRV" -t TCP_STREAM -l 20 -- -m 1024 >"$D/net_stream.txt" 2>&1 || true
    run "netperf RR"; netperf -H "$SRV" -t TCP_RR -l 20 >"$D/net_rr.txt" 2>&1 || true
fi

# 随机化统计快照
[ -r /proc/ikaslr/stats ] && cat /proc/ikaslr/stats >"$D/ikaslr_stats.txt" || true
echo "done: $D"
