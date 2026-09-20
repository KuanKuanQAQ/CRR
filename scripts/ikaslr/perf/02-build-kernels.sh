#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 步骤 2：为每档配置编译一个内核。
#   ./02-build-kernels.sh                    # 编全部档（默认 S1 范围）
#   ./02-build-kernels.sh base R             # 只编指定档
#   IKASLR_FUNCS=.../s3-full.txt ./02-build-kernels.sh    # 换随机化范围
#
# 三条纪律：
#  1. 各档从**同一份 defconfig** 出发，只 toggle IKASLR 相关 CONFIG；
#  2. `ikaslr_mandatory_config` 的那几项**四档一律相同**——它们是"代码可搬移"
#     的前提（retpoline / BTI / unwinder），不是可选项；
#  3. **所有非 base 档用同一份 IKASLR_FUNCS**，否则范围与机制两个变量混在一起，
#     E12 归因失效（见 实现/15-scope-selection.md §3.3）。
#  4. **base 档用空名单而不是关掉插件**——四档的编译流水线必须一致，
#     见 env.sh 里 IKASLR_FUNCS_BASE 的说明。
set -euo pipefail
. "$(dirname "$0")/env.sh"

WANT=("$@"); [ ${#WANT[@]} -eq 0 ] && WANT=("${VARIANTS[@]}")

if [ "$CRR_TRAMPOLINE" != y ]; then
    cat <<'W'
!! CRR_TRAMPOLINE=n：不会有任何真实内核函数被随机化，四档等价。
   这样测出来的"开销"没有意义。除非你只是想跑通流程，否则请设 CRR_TRAMPOLINE=y。
W
fi

# 插件得先编出来
if [ "$CRR_TRAMPOLINE" = y ] && [ ! -f "$ROOT/tools/ikaslr/llvm/libIKaslrPass.so" ]; then
    echo "== 先编译 LLVM pass"
    make -C "$ROOT/tools/ikaslr/llvm"
fi

echo ">> 编译器 : $IKASLR_CC"
NFUNCS=$(grep -vc '^#' "$IKASLR_FUNCS" 2>/dev/null) || NFUNCS=0
echo ">> 随机化范围: $IKASLR_FUNCS ($NFUNCS 个函数)"
echo ">> base 档名单: $IKASLR_FUNCS_BASE (空，仅为保持编译流水线一致)"
[ "$NFUNCS" -gt 0 ] || { echo "!! 名单为空，非 base 档不会有任何函数被随机化"; exit 1; }

defconfig_target() { [ "$ARCH" = x86_64 ] && echo x86_64_defconfig || echo defconfig; }
IMG=$([ "$ARCH" = x86_64 ] && echo bzImage || echo Image)

for v in "${WANT[@]}"; do
    O="$BUILD_ROOT/$v"
    echo "=================== 编 $v -> $O ==================="
    mkdir -p "$O"
    # shellcheck disable=SC2046
    kbuild "$O" $(ikaslr_pass_args "$v") "$(defconfig_target)" >/dev/null

    # LOCALVERSION 让每档有独立的 `uname -r`，从而 /boot 与 GRUB 各成一项。
    # shellcheck disable=SC2046
    "$ROOT/scripts/config" --file "$O/.config" \
        --set-str LOCALVERSION "-ikaslr-$v" --disable LOCALVERSION_AUTO \
        $(ikaslr_mandatory_config) \
        $(variant_config_args "$v")

    if [ "${LOCALMOD:-0}" = 1 ]; then
        echo "-- LOCALMOD=1：按当前 lsmod 裁剪模块（编译更快）"
        # shellcheck disable=SC2046
        yes '' | kbuild "$O" $(ikaslr_pass_args "$v") LSMOD=/proc/modules localmodconfig >/dev/null 2>&1 || true
    fi
    # shellcheck disable=SC2046
    kbuild "$O" $(ikaslr_pass_args "$v") olddefconfig >/dev/null

    echo "-- $v 的关键配置："
    grep -E 'CONFIG_IKASLR' "$O/.config" || echo "   (IKASLR off)"
    grep -E 'CONFIG_(RETPOLINE|UNWINDER_ORC|UNWINDER_FRAME_POINTER|ARM64_BTI)=' "$O/.config" || true

    # 编译输出留一份：pass 会把"这个函数不能随机化"的原因打到 stderr，
    # 下面据此生成 $O/ikaslr-skipped.txt。
    # shellcheck disable=SC2046
    kbuild "$O" $(ikaslr_pass_args "$v") -j"$JOBS" "$IMG" modules 2>&1 | tee "$O/build.log"
    [ "${PIPESTATUS[0]}" = 0 ] || { echo "!! $v 编译失败，见 $O/build.log"; exit 1; }

    if [ "$v" != base ] && [ "$CRR_TRAMPOLINE" = y ]; then
        grep -oE 'ikaslr: skipping [A-Za-z0-9_]+: .*' "$O/build.log" \
            | sed 's/^ikaslr: skipping //' | sort -u > "$O/ikaslr-skipped.txt"
        if [ -s "$O/ikaslr-skipped.txt" ]; then
            echo "-- pass 跳过了 $(wc -l < "$O/ikaslr-skipped.txt") 个函数（名单里有、但改造不了）："
            sed 's/^/     /' "$O/ikaslr-skipped.txt"
        fi
    fi

    # 校验：被随机化的函数体必须真的可搬移（见 实现/16-position-independence.md §8）
    if [ "$v" != base ] && [ "$CRR_TRAMPOLINE" = y ]; then
        echo "-- 校验可搬移性"
        if ! python3 "$ROOT/scripts/ikaslr/verify_movable.py" "$O/vmlinux" \
               --arch "$ARCH" --exclude-list "$O/ikaslr-unmovable.txt"; then
            cat <<W

!! $v：仍有函数体含指向区域外的 PC 相对引用，**搬移后会崩**。
   不合格的函数名已写到 $O/ikaslr-unmovable.txt
   处理：从名单里剔除后重编（编译→扫描→剔除→重编，直到为 0）：

       grep -vxF -f $O/ikaslr-unmovable.txt $IKASLR_FUNCS > /tmp/funcs-clean.txt
       IKASLR_FUNCS=/tmp/funcs-clean.txt $0 $v
W
            exit 1
        fi
    fi
    echo "== $v 编译完成"
done

cat <<NEXT

全部完成。下一步： sudo ./03-install-kernels.sh ${WANT[*]}

提醒：非 base 档必须用**同一份** IKASLR_FUNCS。当前用的是
  $IKASLR_FUNCS  ($NFUNCS 个函数)
base 档用空名单 $IKASLR_FUNCS_BASE —— 插件照样加载，但一个函数都不改造。
NEXT
