#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 步骤 E8：与 Adelie / Dbox 同类负载对比（§6.6）。跑 Adelie 论文所用的**同类**
# 负载(sysbench fileio、O_DIRECT 读吞吐、sysbench OLTP/mySQL、ApacheBench/wrk、
# Kernbench)，触发间隔取 Adelie 的 **20ms**，便于把本文数字摆在它们的数字旁边。
# 在**已启动进某档**后跑；对比方法与文献数字见 11-related-comparison.md。
#   sudo ./10-compare-adelie-dbox.sh
# 注：Adelie/Dbox 开源实现难复现，故本脚本只产出**本文**在同类负载/同频率下的数字，
#     与它们论文报告的开销数字对照（非同机复现，见对比文档）。
set -uo pipefail
. "$(dirname "$0")/env.sh"

REL="$(uname -r)"; CFG="$(current_variant "$REL")"
export TRIGGER_MS=20                                  # 对齐 Adelie
D="$RESULTS_DIR/$ARCH/compare/$CFG"; mkdir -p "$D"
SCRATCH="${SCRATCH:-/var/tmp/ikaslr-scratch}"; mkdir -p "$SCRATCH"
echo "== Adelie/Dbox 同类负载 @ $CFG，触发间隔 ${TRIGGER_MS}ms"

TRIG=""
if variant_needs_trigger "$CFG" && [ -w /proc/ikaslr/trigger ]; then
    "$PERF_DIR/trigger-loop.sh" "$TRIGGER_MS" >/dev/null 2>&1 & TRIG=$!
fi
trap '[ -n "$TRIG" ] && kill "$TRIG" 2>/dev/null || true' EXIT
run(){ echo "  -- $*"; }

# 1) sysbench fileio 随机/顺序读（cached，对齐 Adelie 的 sysbench file_io NVMe）
if command -v sysbench >/dev/null; then
    ( cd "$SCRATCH" && sysbench fileio --file-total-size=2G prepare ) >/dev/null 2>&1 || true
    for mode in rndrd seqrd; do
        run "sysbench fileio $mode"
        ( cd "$SCRATCH" && sysbench fileio --file-total-size=2G --file-test-mode=$mode \
            --file-io-mode=sync --time=20 run ) >"$D/sysbench_fileio_$mode.txt" 2>&1 || true
    done
    ( cd "$SCRATCH" && sysbench fileio --file-total-size=2G cleanup ) >/dev/null 2>&1 || true
fi

# 2) O_DIRECT 读吞吐（对齐 Adelie 的 NVMe read throughput 微基准）
if command -v fio >/dev/null; then
    run "fio O_DIRECT read"
    fio --name=odirect_read --filename="$SCRATCH/odf" --size=2G --bs=512 --rw=read \
        --direct=1 --sync=1 --runtime=20 --time_based --output-format=json \
        >"$D/fio_odirect_read.json" 2>&1 || true
fi

# 3) sysbench OLTP / mySQL（对齐 Adelie 的 mySQL OLTP；需已装并配置 mysql）
if command -v sysbench >/dev/null && command -v mysql >/dev/null; then
    run "sysbench oltp (mysql)  —— 需自行建库/账号，见 11-related-comparison.md"
    sysbench oltp_read_write --db-driver=mysql --mysql-db=sbtest \
        --mysql-user="${MYSQL_USER:-root}" --time=20 --threads="$(nproc)" run \
        >"$D/sysbench_oltp.txt" 2>&1 || echo "     (mysql 未就绪，跳过)"
fi

# 4) ApacheBench/wrk（对齐 Adelie 的 ApacheBench 512B–8KB）
if [ -n "${SERVER_IP:-}" ] && command -v wrk >/dev/null; then
    for sz in 512 4096 8192; do
        run "wrk ${sz}B"
        wrk -t8 -c256 -d15s "http://$SERVER_IP/${sz}.bin" >"$D/wrk_$sz.txt" 2>&1 || true
    done
elif command -v ab >/dev/null && command -v nginx >/dev/null; then
    run "ApacheBench(本机 nginx)"; ab -n 50000 -c 64 http://127.0.0.1/ >"$D/ab.txt" 2>&1 || true
fi

# 5) Kernbench（对齐 Adelie 的 Kernbench：内核编译吞吐）；计时一次内核 image 构建
if [ -d "$ROOT" ]; then
    run "kernbench (计时一次 $ARCH 内核 image 构建)"
    KB="$BUILD_ROOT/_kernbench"; mkdir -p "$KB"
    kbuild "$KB" CRR_TRAMPOLINE=n "$( [ "$ARCH" = x86_64 ] && echo x86_64_defconfig || echo defconfig)" >/dev/null 2>&1
    kbuild "$KB" CRR_TRAMPOLINE=n clean >/dev/null 2>&1 || true
    /usr/bin/time -v kbuild "$KB" CRR_TRAMPOLINE=n -j"$JOBS" \
        "$( [ "$ARCH" = x86_64 ] && echo bzImage || echo Image)" >"$D/kernbench.txt" 2>&1 || true
fi

[ -r /proc/ikaslr/stats ] && cat /proc/ikaslr/stats >"$D/ikaslr_stats.txt"
echo
echo "== 完成： $D"
echo "   把本档数字与 11-related-comparison.md 里 Adelie(<2%)/Dbox(<3.6%) 的报告数字对照。"
echo "   公平性：本文范围是 built-in（更大），另跑一档 scoped(限到网络/存储热路径) 才与 Adelie 同范围。"
