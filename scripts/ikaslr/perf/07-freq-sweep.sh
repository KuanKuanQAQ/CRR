#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 步骤 E9：随机化频率敏感性（§6.7.1 / 图 6-3，全文最关键图之一）。
# 在**已启动进某个 +R/+RD 档**后跑：固定一个负载，人为扫触发频率，测开销曲线。
#   sudo ./07-freq-sweep.sh
#   FREQS="0 1 5 20 100 500 1000" sudo ./07-freq-sweep.sh   # 自定义频率(Hz)
# 频率 0 = 不触发(基线)。负载默认 fio randread-4k，可用 WORKLOAD 覆盖。
set -uo pipefail
. "$(dirname "$0")/env.sh"

[ -w /proc/ikaslr/trigger ] || { echo "!! 当前内核无 /proc/ikaslr/trigger，需先启动进 +R/+RD 档"; exit 1; }

REL="$(uname -r)"; CFG="${REL##*-ikaslr-}"
FREQS="${FREQS:-0 1 5 20 50 100 200 500 1000}"     # Hz；0=基线
D="$RESULTS_DIR/$ARCH/freq-sweep/$CFG"; mkdir -p "$D"
echo "== 频率扫描 @ $CFG ($REL)   频率(Hz): $FREQS"

# 固定负载：默认 fio randread-4k 取 IOPS；WORKLOAD 可整体替换（需回显一个数值指标）
run_workload() {   # 打印本次负载的指标数值（越大越好=吞吐；据负载而定）
    if [ -n "${WORKLOAD:-}" ]; then eval "$WORKLOAD"; return; fi
    if command -v fio >/dev/null && [ -r "$PERF_DIR/fio/randread-4k.fio" ]; then
        fio --output-format=json --minimal=0 "$PERF_DIR/fio/randread-4k.fio" 2>/dev/null \
            | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["jobs"][0]["read"]["iops"])'
    elif command -v lat_syscall >/dev/null; then
        lat_syscall null 2>&1 | awk '/syscall/{print $NF}'   # 越小越好=延迟
    else
        # 兜底：固定次数 getpid 循环耗时(秒)，越小越好
        python3 -c 'import os,time;t=time.time();[os.getpid() for _ in range(2000000)];print(time.time()-t)'
    fi
}

: > "$D/sweep.csv"; echo "freq_hz,interval_ms,metric,rounds,missed" >> "$D/sweep.csv"
for hz in $FREQS; do
    TRIG=""
    if [ "$hz" != 0 ]; then
        ms=$(awk "BEGIN{printf \"%.3f\", 1000/$hz}")
        "$PERF_DIR/trigger-loop.sh" "$ms" >/dev/null 2>&1 &
        TRIG=$!; sleep 0.3
    fi
    r0=$(awk '/rounds/{print $NF}' /proc/ikaslr/stats 2>/dev/null || echo 0)
    m0=$(awk '/missed/{print $NF}' /proc/ikaslr/stats 2>/dev/null || echo 0)
    metric=$(run_workload)
    r1=$(awk '/rounds/{print $NF}' /proc/ikaslr/stats 2>/dev/null || echo 0)
    m1=$(awk '/missed/{print $NF}' /proc/ikaslr/stats 2>/dev/null || echo 0)
    [ -n "$TRIG" ] && kill "$TRIG" 2>/dev/null
    printf "%s,%s,%s,%s,%s\n" "$hz" "${ms:-0}" "$metric" "$((r1-r0))" "$((m1-m0))" | tee -a "$D/sweep.csv"
done

echo
echo "== 完成： $D/sweep.csv"
echo "   列：freq_hz, interval_ms, metric(默认 fio IOPS), 本次实际随机化轮数, 错过次数"
echo "   作图：以 freq_hz 为横轴、metric 相对 freq=0 的开销% 为纵轴，即 §6.7.1 敏感性曲线。"
echo "   在图上标注：第4章实测触发频率区间(阴影) + §1.1.5 的 1.5–3.5s 间隔上界(竖线)。"
