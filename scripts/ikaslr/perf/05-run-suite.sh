#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 步骤 5：在**已启动进某档内核**后，跑该档的全部基准（建议 root，用于绑频/绑核）。
#   sudo ./05-run-suite.sh                 # 自动识别当前档（按 uname -r）
#   sudo ./05-run-suite.sh RD              # 手动指定档名
#   SERVER_IP=10.0.0.2 sudo ./05-run-suite.sh   # 带网络项
# 结果落 $RESULTS_DIR/$ARCH/<variant>/，analyze 时读这里。
set -uo pipefail
. "$(dirname "$0")/env.sh"

# ---- 识别当前档 ----
CFG="${1:-}"
[ -n "$CFG" ] || CFG="$(current_variant)"
echo "== 当前档: $CFG   ($(uname -r))"

D="$RESULTS_DIR/$ARCH/$CFG"; mkdir -p "$D"
UBDIR="$BENCH_DIR/byte-unixbench/UnixBench"
LMBIN="$(ls -d "$BENCH_DIR"/lmbench/bin/* 2>/dev/null | head -1)"
[ -n "$LMBIN" ] && export PATH="$LMBIN:$PATH"

# ---- 环境快照（复现/降噪核对）----
{
    echo "date=$(date -Is)"; echo "uname=$(uname -a)"
    echo "cmdline=$(cat /proc/cmdline)"; echo "nproc=$(nproc)"
    echo "governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
    grep -H . /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null
} > "$D/_env.txt"
[ -r /proc/ikaslr/stats ] && cat /proc/ikaslr/stats > "$D/_ikaslr_stats_before.txt"

# ---- 合理性自检：非 base 档必须真的有函数被随机化 ----
# 若为 0，多半是内核编译时没开 CRR_TRAMPOLINE=y / 没给 IKASLR_FUNCS，
# 这时四档等价，测出来的"开销"没有意义。
if [ "$CFG" != base ]; then
    NF=$(awk '/^functions/{print $2}' /proc/ikaslr/stats 2>/dev/null || echo 0)
    echo "== 随机化函数数: ${NF:-0}"
    if [ "${NF:-0}" -le 3 ]; then
        cat <<'W'
!! 随机化函数数 <= 3：只有内建示例函数，没有真实内核函数被随机化。
   本档数据不可用于性能结论。请用 CRR_TRAMPOLINE=y + IKASLR_FUNCS 重编内核
   （见 02-build-kernels.sh）。
W
    fi
    echo "randomized_functions=${NF:-0}" >> "$D/_env.txt"
fi

# ---- 降噪：绑频、关 turbo（各平台旋钮不同，存在才写）----
if command -v cpupower >/dev/null; then cpupower frequency-set -g performance >/dev/null 2>&1 || true; fi
write_if() { [ -w "$1" ] && printf '%s' "$2" > "$1" 2>/dev/null || true; }
write_if /sys/devices/system/cpu/intel_pstate/no_turbo 1   # Intel pstate: 1=关 turbo
write_if /sys/devices/system/cpu/cpufreq/boost 0           # AMD / acpi-cpufreq: 0=关 boost

# ---- +R/+RD/+RDP：后台定频触发随机化 ----
TRIG_PID=""
if variant_needs_trigger "$CFG" && [ -w /proc/ikaslr/trigger ]; then
    echo "== 定频触发随机化：每 ${TRIGGER_MS}ms"
    "$PERF_DIR/trigger-loop.sh" "$TRIGGER_MS" >"$D/_trigger.log" 2>&1 &
    TRIG_PID=$!
fi
cleanup(){ [ -n "$TRIG_PID" ] && kill "$TRIG_PID" 2>/dev/null || true; }
trap cleanup EXIT

run(){ echo "  -- $*"; }

# ===================== 基准项 =====================
# LMBench 微基准（§6.5.3）
if command -v lat_syscall >/dev/null; then
    run lmbench; : > "$D/lmbench.txt"
    for op in null read write open stat; do lat_syscall $op 2>>"$D/lmbench.txt"; done
    lat_ctx -s 0 2 8 2>>"$D/lmbench.txt" || true
    for k in fork exec shell; do lat_proc $k 2>>"$D/lmbench.txt" || true; done
    lat_pagefault "$D/_env.txt" 2>>"$D/lmbench.txt" || true
    bw_mem 256m rd 2>>"$D/lmbench.txt" || true
else echo "  (lmbench 未装，跳过)"; fi

# UnixBench（§6.5.3）—— syscall/pipe/进程创建等最贴内核路径
if [ -x "$UBDIR/Run" ]; then
    run UnixBench
    ( cd "$UBDIR" && ./Run -c 1 syscall pipe context1 spawn execl shell1 ) >"$D/unixbench.txt" 2>&1 || true
fi

# cyclictest 中断/调度延迟（§6.5.3）
command -v cyclictest >/dev/null && { run cyclictest;
    cyclictest -q -l 200000 -m -p 90 -i 200 -h 400 >"$D/cyclictest.txt" 2>&1 || true; }

# 计算负载（SPEC 替代；§6.5.4）
SEVENZ="$(command -v 7z || command -v 7za || true)"   # openEuler p7zip 提供 7za
[ -n "$SEVENZ" ]               && { run 7z;       "$SEVENZ" b     >"$D/7z.txt" 2>&1 || true; }
command -v openssl >/dev/null  && { run openssl;  openssl speed -evp aes-256-gcm >"$D/openssl.txt" 2>&1 || true; }
command -v sysbench >/dev/null && { run sysbench; sysbench cpu --threads="$(nproc)" --time=20 run >"$D/sysbench_cpu.txt" 2>&1 || true; }
# SPEC CPU2006（若已装并 source shrc，取消注释）：
# run SPEC; ( cd "$SPEC" && source shrc && runspec --config=ikaslr.cfg --size=ref --iterations=3 int fp ) >"$D/spec.txt" 2>&1 || true

# 存储 fio（§6.6.3）
if command -v fio >/dev/null; then
    for j in randread-4k randwrite-4k seqread-1m randrw-qd; do
        [ -r "$PERF_DIR/fio/$j.fio" ] || continue
        run "fio $j"; fio --output-format=json "$PERF_DIR/fio/$j.fio" >"$D/fio_$j.json" 2>&1 || true
    done
fi

# 网络（需对端 netserver / nginx）
if [ -n "$SERVER_IP" ] && command -v netperf >/dev/null; then
    run "netperf $SERVER_IP"
    netperf -H "$SERVER_IP" -t TCP_STREAM -l 20 -- -m 1024 >"$D/net_stream.txt" 2>&1 || true
    netperf -H "$SERVER_IP" -t TCP_RR -l 20 >"$D/net_rr.txt" 2>&1 || true
    command -v wrk >/dev/null && wrk -t8 -c256 -d20s "http://$SERVER_IP/" >"$D/wrk.txt" 2>&1 || true
fi

[ -r /proc/ikaslr/stats ] && cat /proc/ikaslr/stats > "$D/_ikaslr_stats_after.txt"
echo "== 完成： $D"
echo "   全部档跑完后： ./06-analyze.py"
