#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 步骤 E10：随机化范围敏感性（§6.7.2）。自变量 = 随机化的函数数（funcs.txt 规模）。
# 为若干规模各编译并安装一个内核（LOCALVERSION 带规模），供逐档重启测。
#   sudo ./08-range-sweep.sh            # 用默认规模序列
#   SIZES="16 64 256 1024" sudo ./08-range-sweep.sh
#
# 依赖编译器 pass：范围要真的变，必须 CRR_TRAMPOLINE=y（按 funcs.txt 随机化指定函数）。
# 各规模的名单取 master funcs.txt 的前 N 行（先小后大、包含关系，便于看单调趋势）。
set -uo pipefail
. "$(dirname "$0")/env.sh"
[ "$(id -u)" = 0 ] || { echo "需要 root（要 install）： sudo $0 $*"; exit 1; }

SIZES="${SIZES:-16 64 256 1024}"
MASTER="${IKASLR_FUNCS:-$PERF_DIR/funcs.txt}"
AVAIL=$(grep -vcE '^\s*#|^\s*$' "$MASTER" 2>/dev/null || echo 0)
[ "$CRR_TRAMPOLINE" = y ] || { echo "!! 需 CRR_TRAMPOLINE=y 且 master 名单 $MASTER 足够大（现有 $AVAIL 条）"; \
                               echo "   否则范围不会真的变化。用： CRR_TRAMPOLINE=y IKASLR_FUNCS=大名单 sudo $0"; exit 1; }

for n in $SIZES; do
    if [ "$n" -gt "$AVAIL" ]; then echo "!! 规模 $n 超过名单条数 $AVAIL，跳过"; continue; fi
    SUB="$PERF_DIR/funcs-$n.txt"
    grep -vE '^\s*#|^\s*$' "$MASTER" | head -n "$n" > "$SUB"
    O="$BUILD_ROOT/scope$n"; mkdir -p "$O"
    echo "=================== 范围 $n 函数 -> $O ==================="
    kbuild "$O" CRR_TRAMPOLINE=n "$( [ "$ARCH" = x86_64 ] && echo x86_64_defconfig || echo defconfig)" >/dev/null
    # shellcheck disable=SC2046
    "$ROOT/scripts/config" --file "$O/.config" --set-str LOCALVERSION "-ikaslr-scope$n" \
        $(variant_config_args R)
    kbuild "$O" CRR_TRAMPOLINE=n olddefconfig >/dev/null
    [ "${LOCALMOD:-0}" = 1 ] && { yes '' | kbuild "$O" CRR_TRAMPOLINE=n LSMOD=/proc/modules localmodconfig >/dev/null 2>&1 || true; }

    IMG=$([ "$ARCH" = x86_64 ] && echo bzImage || echo Image)
    IKASLR_FUNCS="$SUB" kbuild "$O" CRR_TRAMPOLINE=y -j"$JOBS" "$IMG" modules
    kbuild "$O" CRR_TRAMPOLINE=y -j"$JOBS" modules_install >/dev/null
    kbuild "$O" CRR_TRAMPOLINE=y install
    echo "== scope$n 已装： $(cat "$O/include/config/kernel.release")"
done
command -v update-grub >/dev/null && update-grub || grub2-mkconfig -o /boot/grub2/grub.cfg 2>/dev/null || true

cat <<'NEXT'

== 各范围内核已装。逐个测（每个一轮）：
   sudo ./04-boot-into.sh scope<N> --now      # 需 04 支持任意档名（已支持，按 uname -r 找）
   启动后，读内核内指标 + 跑负载：
     cat /proc/ikaslr/layout    > results/<arch>/range-sweep/scope<N>_layout.txt   # 随机化函数数
     cat /proc/ikaslr/stats     > results/<arch>/range-sweep/scope<N>_stats.txt    # 单次随机化耗时等
     sudo ./05-run-suite.sh scope<N>                                               # 端到端开销
   §6.7.2 需要的量：随机化区域函数数、单次随机化耗时、跨区域间接调用占比、
   PA 验证降幅、端到端开销 —— 前几项来自 /proc/ikaslr，最后来自 05 的对比。
NEXT
