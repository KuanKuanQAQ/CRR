#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 生成四档内核配置（Base/+R/+RD/+RDP）到各自 build 目录。
#   gen-configs.sh <arch: x86_64|arm64> <src-defconfig-build-dir>
# 之后各档按先决条件（编译器 pass + IKASLR_FUNCS）编译。
set -eu
ARCH="${1:?usage: gen-configs.sh <x86_64|arm64> <base-build-dir>}"
BASE="${2:?}"
ROOT="$(git rev-parse --show-toplevel)"
CROSS=""; [ "$ARCH" = arm64 ] && CROSS="CROSS_COMPILE=aarch64-linux-gnu-"

mkcfg() {   # <build-dir> <extra scripts/config args...>
    local d="$1"; shift
    mkdir -p "$d"
    make -C "$ROOT" O="$d" ARCH="$ARCH" $CROSS ${ARCH/x86_64/x86_64_}defconfig >/dev/null 2>&1 \
        || make -C "$ROOT" O="$d" ARCH="$ARCH" $CROSS defconfig >/dev/null 2>&1
    "$ROOT/scripts/config" --file "$d/.config" "$@"
    make -C "$ROOT" O="$d" ARCH="$ARCH" $CROSS olddefconfig >/dev/null 2>&1
    echo "configured $d:"; grep -E 'CONFIG_IKASLR' "$d/.config" || echo "  (IKASLR off)"
}

mkcfg "$BASE-base" --disable IKASLR
mkcfg "$BASE-R"    --enable IKASLR --enable IKASLR_STATS
if [ "$ARCH" = x86_64 ]; then
    mkcfg "$BASE-RD"  --enable IKASLR --enable IKASLR_STATS --enable IKASLR_XOM_EPT
else
    mkcfg "$BASE-RD"  --enable IKASLR --enable IKASLR_STATS
    mkcfg "$BASE-RDP" --enable IKASLR --enable IKASLR_STATS --enable IKASLR_PACFI
fi
echo "done. 各档还须按 10-perf-plan.md §0 用编译器 pass + IKASLR_FUNCS 编译。"
