#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 步骤 4：把"下次启动"设成某档内核，然后重启进入它（需 root）。
#   sudo ./04-boot-into.sh RD           # 下次启动进 +RD，之后自动回默认档
#   sudo ./04-boot-into.sh RD --now     # 立即重启
# 用 grub-reboot（一次性），不改默认项，重启后恢复默认，方便脚本化逐档轮测。
set -euo pipefail
. "$(dirname "$0")/env.sh"
[ "$(id -u)" = 0 ] || { echo "需要 root： sudo $0 $*"; exit 1; }
V="${1:?usage: 04-boot-into.sh <variant> [--now]}"; NOW="${2:-}"

REL="$(cat "$BUILD_ROOT/$V/include/config/kernel.release" 2>/dev/null)" \
    || { echo "!! $V 未安装（缺 kernel.release），先跑 02/03"; exit 1; }

GCFG=/boot/grub/grub.cfg; [ -r "$GCFG" ] || GCFG=/boot/grub2/grub.cfg
[ -r "$GCFG" ] || { echo "!! 找不到 grub.cfg"; exit 1; }

# 找到含该 release 的 menuentry；若在 submenu 里，拼成 "子菜单>菜单项"。
ENTRY="$(awk -F\' -v rel="$REL" '
    /^menuentry / { top=$2 }
    /^submenu /   { sub=$2 }
    /menuentry / && index($0, rel) {
        if ($0 ~ /^[[:space:]]+menuentry/ && sub!="") print sub ">" $2;
        else print $2;
        exit
    }' "$GCFG")"
[ -n "$ENTRY" ] || { echo "!! grub.cfg 里找不到 $REL 的菜单项，手动确认"; exit 1; }

echo "下次启动 -> [$V] $ENTRY"
grub-reboot "$ENTRY" 2>/dev/null || grub2-reboot "$ENTRY"

if variant_is_single_core "$V"; then
    cat <<'W'
!! 注意：x86 +RD(XOM) 是单核机制。做 XOM 开销对比时，参与对比的所有档都应以
   nr_cpus=1 启动（改 /etc/default/grub 的 GRUB_CMDLINE_LINUX_DEFAULT 加 nr_cpus=1，
   update-grub 后再逐档测），单核对单核才可比。详见 README / 10-perf-plan.md §2。
W
fi

if [ "$NOW" = --now ]; then echo "重启中…"; sleep 2; reboot
else echo "已设定。手动重启： sudo reboot"; fi
