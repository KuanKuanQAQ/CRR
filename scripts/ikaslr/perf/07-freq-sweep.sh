#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# E9　随机化频率敏感性（§6.7.1 / 图 6-3）—— 清单称"全文最关键实验图之一"。
#
# 在**已启动进某个 +R/+RD 档之后**跑：固定一个负载，人为扫触发频率，测开销曲线。
#
#   sudo ./07-freq-sweep.sh
#   FREQS="0 1 5 20 100 500" sudo ./07-freq-sweep.sh
#   WORKLOAD='fio --output-format=json ... | jq ...' sudo ./07-freq-sweep.sh
#
# 频率 0 = 不触发，作为本机的基线（注意：它**不是** Base 档，跳板税照付；
# 本图要看的是"频率带来的额外开销"，跳板税那一部分由 E3 给出）。
#
# 判据（清单，全章成败所系）：这张图要传达 **实测触发频率落在安全所要求的频率之上、
# 同时落在系统可承受的频率之下**。作图时在图上标两处：
#   · 第 4 章实测触发频率的分布区间（阴影带）
#   · §1.1.5 的 1.5–3.5 s 间隔上界对应的频率（竖虚线）
set -uo pipefail
. "$(dirname "$0")/env.sh"

[ -w /proc/ikaslr/trigger ] || {
    echo "!! 当前内核没有可写的 /proc/ikaslr/trigger。"
    echo "   需要先启动进 +R/+RD 档，并以 root 运行。"; exit 1; }

REL="$(uname -r)"; CFG="$(current_variant "$REL")"
FREQS="${FREQS:-0 1 5 20 50 100 200 500 1000}"
D="$RESULTS_DIR/$ARCH/freq-sweep/$CFG"; mkdir -p "$D"

# 只取一行、精确匹配键名。原来的 /rounds/ 会同时匹配 rounds 与 rounds_missed，
# 读出来的是后者——扫描曲线会整条错掉。
stat_of() { awk -v k="$1" '$1==k{print $2; exit}' /proc/ikaslr/stats; }

echo "== E9 频率扫描 @ $CFG ($REL)"
echo "   频率(Hz): $FREQS      每点重复: $REPEAT 次"
echo "   随机化函数数: $(stat_of functions)"

# 负载：默认取 fio randread-4k 的 IOPS（越大越好）。
# 用 WORKLOAD 整体替换时，要求它**只在 stdout 打印一个数**。
run_workload() {
    if [ -n "${WORKLOAD:-}" ]; then eval "$WORKLOAD"; return; fi
    if command -v fio >/dev/null && [ -r "$PERF_DIR/fio/randread-4k.fio" ]; then
        fio --output-format=json "$PERF_DIR/fio/randread-4k.fio" 2>/dev/null \
          | python3 -c 'import sys,json;print(json.load(sys.stdin)["jobs"][0]["read"]["iops"])'
    elif command -v lat_syscall >/dev/null; then
        lat_syscall null 2>&1 | awk '/syscall/{print $NF}'
    else
        # 兜底：固定次数的 getppid 循环耗时（秒，越小越好）
        python3 -c 'import os,time;t=time.time();[os.getppid() for _ in range(2000000)];print(round(time.time()-t,4))'
    fi
}

echo "freq_hz,interval_ms,rep,metric,rounds_done,rounds_missed,backoffs,forced,cp_ns" > "$D/sweep.csv"

for hz in $FREQS; do
    ms=0
    for rep in $(seq 1 "$REPEAT"); do
        TRIG=""
        if [ "$hz" != 0 ]; then
            ms=$(awk "BEGIN{printf \"%.3f\", 1000/$hz}")
            "$PERF_DIR/trigger-loop.sh" "$ms" >/dev/null 2>&1 &
            TRIG=$!
            sleep 0.3                     # 让触发循环先跑起来，避免头几秒是空档
        fi
        r0=$(stat_of rounds); m0=$(stat_of rounds_missed)
        b0=$(stat_of enter_backoffs); f0=$(stat_of enter_forced)
        metric=$(run_workload)
        r1=$(stat_of rounds); m1=$(stat_of rounds_missed)
        b1=$(stat_of enter_backoffs); f1=$(stat_of enter_forced)
        cp=$(stat_of critical_path_ns)
        [ -n "$TRIG" ] && kill "$TRIG" 2>/dev/null
        printf "%s,%s,%s,%s,%s,%s,%s,%s,%s\n" "$hz" "$ms" "$rep" "$metric" \
               "$((r1-r0))" "$((m1-m0))" "$((b1-b0))" "$((f1-f0))" "${cp:-0}" \
          | tee -a "$D/sweep.csv"
    done
done

echo
echo "== 完成: $D/sweep.csv"
python3 - "$D/sweep.csv" <<'PY'
import csv, statistics as st, sys
rows = list(csv.DictReader(open(sys.argv[1])))
by = {}
for r in rows:
    by.setdefault(float(r["freq_hz"]), []).append(r)
base = None
print()
print(f"{'频率(Hz)':>10}{'指标中位数':>16}{'相对 0Hz':>12}{'实际轮数/次':>14}{'错过':>8}{'回退':>12}")
for hz in sorted(by):
    ms = [float(r["metric"]) for r in by[hz]]
    med = st.median(ms)
    if base is None:
        base = med
    rd = st.median([int(r["rounds_done"]) for r in by[hz]])
    mi = st.median([int(r["rounds_missed"]) for r in by[hz]])
    bo = st.median([int(r["backoffs"]) for r in by[hz]])
    rel = f"{(med/base-1)*100:+.1f}%" if base else "—"
    print(f"{hz:>10.0f}{med:>16.4g}{rel:>12}{rd:>14.0f}{mi:>8.0f}{bo:>12,.0f}")
print()
print("  注：'相对 0Hz' 的正负号按指标本身的方向读——默认负载是 IOPS（越大越好），")
print("      因此负值表示变慢。换 WORKLOAD 时自己对齐方向。")
print("  注：'错过' 是 rounds_missed（没有就绪变体而放弃的轮次）；它随频率上升是预期的，")
print("      变体池只有 4 份，补充跟不上就会错过。")
PY
echo
echo "作图：横轴 freq_hz（**对数刻度**），纵轴相对 0Hz 的开销%。"
echo "     E3-A 的模型预测低频段应趋于水平（常驻税主导）——两条曲线的差额交给 E12。"
