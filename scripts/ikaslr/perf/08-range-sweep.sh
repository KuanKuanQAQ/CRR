#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# E10　随机化范围敏感性（§6.7.2）+ E5-B 的降幅曲线（清单要求两者**合并做**）。
#
# 自变量 = 随机化的函数数。为每个规模编译并安装一个内核（LOCALVERSION 带规模），
# 供逐档重启测。
#
#   sudo ./08-range-sweep.sh                 # 默认 16 / 64 / 256 / 全量(S3)
#   SCOPES="e10-16 e10-64 s3-full" sudo ./08-range-sweep.sh
#
# **名单直接用 funcs/e10-*.txt，不要现切**。那几份是从已清理的 s3-full 排序里切出来
# 的（各档之间有包含关系），而 s3-full 本身经过了"编译 → verify_movable → 剔除 →
# 重编"的迭代。用 `head -n` 从别处现切会把已剔除的不可搬移函数放回来，编出来的内核
# 一搬就崩——这条踩过。
#
# 为什么必须做（清单）：**这组数据是三章之间参数耦合的唯一定量呈现**。缺了它，
# 三章的取值看起来是各自拍定的。
set -uo pipefail
. "$(dirname "$0")/env.sh"
[ "$(id -u)" = 0 ] || { echo "需要 root（要 make install）： sudo $0 $*"; exit 1; }

SCOPES="${SCOPES:-e10-16 e10-64 e10-256 s3-full}"
IMG=$([ "$ARCH" = x86_64 ] && echo bzImage || echo Image)
defcfg=$([ "$ARCH" = x86_64 ] && echo x86_64_defconfig || echo defconfig)

echo "== E10 范围扫描，规模序列: $SCOPES"
for sc in $SCOPES; do
    F="$ROOT/scripts/ikaslr/funcs/$sc.txt"
    [ -r "$F" ] || { echo "!! 缺名单 $F，跳过"; continue; }
    N=$(grep -vc '^#' "$F") || N=0
    [ "$N" -gt 0 ] || { echo "!! $sc 名单为空，跳过（e10-1024 目前就是空的，见文件内说明）"; continue; }

    O="$BUILD_ROOT/scope-$sc"; mkdir -p "$O"
    echo "=================== 范围 $sc（$N 个函数）-> $O ==================="
    kbuild "$O" CRR_TRAMPOLINE=y IKASLR_FUNCS="$F" "$defcfg" >/dev/null
    # shellcheck disable=SC2046
    "$ROOT/scripts/config" --file "$O/.config" \
        --set-str LOCALVERSION "-ikaslr-$sc" --disable LOCALVERSION_AUTO \
        $(ikaslr_mandatory_config) $(variant_config_args Rg)
    #  ↑ 用 Rg（R + STATS）：范围扫描要读 enters/outs 算跨区域调用占比（E5-B 的降幅），
    #    那需要 STATS。统计本身只值 1.0~1.6 ns/次，不影响趋势。
    kbuild "$O" CRR_TRAMPOLINE=y IKASLR_FUNCS="$F" olddefconfig >/dev/null

    if ! kbuild "$O" CRR_TRAMPOLINE=y IKASLR_FUNCS="$F" -j"$JOBS" "$IMG" modules \
            > "$O/build.log" 2>&1; then
        echo "!! $sc 编译失败，见 $O/build.log"; continue
    fi
    if ! python3 "$ROOT/scripts/ikaslr/verify_movable.py" "$O/vmlinux" --arch "$ARCH" \
            > "$O/movable.txt" 2>&1; then
        echo "!! $sc 有不可搬移的函数，**不要用这一档的数据**，见 $O/movable.txt"
        continue
    fi
    kbuild "$O" CRR_TRAMPOLINE=y IKASLR_FUNCS="$F" -j"$JOBS" modules_install >/dev/null
    kbuild "$O" CRR_TRAMPOLINE=y IKASLR_FUNCS="$F" install
    echo "== $sc 已装： $(cat "$O/include/config/kernel.release")"
done

command -v update-grub >/dev/null && update-grub \
    || grub2-mkconfig -o /boot/grub2/grub.cfg 2>/dev/null || true

cat <<NEXT

== 各规模内核已装。逐个测（每个规模一轮重启）：

     sudo ./04-boot-into.sh scope-<规模> --now
     # 启动后：
     sudo ./08b-range-collect.sh           # 采本档的范围相关量（见下）
     sudo ./05-run-suite.sh scope-<规模>   # 端到端开销（可选，慢）

   §6.7.2 + E5-B 需要的六个量，来源分别是：
     随机化区域函数数          /proc/ikaslr/stats  functions
     单次随机化耗时            /proc/ikaslr/stats  critical_path_ns（触发若干次取分布）
     跨区域调用频度 / 占比      /proc/ikaslr/stats  enters + outs（需 STATS，已开）
     PA 验证降幅               = 跨区域间接调用占比（ARM，第 5 章；本机 x86 只能给前项）
     内存开销                  变体池 = 4 × 容量（启动日志 "variant pool"）
     端到端开销                05-run-suite.sh 的对比

   全部规模跑完后： ./08c-range-analyze.py
NEXT
