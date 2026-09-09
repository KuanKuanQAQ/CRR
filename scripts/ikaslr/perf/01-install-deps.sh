#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 步骤 1：安装/下载全部基准与编译依赖（在正常内核下、联网时跑一次）。
#   sudo ./01-install-deps.sh
# 自动识别包管理器（apt / dnf / yum / zypper —— 覆盖 Debian/Ubuntu 与
# openEuler/RHEL/Fedora/openSUSE）。基准源码放 $BENCH_DIR（见 env.sh）。
# 幂等：已装的跳过；仓库里没有的（openEuler 上的 netperf/wrk）用源码兜底。
set -uo pipefail
. "$(dirname "$0")/env.sh"

# ---- 识别包管理器 ----
if   command -v apt-get >/dev/null; then PM=apt
elif command -v dnf     >/dev/null; then PM=dnf
elif command -v yum     >/dev/null; then PM=yum
elif command -v zypper  >/dev/null; then PM=zypper
else PM=none; fi
echo "== 包管理器: $PM   (架构 $ARCH)"

pm_install() {   # 尽力安装列表，个别缺失不致命
    case "$PM" in
        apt)    DEBIAN_FRONTEND=noninteractive apt-get install -y "$@" ;;
        dnf)    dnf install -y --skip-broken "$@" ;;
        yum)    yum install -y "$@" ;;
        zypper) zypper --non-interactive install -y "$@" ;;
        none)   echo "!! 无包管理器，请手动安装： $*"; return 1 ;;
    esac
}
pm_refresh() {
    case "$PM" in
        apt) apt-get update -y ;;
        dnf) dnf makecache -y || true ;;
        yum) yum makecache -y || true ;;
        zypper) zypper --non-interactive refresh || true ;;
    esac
}

# ---- 按发行版给出包名（同一件东西在两派里名字不同）----
if [ "$PM" = apt ]; then
    BUILD_PKGS=(build-essential bc bison flex libssl-dev libelf-dev ncurses-dev)
    BENCH_PKGS=(git wget curl fio iperf3 nginx rt-tests stress-ng sysbench
                p7zip-full openssl numactl linux-cpupower cpufrequtils)
    NETPERF_PKG=netperf; WRK_PKG=wrk
else
    # dnf/yum/zypper（openEuler 等）
    BUILD_PKGS=(make gcc bc bison flex elfutils-libelf-devel openssl-devel
                ncurses-devel perl dwarves)
    BENCH_PKGS=(git wget curl fio iperf3 nginx rt-tests stress-ng sysbench
                p7zip openssl numactl kernel-tools)
    NETPERF_PKG=netperf; WRK_PKG=wrk    # openEuler 仓库多半没有，下面走源码兜底
fi

echo "== 刷新软件源"; [ "$PM" != none ] && pm_refresh

echo "== 安装编译依赖"
if [ "$PM" = apt ]; then
    pm_install "${BUILD_PKGS[@]}"
else
    # rpm 系：Development Tools 组 + 明确的几个 devel 包
    case "$PM" in
        dnf) dnf groupinstall -y "Development Tools" 2>/dev/null || true ;;
        yum) yum groupinstall -y "Development Tools" 2>/dev/null || true ;;
        zypper) zypper --non-interactive install -y -t pattern devel_basis 2>/dev/null || true ;;
    esac
    pm_install "${BUILD_PKGS[@]}" || echo "!! 部分编译依赖缺失，见上"
fi

echo "== 安装基准工具"
pm_install "${BENCH_PKGS[@]}" || echo "!! 部分基准包缺失（openEuler 可尝试启用 EPOL 源），按需手动补"
pm_install "$NETPERF_PKG" 2>/dev/null || true
pm_install "$WRK_PKG"     2>/dev/null || true

mkdir -p "$BENCH_DIR"
GITOPT="--depth=1"

# ---- LMBench（微基准，源码编译）----
if ! ls "$BENCH_DIR"/lmbench/bin/*/lat_syscall >/dev/null 2>&1; then
    echo "== 源码编译 LMBench"
    rm -rf "$BENCH_DIR/lmbench"
    git clone $GITOPT https://github.com/intel/lmbench "$BENCH_DIR/lmbench" \
        && make -C "$BENCH_DIR/lmbench" build 2>&1 | tail -3 \
        || echo "!! lmbench 编译需手动看一眼"
fi

# ---- UnixBench（系统基准，源码编译）----
if [ ! -x "$BENCH_DIR/byte-unixbench/UnixBench/Run" ]; then
    echo "== 源码编译 UnixBench"
    git clone $GITOPT https://github.com/kdlucas/byte-unixbench "$BENCH_DIR/byte-unixbench" \
        && make -C "$BENCH_DIR/byte-unixbench/UnixBench" 2>&1 | tail -3
fi

# ---- netperf（rpm 系仓库常缺 → 源码兜底）----
if ! command -v netperf >/dev/null; then
    echo "== 源码编译 netperf（仓库未提供）"
    if [ ! -d "$BENCH_DIR/netperf" ]; then
        git clone $GITOPT https://github.com/HewlettPackard/netperf "$BENCH_DIR/netperf" || true
    fi
    if [ -d "$BENCH_DIR/netperf" ]; then
        ( cd "$BENCH_DIR/netperf" && ./autogen.sh 2>/dev/null; ./configure && make -j"$JOBS" \
          && make install ) 2>&1 | tail -3 || echo "!! netperf 源码编译失败，网络项可改用 iperf3"
    fi
fi

# ---- wrk（HTTP 压测，rpm 系常缺 → 源码兜底）----
if ! command -v wrk >/dev/null; then
    echo "== 源码编译 wrk（仓库未提供）"
    if [ ! -x "$BENCH_DIR/wrk/wrk" ]; then
        git clone $GITOPT https://github.com/wg/wrk "$BENCH_DIR/wrk" \
            && make -C "$BENCH_DIR/wrk" -j"$JOBS" 2>&1 | tail -3 || echo "!! wrk 编译失败，web 项可用 ab/nginx 自带"
    fi
    [ -x "$BENCH_DIR/wrk/wrk" ] && install -m0755 "$BENCH_DIR/wrk/wrk" /usr/local/bin/wrk 2>/dev/null || true
fi

# ---- SPEC CPU2006：授权软件，无法下载 ----
cat <<'TIP'

== SPEC CPU2006（需授权，脚本不自动下载）
   问实验室/图书馆拿会员授权与 ISO；装好后 `source shrc`，在 05-run-suite.sh 的
   SPEC 段填 runspec，或直接用已装的免费替代（7z / openssl / sysbench）。
TIP

echo
echo "== 自检（有则打勾）："
for t in gcc make fio iperf3 nginx cyclictest stress-ng sysbench 7z 7za openssl \
         netperf wrk cpupower numactl; do
    command -v "$t" >/dev/null && echo "  [x] $t" || echo "  [ ] $t   (缺，按需补)"
done
ls "$BENCH_DIR"/lmbench/bin/*/lat_syscall >/dev/null 2>&1 && echo "  [x] lmbench" || echo "  [ ] lmbench"
[ -x "$BENCH_DIR/byte-unixbench/UnixBench/Run" ] && echo "  [x] unixbench" || echo "  [ ] unixbench"
echo
echo "== 完成。基准位于 $BENCH_DIR"
