#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 在 QEMU TCG 上启动 arm64 IKASLR 内核做功能冒烟（无 KVM，性能不代表真机）。
#   用法: boot-arm64-smoke.sh <arm64-build-dir> [<initramfs.cpio.gz>]
# 需要 aarch64-linux-gnu- 工具链、qemu-system-aarch64。
set -euo pipefail
BA="${1:?usage: boot-arm64-smoke.sh <arm64-build-dir> [initramfs]}"
IRFS="${2:-}"
if [ -z "$IRFS" ]; then
    T=$(mktemp -d)
    aarch64-linux-gnu-gcc -static -O2 -o "$T/init" tools/ikaslr/init_smoke.c
    IRFS="$T/initramfs.cpio.gz"
    ( cd "$T" && mkdir -p proc sys dev &&
      find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -9 > initramfs.cpio.gz )
fi
# -cpu max 提供 FEAT_PAuth 等（第5章 PA CFI 需要）；单核（EPT/单核策略，见 04a）。
exec qemu-system-aarch64 -M virt -cpu max -smp 1 -m 2G \
    -kernel "$BA/arch/arm64/boot/Image" -initrd "$IRFS" -nographic \
    -append "console=ttyAMA0 nokaslr"
