#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# 步骤 5：在**已启动进某档内核之后**，跑该档的全部基准。
#
#   sudo ./05-run-suite.sh                    # 自动按 uname -r 识别当前档
#   sudo ./05-run-suite.sh RD                 # 手动指定档名
#   SERVER_IP=10.0.0.2 sudo ./05-run-suite.sh # 带网络项（需对端已起 netserver）
#   QUICK=1 sudo ./05-run-suite.sh            # 缩短版：先打通流程用，数据别用
#   REPEAT=7 sudo ./05-run-suite.sh           # 覆盖重复次数（默认 5）
#
# 结果落 $RESULTS_DIR/$ARCH/<档>/<负载>/rep<N>.txt，06-analyze.py 读这里。
#
# 三件事这个脚本会替你把住：
#   1. **非 base 档必须真有函数被随机化**，否则四档等价、数据无意义 —— 直接退出；
#   2. **逐个负载采样跨区域调用频度 G**（需 Rg 档，见 env.sh），把端到端结果接回
#      E3-A 的开销模型 `总开销 ≈ G × 常驻税 + M × K`；
#   3. **每项重复 REPEAT 次**（清单 §0.4：>=5 次，报中位数 + 四分位）。
set -uo pipefail
. "$(dirname "$0")/env.sh"

CFG="${1:-$(current_variant)}"
D="$RESULTS_DIR/$ARCH/$CFG"; mkdir -p "$D"
QUICK="${QUICK:-0}"
[ "$QUICK" = 1 ] && REPEAT=1

UBDIR="$BENCH_DIR/byte-unixbench/UnixBench"
LMBIN="$(ls -d "$BENCH_DIR"/lmbench/bin/* 2>/dev/null | head -1)"
[ -n "$LMBIN" ] && export PATH="$LMBIN:$PATH"

echo "=================================================================="
echo " 档: $CFG      内核: $(uname -r)      重复: $REPEAT 次$([ "$QUICK" = 1 ] && echo '  【QUICK：数据不可用】')"
echo "=================================================================="

# ---------------- 合理性自检：拦住"白跑一轮"的几种情况 ----------------
fail() { echo "!! $*" >&2; exit 1; }

if [ "$CFG" != base ]; then
    [ -r /proc/ikaslr/stats ] || fail "没有 /proc/ikaslr/stats：这个内核没开 CONFIG_IKASLR，但档名是 $CFG"
    NF=$(awk '/^functions/{print $2}' /proc/ikaslr/stats)
    echo "== 随机化函数数: $NF"
    [ "${NF:-0}" -gt 3 ] || fail "随机化函数数 = ${NF:-0}（只有内建示例）。
   多半是编内核时没给 CRR_TRAMPOLINE=y / IKASLR_FUNCS。
   这一档与 base 等价，测出来的开销没有意义。回去看 02-build-kernels.sh。"
    [ -w /proc/ikaslr/trigger ] || fail "/proc/ikaslr/trigger 不可写（需要 root？）"
else
    [ -r /proc/ikaslr/stats ] && fail "base 档不该有 /proc/ikaslr/stats —— 你可能启动错了内核"
fi

# G 采样只在 Rg 档（开了 STATS）有意义
HAVE_G=0
if [ -r /proc/ikaslr/stats ] && awk '/^enters/{exit ($2=="")}' /proc/ikaslr/stats 2>/dev/null; then
    grep -q '^enters' /proc/ikaslr/stats && HAVE_G=1
fi
[ "$HAVE_G" = 1 ] && echo "== 本档开着 STATS，会逐个负载采样 G（跨区域调用频度）"

# ---------------- 环境快照（复现与降噪核对）----------------
{
    echo "date=$(date -Is)"
    echo "variant=$CFG"
    echo "uname=$(uname -a)"
    echo "cmdline=$(cat /proc/cmdline)"
    echo "nproc=$(nproc)"
    echo "repeat=$REPEAT"
    echo "quick=$QUICK"
    echo "governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
    echo "no_turbo=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo n/a)"
    echo "trigger_ms=$TRIGGER_MS"
    [ -r /proc/ikaslr/stats ] && awk '/^(functions|rand_region_bytes|whitelist_entries)/{print}' /proc/ikaslr/stats
} > "$D/_env.txt"
[ -r /proc/ikaslr/stats ] && cp /proc/ikaslr/stats "$D/_stats_before.txt"

# ---------------- 降噪（清单 §0.4）----------------
command -v cpupower >/dev/null && cpupower frequency-set -g performance >/dev/null 2>&1
w() { [ -w "$1" ] && printf '%s' "$2" > "$1" 2>/dev/null; return 0; }
w /sys/devices/system/cpu/intel_pstate/no_turbo 1   # Intel: 1 = 关 turbo
w /sys/devices/system/cpu/cpufreq/boost 0           # AMD/acpi-cpufreq: 0 = 关 boost

# ---------------- 定频触发随机化 ----------------
TRIG_PID=""
if variant_needs_trigger "$CFG"; then
    echo "== 后台定频触发随机化：每 ${TRIGGER_MS} ms"
    "$PERF_DIR/trigger-loop.sh" "$TRIGGER_MS" > "$D/_trigger.log" 2>&1 &
    TRIG_PID=$!
fi
cleanup() { [ -n "$TRIG_PID" ] && kill "$TRIG_PID" 2>/dev/null; }
trap cleanup EXIT

# ---------------- 跑一项负载 ----------------
# bench <名字> <命令...>
#   跑 REPEAT 次，每次的原始输出存 <名字>/rep<N>.txt；
#   若本档开着 STATS，同时把该负载期间的 enters/outs 增量写进 <名字>/g.csv。
stat_field() { awk -v k="$1" '$1==k{print $2}' /proc/ikaslr/stats 2>/dev/null; }

bench() {
    local name="$1"; shift
    local dir="$D/$name" i t0 t1 e0 o0 e1 o1
    mkdir -p "$dir"
    echo "  -- $name"
    [ "$HAVE_G" = 1 ] && echo "rep,secs,d_enters,d_outs" > "$dir/g.csv"
    for i in $(seq 1 "$REPEAT"); do
        if [ "$HAVE_G" = 1 ]; then
            e0=$(stat_field enters); o0=$(stat_field outs)
        fi
        t0=$(date +%s.%N)
        "$@" > "$dir/rep$i.txt" 2>&1
        t1=$(date +%s.%N)
        if [ "$HAVE_G" = 1 ]; then
            e1=$(stat_field enters); o1=$(stat_field outs)
            awk -v r="$i" -v a="$t0" -v b="$t1" -v e0="$e0" -v e1="$e1" \
                -v o0="$o0" -v o1="$o1" \
                'BEGIN{printf "%d,%.3f,%d,%d\n", r, b-a, e1-e0, o1-o0}' >> "$dir/g.csv"
        fi
    done
}

# ================================================================
# 一、LMBench 微基准（§6.5.3；跳板税最直接的体现）
# ================================================================
if command -v lat_syscall >/dev/null; then
    # lat_syscall 的 read/write/open/stat 都要一个文件参数
    TF="$D/_scratch"; dd if=/dev/zero of="$TF" bs=1M count=8 status=none
    lm_syscall() {
        for op in null read write open stat; do
            case "$op" in null) lat_syscall null ;; *) lat_syscall "$op" "$TF" ;; esac
        done
    }
    bench lat_syscall lm_syscall
    bench lat_ctx     lat_ctx -s 0 2 4 8
    lm_proc() { for k in fork exec shell; do lat_proc "$k"; done; }
    bench lat_proc    lm_proc
    bench lat_pagefault lat_pagefault "$TF"
    lm_bw() { bw_mem 256m rd; bw_mem 256m wr; }
    bench bw_mem      lm_bw          # 对照组：内存带宽应无变化
else
    echo "  (lmbench 未装，跳过；见 01-install-deps.sh)"
fi

# ================================================================
# 二、UnixBench（内核路径综合）
# ================================================================
if [ -x "$UBDIR/Run" ]; then
    ub() { ( cd "$UBDIR" && ./Run -q -c 1 -i 1 syscall pipe context1 spawn execl shell1 shell8 ); }
    bench unixbench ub
else
    echo "  (UnixBench 未装，跳过)"
fi

# ================================================================
# 三、cyclictest 中断与调度延迟（§6.5.3）
#     清单要求 -l 1000000；那是每次约 200 s。CYCLIC_LOOPS 可调，默认 200000（约 40 s）。
#     **正式出数请用 1000000**，并报 max 与 99.9 分位。
# ================================================================
if command -v cyclictest >/dev/null; then
    CL="${CYCLIC_LOOPS:-200000}"; [ "$QUICK" = 1 ] && CL=20000
    bench cyclictest cyclictest -q -l "$CL" -m -S -p 90 -i 200 -h 400
else
    echo "  (cyclictest 未装，跳过)"
fi

# ================================================================
# 四、计算负载（SPEC 的免费替代；清单已把 SPEC 改为估算，见 §第4章 E5）
# ================================================================
SEVENZ="$(command -v 7z || command -v 7za || true)"
[ -n "$SEVENZ" ]              && bench 7z "$SEVENZ" b
command -v openssl >/dev/null && bench openssl openssl speed -evp aes-256-gcm
command -v sysbench >/dev/null && bench sysbench sysbench cpu --threads="$(nproc)" --time=20 run

# ================================================================
# 五、存储 fio（E7；走 VFS 热路径）
#     判据：小块尺寸每单位数据跨更多次区域边界，开销应显著高于大块。
# ================================================================
if command -v fio >/dev/null; then
    for j in randread-4k randwrite-4k seqread-1m randrw-qd; do
        [ -r "$PERF_DIR/fio/$j.fio" ] || continue
        bench "fio_$j" fio --output-format=json "$PERF_DIR/fio/$j.fio"
    done
else
    echo "  (fio 未装，跳过)"
fi

# ================================================================
# 六、网络（E6；需对端 netserver / HTTP 服务）
#     判据同上：小报文的开销应显著高于大报文。
# ================================================================
if [ -n "$SERVER_IP" ] && command -v netperf >/dev/null; then
    for m in 64 256 1024 16384; do
        bench "netperf_stream_$m" netperf -H "$SERVER_IP" -t TCP_STREAM -l 20 -- -m "$m"
    done
    bench netperf_rr netperf -H "$SERVER_IP" -t TCP_RR -l 20 -- -r 1,1
    command -v wrk >/dev/null && bench wrk wrk -t8 -c256 -d20s "http://$SERVER_IP/"
elif [ -z "$SERVER_IP" ]; then
    echo "  (未设 SERVER_IP，跳过网络项；E6 需要它)"
fi

# ---------------- 收尾 ----------------
[ -r /proc/ikaslr/stats ] && cp /proc/ikaslr/stats "$D/_stats_after.txt"
cleanup; TRIG_PID=""

echo
echo "== 完成： $D"
if [ -r "$D/_stats_after.txt" ]; then
    echo "-- 本档随机化统计（供 E3-A 的 M 项与 E12 归因）"
    awk '/^(rounds|critical_path_ns|rounds_missed|enter_backoffs|enter_forced|nested_admitted|whitelist_rejects|stale_fixup_fails)/{printf "     %-20s %s\n",$1,$2}' "$D/_stats_after.txt"
fi
echo
echo "下一步：把四档都跑完（04-boot-into.sh 逐档重启），然后 ./06-analyze.py"
