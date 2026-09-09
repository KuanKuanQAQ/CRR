#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 步骤 1：安装/下载全部基准与编译依赖（在正常内核下、联网时跑一次）。
#   sudo ./01-install-deps.sh
# 幂等：已装的跳过。基准源码放 $BENCH_DIR（见 env.sh）。
set -euo pipefail
. "$(dirname "$0")/env.sh"

APT_PKGS=(
    build-essential bc bison flex libssl-dev libelf-dev  # 编内核
    git wget curl                                        # 拉源码
    fio netperf iperf3 wrk nginx                         # 存储/网络
    rt-tests stress-ng sysbench p7zip-full openssl       # 延迟/计算
    cpufrequtils linux-cpupower numactl                  # 降噪/绑核
)
[ "$ARCH" = arm64 ] && [ "$(uname -m)" != aarch64 ] && APT_PKGS+=(gcc-aarch64-linux-gnu)

if command -v apt-get >/dev/null; then
    echo "== apt install"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -y
    apt-get install -y "${APT_PKGS[@]}" || echo "!! 部分包安装失败，按需手动补"
else
    echo "!! 非 apt 系统，请自行安装： ${APT_PKGS[*]}"
fi

mkdir -p "$BENCH_DIR"

# LMBench（微基准，源码编译）
if [ ! -x "$BENCH_DIR/lmbench/bin"/*/lat_syscall 2>/dev/null ]; then
    echo "== LMBench"
    rm -rf "$BENCH_DIR/lmbench"
    git clone --depth1 https://github.com/intel/lmbench "$BENCH_DIR/lmbench" \
        || git clone --depth1 https://github.com/intel/lmbench.git "$BENCH_DIR/lmbench"
    make -C "$BENCH_DIR/lmbench" build 2>&1 | tail -3 || echo "!! lmbench build 需手动看一眼"
fi

# UnixBench（系统基准）
if [ ! -x "$BENCH_DIR/byte-unixbench/UnixBench/Run" ]; then
    echo "== UnixBench"
    git clone --depth1 https://github.com/kdlucas/byte-unixbench "$BENCH_DIR/byte-unixbench"
    make -C "$BENCH_DIR/byte-unixbench/UnixBench" 2>&1 | tail -3
fi

# SPEC CPU2006：授权软件，无法下载。给出提示。
cat <<'TIP'

== SPEC CPU2006（需授权，脚本不自动下载）
   问实验室/图书馆拿会员授权与 ISO；装好后 `source shrc`，把 runspec 路径写进
   05-run-suite.sh 的 SPEC 段，或用已装的免费替代（7z / openssl / sysbench）。
TIP

echo
echo "== 完成。基准位于 $BENCH_DIR"
echo "   lmbench : $BENCH_DIR/lmbench"
echo "   unixbench: $BENCH_DIR/byte-unixbench/UnixBench"
