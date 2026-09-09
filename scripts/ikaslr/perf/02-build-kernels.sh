#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 步骤 2：为每档配置编译一个内核。
#   ./02-build-kernels.sh              # 编全部档
#   ./02-build-kernels.sh base R       # 只编指定档
#
# 关键纪律：各档从**同一份 defconfig** 出发，只 toggle IKASLR 相关 CONFIG，
# 其它一切（编译器、CONFIG_HZ、mitigations、base-KASLR）保持一致 —— 否则测的
# 是 config 噪声而非随机化开销。输出到 $BUILD_ROOT/<variant>/。
set -euo pipefail
. "$(dirname "$0")/env.sh"

WANT=("$@"); [ ${#WANT[@]} -eq 0 ] && WANT=("${VARIANTS[@]}")

# 编译器 pass 透传
PASS_ARGS=(CRR_TRAMPOLINE="$CRR_TRAMPOLINE")
if [ "$CRR_TRAMPOLINE" = y ]; then
    [ -r "$IKASLR_FUNCS" ] || { echo "!! 需要 IKASLR_FUNCS=$IKASLR_FUNCS（编译器 pass 名单）"; exit 1; }
    export IKASLR_FUNCS
    echo ">> 编译器 pass 已开：按 $IKASLR_FUNCS 随机化指定函数"
else
    echo ">> 编译器 pass 未开（CRR_TRAMPOLINE=n）：仅内建示例函数被随机化，"
    echo "   开销偏小、仅供流程验证。真实测量请设 CRR_TRAMPOLINE=y 并填好 funcs.txt。"
fi

defconfig_target() { [ "$ARCH" = x86_64 ] && echo x86_64_defconfig || echo defconfig; }

for v in "${WANT[@]}"; do
    O="$BUILD_ROOT/$v"
    echo "=================== 编 $v -> $O ==================="
    mkdir -p "$O"
    # PASS_ARGS 要带进**每一次** make：主 Makefile 的 LLVM 插件检查对任意
    # make 目标都会触发（含 defconfig/olddefconfig），CRR_TRAMPOLINE=n 才放行。
    kbuild "$O" "${PASS_ARGS[@]}" "$(defconfig_target)" >/dev/null
    # 只动 IKASLR 相关开关；其余保持 defconfig 一致
    # LOCALVERSION 让每档有独立的 `uname -r`，从而 /boot 与 GRUB 各成一项。
    # shellcheck disable=SC2046
    "$ROOT/scripts/config" --file "$O/.config" \
        --set-str LOCALVERSION "-ikaslr-$v" \
        $(variant_config_args "$v")
    # 可选快速档：只保留当前 lsmod 里已加载的模块，编译模块数从几千降到几十，
    # 大机器也能快很多。代价：只在**本机**语义正确（依赖当前 lsmod）；四档都从
    # 同一 lsmod 出发做 localmodconfig，故彼此仍一致。跨机搬内核别用它。
    if [ "${LOCALMOD:-0}" = 1 ]; then
        echo "-- LOCALMOD=1：按当前 lsmod 裁剪模块（编译更快）"
        yes '' | kbuild "$O" "${PASS_ARGS[@]}" LSMOD=/proc/modules localmodconfig >/dev/null 2>&1 || true
    fi
    kbuild "$O" "${PASS_ARGS[@]}" olddefconfig >/dev/null
    echo "-- $v 的 IKASLR 相关配置："
    grep -E 'CONFIG_IKASLR' "$O/.config" || echo "   (IKASLR off)"

    if [ "$ARCH" = x86_64 ]; then IMG=bzImage; else IMG=Image; fi
    kbuild "$O" "${PASS_ARGS[@]}" -j"$JOBS" "$IMG" modules
    echo "== $v 编译完成：$O/$( [ "$ARCH" = x86_64 ] && echo arch/x86/boot/bzImage || echo arch/arm64/boot/Image )"
done
echo
echo "全部完成。下一步： sudo ./03-install-kernels.sh ${WANT[*]}"
