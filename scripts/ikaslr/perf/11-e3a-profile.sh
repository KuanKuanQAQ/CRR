#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# E3-A：跳板机制的完整开销剖面。
# 对应 Documentation/crr/设计文档/草稿/实验总清单.md §第1章 E3-A。
#
#   用法: ./11-e3a-profile.sh [输出目录]
#
# 采集三块数据（见 tools/ikaslr/init_e3a.c 的说明）：
#   常驻税 A–E     —— 五档微基准，**STATS 双档**分别测，以便把"统计本身的开销"
#                      单列（清单 §0.3 的报数纪律）
#   随机化代价 H–M —— 逐次触发采样三阶段；其中 I（更新全部 target）要证明与函数数
#                      成正比，因此**扫四个范围规模**
#   跨区域频度 G   —— 固定墙钟内 enters/outs 的增量，分空闲与负载
#
# 平台口径：本脚本在 QEMU+KVM 上跑。微基准循环全在客户机内、不产生 VM exit，
# 因此周期数接近裸机；但**绝对值仍应在裸机复测**（清单 §0.2）。
set -euo pipefail
. "$(dirname "$0")/env.sh"

OUT="${1:-$RESULTS_DIR/e3a-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"
IRFS="$OUT/e3a.cpio.gz"
"$ROOT/scripts/ikaslr/mkinitramfs.sh" "$ROOT/tools/ikaslr/init_e3a.c" "$IRFS" >/dev/null

[ "$ARCH" = x86_64 ] || { echo "!! 本脚本只做 x86_64；arm64 的 E3-A 含档 F(PA)，另行处理"; exit 1; }

# 一档 = 名单 + STATS + DEBUG
#   名单规模   -> I 项的正比性
#   STATS 双档 -> 把"统计本身的开销"单列（清单 §0.3）
#   DEBUG=n    -> 生产配置下的随机化代价；DEBUG=y 才有 /proc/ikaslr/bench，
#                 所以常驻税只能在 DEBUG=y 下测，两者分工明确
declare -a CASES=(
    "s3-full   y y"   # 主档：常驻税 A–E 与 G 都取自这一档
    "s3-full   n y"   # 关统计：与主档的差额即统计本身的开销
    "s3-full   n n"   # 生产配置：随机化代价 H–M 取这一档
    "e10-16    y y"
    "e10-64    y y"
    "e10-256   y y"
)

build_one() {   # $1=名单名 $2=STATS(y/n) $3=DEBUG(y/n)
    local list="$1" stats="$2" dbg="$3" o="$BUILD_ROOT/e3a-$1-s$2-d$3"
    local funcs="$ROOT/scripts/ikaslr/funcs/$list.txt"
    [ -r "$funcs" ] || { echo "!! 缺名单 $funcs"; return 1; }
    mkdir -p "$o"
    echo "== 编译 范围=$list STATS=$stats DEBUG=$dbg -> $o"
    kbuild "$o" CRR_TRAMPOLINE=y IKASLR_FUNCS="$funcs" x86_64_defconfig >/dev/null
    # shellcheck disable=SC2046
    "$ROOT/scripts/config" --file "$o/.config" \
        --set-str LOCALVERSION "-e3a-$list-s$stats-d$dbg" --disable LOCALVERSION_AUTO \
        $(ikaslr_mandatory_config) --enable IKASLR \
        $([ "$stats" = y ] && echo --enable IKASLR_STATS || echo --disable IKASLR_STATS) \
        $([ "$dbg" = y ] && echo --enable IKASLR_DEBUG || echo --disable IKASLR_DEBUG)
    kbuild "$o" CRR_TRAMPOLINE=y IKASLR_FUNCS="$funcs" olddefconfig >/dev/null
    kbuild "$o" CRR_TRAMPOLINE=y IKASLR_FUNCS="$funcs" -j"$JOBS" bzImage \
        > "$o/build.log" 2>&1 || { echo "!! 编译失败，见 $o/build.log"; return 1; }
    # 可搬移性是硬前提：不合格就别拿它的数字
    python3 "$ROOT/scripts/ikaslr/verify_movable.py" "$o/vmlinux" --arch x86_64 \
        > "$o/movable.txt" 2>&1 || { echo "!! $list 有不可搬移函数，见 $o/movable.txt"; return 1; }
    echo "$o"
}

run_one() {     # $1=构建目录 $2=标签
    local o="$1" tag="$2"
    echo "== 采集 $tag"
    # 单核跑微基准：多核会让缓存与调度噪声进入十纳秒量级的测量
    timeout 600 qemu-system-x86_64 -enable-kvm -cpu host -smp 1 -m 2G \
        -kernel "$o/arch/x86/boot/bzImage" -initrd "$IRFS" -nographic \
        -append "console=ttyS0 nokaslr panic=-1 processor.max_cstate=1 intel_idle.max_cstate=0" \
        > "$OUT/$tag.log" 2>&1 || true
    grep -c '^E3A-DONE' "$OUT/$tag.log" >/dev/null 2>&1 || true
    if ! grep -q '^E3A-DONE' "$OUT/$tag.log"; then
        echo "   !! $tag 未跑完，见 $OUT/$tag.log"
        return 1
    fi
    grep -E '^E3A-' "$OUT/$tag.log" > "$OUT/$tag.e3a"
    echo "   -> $OUT/$tag.e3a"
}

# 一档失败不能带倒整轮：把每档的成败如实记下，继续下一档。
FAILED=()
for c in "${CASES[@]}"; do
    read -r list stats dbg <<<"$c"
    tag="$list-s$stats-d$dbg"
    if o=$(build_one "$list" "$stats" "$dbg" | tail -1); then
        run_one "$o" "$tag" || FAILED+=("$tag(采集)")
    else
        echo "!! 跳过 $tag：编译或可搬移性校验未通过"
        FAILED+=("$tag(编译)")
    fi
done
[ ${#FAILED[@]} -eq 0 ] || printf '!! 未完成的档: %s\n' "${FAILED[*]}"

echo
echo "== 汇总"
python3 "$ROOT/scripts/ikaslr/e3a_collect.py" "$OUT" | tee "$OUT/summary.txt"
echo
echo "原始日志与汇总: $OUT"
