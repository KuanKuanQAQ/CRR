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

# Rg 档：与 R 完全相同，只多开 CONFIG_IKASLR_STATS。**不参与开销对比**，
# 只用来测每个负载的跨区域调用频度 G（enters/outs 计数需要 STATS）。
# 统计本身实测只值 1.0~1.6 ns/次（见 实验/E3-A），因此 Rg 的吞吐与 R 相差约 2%，
# 用它测 G 是足够的。用法： ./02-build-kernels.sh Rg && ... && ./05-run-suite.sh Rg

# 编译并行度
JOBS="${JOBS:-$(nproc)}"

# 编译器 pass：**必须开**，否则没有任何真实内核函数被随机化，四档等价、测不出差异。
#   CRR_TRAMPOLINE=y + IKASLR_FUNCS=名单   （真实测量）
#   CRR_TRAMPOLINE=n                        （仅功能占位，四档等价，勿用于性能数据）
CRR_TRAMPOLINE="${CRR_TRAMPOLINE:-y}"
# 默认用 S3（483 个函数，fs/+block/+net/core/+mm/）——第 6 章的默认范围。
# 规模与选取理由见 ../../../Documentation/crr/实现/15-scope-selection.md
IKASLR_FUNCS="${IKASLR_FUNCS:-$ROOT/scripts/ikaslr/funcs/s3-full.txt}"

# **Base 档用空名单，而不是关掉插件。**
# 清单 §0.1 的纪律是"四档之间只允许差 IKASLR 相关 CONFIG，编译器全部一致"。
# 若 Base 干脆不加 -fpass-plugin，两档的编译流水线就不同，测出来的差额里会混着
# "加载插件本身"的影响。空名单则插件照样加载、一个函数都不改造。
IKASLR_FUNCS_BASE="${IKASLR_FUNCS_BASE:-$ROOT/scripts/ikaslr/funcs/empty.txt}"

# 某一档该用哪份名单
variant_funcs() { [ "$1" = base ] && echo "$IKASLR_FUNCS_BASE" || echo "$IKASLR_FUNCS"; }

# **编译器必须是 clang**：-fpass-plugin 是 clang 特性。本机若无 ld.lld，
# 用 CC=clang + GNU binutils 即可（LLVM=1 会因缺 ld.lld 失败）。
IKASLR_CC="${IKASLR_CC:-clang}"

# +R/+RD/+RDP 的随机化触发间隔（ms）；与 Adelie 对齐取 20。
TRIGGER_MS="${TRIGGER_MS:-20}"

# 网络对端（netperf/wrk 的服务器 IP）；留空则跳过网络项。
SERVER_IP="${SERVER_IP:-}"

# ---- 所有档都必须相同的强制配置 ----
#
# 这些**不是**可选项，是"代码可搬移"的前提（见 实现/16-position-independence.md）。
# 四档必须**一律相同**地设置，否则测的是这些设施的开销而不是随机化开销。
#
#   RETPOLINE   x86：间接调用会被编成到固定 thunk 的 PC 相对调用，一搬就错
#   ARM64_BTI   arm64：本方案把直接调用改成间接调用，而 BTI 要求落点有 BTI 指令
#   UNWINDER    ORC 表以代码地址为键，函数搬走后回溯失效 → 改用帧指针 unwinder
ikaslr_mandatory_config() {
    if [ "$ARCH" = x86_64 ]; then
        echo "--disable RETPOLINE --disable RETHUNK --disable CPU_UNRET_ENTRY \
              --disable CALL_DEPTH_TRACKING \
              --disable UNWINDER_ORC --enable UNWINDER_FRAME_POINTER"
    else
        echo "--disable ARM64_BTI --disable ARM64_BTI_KERNEL"
    fi
}

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
        # Rg：与 R 相同，只多开 STATS。仅用于测 G，见上面 VARIANTS 处的说明。
        Rg)   echo "--enable IKASLR --enable IKASLR_STATS --disable IKASLR_DEBUG" ;;
        *)    echo "unknown variant: $1" >&2; return 1 ;;
    esac
}

# 该档是否是 x86 单核 XOM（对比时需 nr_cpus=1）
variant_is_single_core() { [ "$1" = RD ] && [ "$ARCH" = x86_64 ]; }

# 该档是否需要后台定频触发随机化
variant_needs_trigger() { [ "$1" != base ]; }

# 每档跑几遍（清单 §0.4：每项 >=5 次，报中位数 + 四分位）
REPEAT="${REPEAT:-5}"

kbuild() { make -C "$ROOT" O="$1" ARCH="$ARCH" CC="$IKASLR_CC" $CROSS "${@:2}"; }

# 传给每次 make 的 pass 参数（主 Makefile 的插件检查对任意目标都会触发）
ikaslr_pass_args() {   # $1=档名（可省，省则用全局 IKASLR_FUNCS）
    if [ "$CRR_TRAMPOLINE" = y ]; then
        echo "CRR_TRAMPOLINE=y IKASLR_FUNCS=$(variant_funcs "${1:-R}")"
    else
        echo "CRR_TRAMPOLINE=n"
    fi
}

# 从 uname -r 解析当前档名，剥掉 git 描述后缀（-g<sha> / 尾部 '+'）。
current_variant() {
    local rel="${1:-$(uname -r)}" v
    case "$rel" in
        *-ikaslr-*) v="${rel##*-ikaslr-}"; v="${v%%-*}"; v="${v%+}"; echo "$v" ;;
        *)          echo base ;;
    esac
}
