#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 步骤 3：把编好的各档内核装进 /boot 并加入 GRUB 启动菜单（需 root）。
#   sudo ./03-install-kernels.sh            # 装全部档
#   sudo ./03-install-kernels.sh base R
# 每档因 LOCALVERSION 不同而成为独立的 `uname -r` 与独立 GRUB 项。
set -euo pipefail
. "$(dirname "$0")/env.sh"
[ "$(id -u)" = 0 ] || { echo "需要 root： sudo $0 $*"; exit 1; }

WANT=("$@"); [ ${#WANT[@]} -eq 0 ] && WANT=("${VARIANTS[@]}")

for v in "${WANT[@]}"; do
    O="$BUILD_ROOT/$v"
    [ -r "$O/.config" ] || { echo "!! $v 未编译（缺 $O），先跑 02-build-kernels.sh"; exit 1; }
    echo "=================== 安装 $v ==================="
    # CRR_TRAMPOLINE 透传：主 Makefile 的插件检查对 modules_install/install 同样触发。
    # shellcheck disable=SC2046
    kbuild "$O" $(ikaslr_pass_args) -j"$JOBS" modules_install  # /lib/modules/<rel>
    # shellcheck disable=SC2046
    kbuild "$O" $(ikaslr_pass_args) install                    # /boot + initramfs + grub 钩子
    REL="$(cat "$O/include/config/kernel.release")"
    echo "== $v 已装： $REL"
done

echo "== 刷新 GRUB 菜单"
if command -v update-grub >/dev/null; then update-grub
elif command -v grub2-mkconfig >/dev/null; then grub2-mkconfig -o /boot/grub2/grub.cfg
else echo "!! 未找到 update-grub/grub2-mkconfig，请手动更新 grub 配置"; fi

echo
echo "== 各档的 uname -r 与对应 GRUB 菜单项（供 04-boot-into.sh 用）："
for v in "${WANT[@]}"; do
    REL="$(cat "$BUILD_ROOT/$v/include/config/kernel.release" 2>/dev/null || echo '?')"
    ENT="$(awk -F\' "/menuentry / && /$REL/ {print \$2; exit}" /boot/grub/grub.cfg 2>/dev/null \
           || awk -F\' "/menuentry / && /$REL/ {print \$2; exit}" /boot/grub2/grub.cfg 2>/dev/null)"
    printf "   %-4s  rel=%s\n         entry=%s\n" "$v" "$REL" "${ENT:-<未在 grub.cfg 找到，手动确认>}"
done
echo
echo "下一步（每档一轮）： sudo ./04-boot-into.sh <variant> && 重启后 ./05-run-suite.sh"
