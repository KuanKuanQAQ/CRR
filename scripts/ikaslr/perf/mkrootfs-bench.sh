#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 在现有 Debian rootfs 镜像里安装所有免费基准工具（chroot）。
#   mkrootfs-bench.sh <rootfs.img>
# SPEC CPU2006/Phoronix 需另装（体积大），见 10-perf-plan.md。
set -euo pipefail
IMG="${1:?usage: mkrootfs-bench.sh <rootfs.img>}"
MNT=$(mktemp -d)
sudo mount -o loop "$IMG" "$MNT"
trap 'sudo umount "$MNT"; rmdir "$MNT"' EXIT
sudo cp /etc/resolv.conf "$MNT/etc/" || true
sudo chroot "$MNT" /bin/bash -c '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y --no-install-recommends \
    fio netperf iperf3 wrk apache2-utils nginx rt-tests \
    stress-ng sysbench p7zip-full openssl lmbench build-essential \
    linux-perf 2>/dev/null || \
  apt-get install -y --no-install-recommends \
    fio netperf iperf3 apache2-utils nginx rt-tests \
    stress-ng sysbench p7zip-full openssl build-essential
'
echo "benchmarks installed into $IMG"
echo "注意：netperf 交互协议问题可能需手动确认许可；SPEC/Phoronix 另装。"
