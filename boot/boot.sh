#!/usr/bin/env bash
# Boot the freshly built kernel under QEMU.
#
#     ./boot.sh                 boot arm64 (default)
#     ARCH=x86_64 ./boot.sh     boot x86_64
#
# QEMU listens for gdb on :1234 (-s).  See config.sh for everything tunable.
set -eu
cd "$(dirname "$0")"
. ./config.sh

exec sudo "$QEMU" -s "${QEMU_ARGS[@]}"
