#!/usr/bin/env bash
# Same as boot.sh, but stopped at reset (-S) waiting for a debugger:
#
#     ./boot-S.sh
#     gdb build/vmlinux -ex 'target remote :1234'
set -eu
cd "$(dirname "$0")"
. ./config.sh

exec sudo "$QEMU" -s -S "${QEMU_ARGS[@]}"
