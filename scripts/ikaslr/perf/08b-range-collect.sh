#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# E10/E5-B 的逐档采集：在**已启动进某个 scope-* 内核之后**跑，采该规模的六个量。
#
#   sudo ./08b-range-collect.sh              # 档名按 uname -r 自动识别
#   sudo ./08b-range-collect.sh scope-e10-64 # 手动指定
#
# 采完全部规模后： ./08c-range-analyze.py
set -uo pipefail
. "$(dirname "$0")/env.sh"

CFG="${1:-}"
if [ -z "$CFG" ]; then
    CFG="$(uname -r)"; CFG="${CFG#*-ikaslr-}"; CFG="scope-${CFG%+}"
fi
[ -r /proc/ikaslr/stats ] || { echo "!! 没有 /proc/ikaslr/stats：启动错内核了？"; exit 1; }

D="$RESULTS_DIR/$ARCH/range-sweep"; mkdir -p "$D"
stat_of() { awk -v k="$1" '$1==k{print $2; exit}' /proc/ikaslr/stats; }

NF=$(stat_of functions)
echo "== 采集 $CFG（$(uname -r)），随机化函数数 $NF"
[ "${NF:-0}" -gt 3 ] || { echo "!! 函数数 = ${NF:-0}，这一档没有真正随机化，数据不可用"; exit 1; }

# ---- 一、单次随机化耗时的分布：触发 N 次，每次读三阶段 ----
ROUNDS="${ROUNDS:-120}"
[ -w /proc/ikaslr/trigger ] || { echo "!! /proc/ikaslr/trigger 不可写（需 root）"; exit 1; }
: > "$D/$CFG.rounds.csv"
echo "i,done,cp_ns,wait_ns,update_ns,remap_ns,prep_ns" >> "$D/$CFG.rounds.csv"
for i in $(seq 1 "$ROUNDS"); do
    r0=$(stat_of rounds)
    echo 1 > /proc/ikaslr/trigger
    r1=$(stat_of rounds)
    printf "%d,%d,%s,%s,%s,%s,%s\n" "$i" "$([ "$r1" -gt "$r0" ] && echo 1 || echo 0)" \
        "$(stat_of critical_path_ns)" "$(stat_of cp_wait_ns)" \
        "$(stat_of cp_update_ns)" "$(stat_of cp_remap_ns)" "$(stat_of prepare_ns)" \
        >> "$D/$CFG.rounds.csv"
    sleep 0.005
done

# ---- 二、跨区域调用频度：固定负载下的 enters/outs 增量 ----
# 与 E3-A 的 G 项同口径。默认用一个纯系统调用循环，5 秒。
G_SECS="${G_SECS:-5}"
e0=$(stat_of enters); o0=$(stat_of outs)
if [ -z "$e0" ]; then
    echo "   （本档没开 CONFIG_IKASLR_STATS，跳过 G 采样；08 默认用 Rg 配置，应该是开着的）"
else
    t0=$(date +%s.%N)
    timeout "$G_SECS" bash -c 'while :; do cat /proc/uptime >/dev/null; stat /proc >/dev/null; done' || true
    t1=$(date +%s.%N)
    e1=$(stat_of enters); o1=$(stat_of outs)
    awk -v a="$t0" -v b="$t1" -v e0="$e0" -v e1="$e1" -v o0="$o0" -v o1="$o1" \
        'BEGIN{printf "secs=%.3f d_enters=%d d_outs=%d g_in=%.0f g_out=%.0f\n",
               b-a, e1-e0, o1-o0, (e1-e0)/(b-a), (o1-o0)/(b-a)}' > "$D/$CFG.g.txt"
    cat "$D/$CFG.g.txt"
fi

# ---- 三、静态量与内存 ----
{
    echo "variant=$CFG"
    echo "release=$(uname -r)"
    echo "functions=$NF"
    echo "rand_region_bytes=$(stat_of rand_region_bytes)"
    echo "tramp_region_bytes=$(stat_of tramp_region_bytes)"
    echo "whitelist_entries=$(stat_of whitelist_entries)"
    dmesg 2>/dev/null | grep -oE 'variant pool: [0-9]+ variants x [0-9]+ B' | head -1
} > "$D/$CFG.env.txt"
cp /proc/ikaslr/stats "$D/$CFG.stats.txt"

echo "== 完成： $D/$CFG.*"
echo "   全部规模采完后： ./08c-range-analyze.py"
