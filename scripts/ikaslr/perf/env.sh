#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# I-KASLR 性能测试的公共配置：所有编号脚本都 `source` 本文件。
# 需要覆盖时从环境传入，例如：  ARCH=arm64 ./02-build-kernels.sh
#
# 部署模型：同一台真机上，为每档配置编译并安装一个内核，靠"改下次启动项 + 重启"
# 做 A/B（见 ../../../Documentation/crr/实现/10-perf-plan.md）。

# ---- 路径 ----
ROOT="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
PERF_DIR="$ROOT/scripts/ikaslr/perf"
RESULTS_DIR="${RESULTS_DIR:-$PERF_DIR/results}"   # 结果就放在本目录下
BENCH_DIR="${BENCH_DIR:-$HOME/ikaslr-bench}"      # 下载/编译的基准放这里
BUILD_ROOT="${BUILD_ROOT:-$ROOT/../ikaslr-builds}" # 各档内核的 O= 输出根目录

# ---- 目标架构 ----
ARCH="${ARCH:-$(uname -m)}"
case "$ARCH" in
    x86_64|amd64) ARCH=x86_64 ;;
    aarch64|arm64) ARCH=arm64 ;;
esac
CROSS=""
[ "$ARCH" = arm64 ] && [ "$(uname -m)" != aarch64 ] && CROSS="CROSS_COMPILE=aarch64-linux-gnu-"

# ---- 配置矩阵（自变量：启用的机制组合）----
# 每档一个名字；per-arch 列表。x86 的 +RD=XOM_EPT（单核）；arm 的 +RDP=PACFI。
if [ "$ARCH" = x86_64 ]; then
    VARIANTS=(base R RD)
else
    VARIANTS=(base R RD RDP)
fi

# 编译并行度
JOBS="${JOBS:-$(nproc)}"

# 编译器 pass（§0）：铺到真实子系统才测得出真实开销。
#   CRR_TRAMPOLINE=n   仅内建的 3 个示例函数被随机化（功能验证/占位，开销偏小）
#   CRR_TRAMPOLINE=y   启用 LLVM 插件按 IKASLR_FUNCS 随机化指定函数（真实测量）
CRR_TRAMPOLINE="${CRR_TRAMPOLINE:-n}"
IKASLR_FUNCS="${IKASLR_FUNCS:-$PERF_DIR/funcs.txt}"

# +R/+RD/+RDP 的随机化触发间隔（ms）；与 Adelie 对齐取 20。
TRIGGER_MS="${TRIGGER_MS:-20}"

# 网络对端（netperf/wrk 的服务器 IP）；留空则跳过网络项。
SERVER_IP="${SERVER_IP:-}"

# ---- 把一个档名映射到 scripts/config 参数 ----
# 用法: variant_config_args <name>  → 打印 scripts/config 参数
variant_config_args() {
    case "$1" in
        base) echo "--disable IKASLR" ;;
        R)    echo "--enable IKASLR --disable IKASLR_STATS --disable IKASLR_DEBUG" ;;
        RD)   if [ "$ARCH" = x86_64 ]; then
                  echo "--enable IKASLR --disable IKASLR_STATS --enable IKASLR_XOM_EPT"
              else
                  echo "--enable IKASLR --disable IKASLR_STATS"   # arm 的检测走观察点
              fi ;;
        RDP)  echo "--enable IKASLR --disable IKASLR_STATS --enable IKASLR_PACFI" ;;
        *)    echo "unknown variant: $1" >&2; return 1 ;;
    esac
}

# 该档是否是 x86 单核 XOM（对比时需 nr_cpus=1）
variant_is_single_core() { [ "$1" = RD ] && [ "$ARCH" = x86_64 ]; }

# 该档是否需要后台定频触发随机化
variant_needs_trigger() { [ "$1" != base ]; }

kbuild() { make -C "$ROOT" O="$1" ARCH="$ARCH" $CROSS "${@:2}"; }

# 从 uname -r 解析当前档名，剥掉 git 描述后缀（-g<sha> / 尾部 '+'）。
current_variant() {
    local rel="${1:-$(uname -r)}" v
    case "$rel" in
        *-ikaslr-*) v="${rel##*-ikaslr-}"; v="${v%%-*}"; v="${v%+}"; echo "$v" ;;
        *)          echo base ;;
    esac
}
