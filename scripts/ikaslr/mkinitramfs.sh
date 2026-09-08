#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 构建一个最小 initramfs，用于在 QEMU 中以用户态驱动 I-KASLR 的实验。
#
# 不依赖 busybox 或宿主机 rootfs：init 是一个静态链接的小程序，把要执行的
# 命令写死在里面（读 /proc/ikaslr/*、触发随机化等），跑完即关机。
#
# 用法: scripts/ikaslr/mkinitramfs.sh <init.c> <输出.cpio.gz>
set -euo pipefail
SRC="${1:?usage: mkinitramfs.sh <init.c> <out.cpio.gz>}"
OUT="${2:?usage: mkinitramfs.sh <init.c> <out.cpio.gz>}"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/root/proc" "$TMP/root/sys" "$TMP/root/dev"

gcc -static -O2 -o "$TMP/root/init" "$SRC"
( cd "$TMP/root" && find . -print0 | cpio --null -o --format=newc 2>/dev/null ) \
    | gzip -9 > "$OUT"
echo "initramfs -> $OUT ($(stat -c%s "$OUT") bytes)"
