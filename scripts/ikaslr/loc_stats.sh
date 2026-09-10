#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# I-KASLR 工程量(LOC)统计（实验 E1 / 论文 §6.3.1，W-61）。
#
# 统计 I-KASLR 相对未改动基线内核新增/改动的代码量，按组件分列。
# 随代码演进可重复运行，答辩「你到底改了多少东西」用此产出。
#
# 用法: scripts/ikaslr/loc_stats.sh [基线 tag，默认 v6.8]
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
BASE="${1:-v6.8}"

if ! git rev-parse -q --verify "$BASE^{commit}" >/dev/null; then
    echo "loc_stats: 找不到基线 '$BASE'（用 git tag 查看；论文基线为 v6.8）" >&2
    exit 1
fi

# 新增的独立目录（整目录都是 I-KASLR 的）
NEW_DIRS=(kernel/ikaslr scripts/ikaslr tools/ikaslr)
count_dir() {  # 目录 -> 现有代码行数（仅统计已跟踪的源码类文件）
    local d="$1" n=0
    [ -d "$d" ] || { echo 0; return; }
    n=$(git ls-files "$d" | grep -E '\.(c|h|S|py|sh|rs|ld|lds)$' | \
        xargs -r wc -l 2>/dev/null | tail -1 | awk '{print $1}')
    echo "${n:-0}"
}

echo "# I-KASLR 工程量统计 (E1)"
echo "# 基线: $BASE ($(git rev-parse --short "$BASE"))   当前: $(git rev-parse --abbrev-ref HEAD) ($(git rev-parse --short HEAD))"
echo "# 日期: $(date +%F)"
echo

echo "== 一、对既有内核文件的改动（只算代码）=="
# 口径：**文档不算工程量**。本仓库里 Documentation/crr/ 有上万行设计与实验文档，
# 混进来会把"改了多少内核代码"报虚数倍。samples/ 与 boot/ 是开发期脚手架，同理。
# 三者各自单列在第六节，如实交代但不与内核改动相加。
EXCL=(); for d in "${NEW_DIRS[@]}"; do EXCL+=(":(exclude)$d"); done
NONCODE=(":(exclude)Documentation" ":(exclude)samples" ":(exclude)boot")
# CKASLR 是**更早的一个原型**（syscall 驱动的重随机化），代码仍在树里但
# CONFIG_CKASLR 在所有被评估的构建里都是 off。它不属于本工作被评估的实现，
# 计进来会把工程量报虚，因此排除并在第六节单列。
CKASLR=(":(exclude)kernel/rerand.c" ":(exclude)kernel/rerand_utils.c"
        ":(exclude)include/linux/rerand.h" ":(exclude)fs/read_write.c"
        ":(exclude)include/linux/syscalls.h" ":(exclude)include/linux/fs.h"
        ":(exclude)README.md")
git diff --stat "$BASE" -- . "${EXCL[@]}" "${NONCODE[@]}" "${CKASLR[@]}" | tail -40 || true
echo

echo "== 二、新增组件的代码行数 =="
printf "%-24s %10s\n" "组件(目录)" "行数"
printf "%-24s %10s\n" "------------------------" "----------"
total=0
for d in "${NEW_DIRS[@]}"; do
    n=$(count_dir "$d"); total=$((total+n))
    printf "%-24s %10s\n" "$d" "$n"
done
printf "%-24s %10s\n" "新增小计" "$total"
echo

# ---------------------------------------------------------------------------
# 以下三节对应 实验总清单.md §第4章 E1 的三项要求。
# ---------------------------------------------------------------------------

echo
echo "== 三、编译器侧：按功能分列 =="
# 按功能而不是按文件分列：清单要求区分「函数级 PIC 转换 / 跳板生成与控制流重定向 /
# 指针认证插桩 / 静态验证检查」。用各功能的入口函数在源码里的区间长度近似。
llvm_fn_loc() {   # $1=函数名 -> 该函数体的行数（含注释，注释是本工作的一部分）
    awk -v fn="$1" '
        $0 ~ "^[A-Za-z_].*[ *]" fn "\\(" || $0 ~ "^  [A-Za-z].*[ *]" fn "\\(" { d=0; inf=1 }
        inf { n++; d += gsub(/\{/,"{"); d -= gsub(/\}/,"}"); if (d==0 && n>1) { print n; exit } }
    ' tools/ikaslr/llvm/IKaslrPass.cpp
}
printf "%-40s %8s %s\n" "功能" "行数" "入口"
printf "%-40s %8s %s\n" "----------------------------------------" "--------" "----"
for pair in \
    "跳板生成与调用点重定向|transform" \
    "函数级位置无关改造|makeBodyPositionIndependent" \
    "绝对地址物化（含 arm64 字面量池）|materializeAbs" \
    "fixed_out 序列与白名单生成|wrapOutboundCalls" \
    "内建 mem* 降级|lowerMemIntrinsics" \
    "跳板表项与 target 槽发射|emitTableEntry" \
    "静态验证检查（-ikaslr-verify）|verify" \
    "不可改造函数的拒绝判定|rejectReason" ; do
    fn="${pair#*|}"; label="${pair%|*}"
    printf "%-40s %8s %s()\n" "$label" "$(llvm_fn_loc "$fn")" "$fn"
done
printf "%-40s %8s\n" "LLVM 插件合计" "$(wc -l < tools/ikaslr/llvm/IKaslrPass.cpp)"
printf "%-40s %8s\n" "GCC 插件合计" "$(wc -l < tools/ikaslr/gcc/ikaslr_gcc.c)"
echo
echo "  流水线插入位置（LLVM 新 Pass Manager）:"
grep -oE 'register(PipelineStart|OptimizerLast|PipelineParsing)EPCallback' \
     tools/ikaslr/llvm/IKaslrPass.cpp | sort -u | sed 's/^/    /'
echo "    · PipelineStart   : 给选中函数打 noinline（必须在内联器之前）"
echo "    · OptimizerLast   : 做改造（在常规优化之后，避免优化把跳板内联掉）"
echo "  与 LTO 的关系: 本方案用 -fpass-plugin 在每个翻译单元内改造，"
echo "    不依赖 LTO；若开 LTO，改造点会移到 LTO 阶段，跳板的 RAUW 语义不变。"

echo
echo "== 四、内核侧：按子系统分列 =="
printf "%-34s %8s %8s  %s\n" "子系统" "新增" "删除" "文件"
printf "%-34s %8s %8s  %s\n" "----------------------------------" "--------" "--------" "----"
kloc() {   # $1=标签  $2..=路径
    local label="$1"; shift
    local st add del
    st=$(git diff --numstat "$BASE" -- "$@" 2>/dev/null | \
         awk '{a+=$1; d+=$2} END{printf "%d %d", a, d}')
    add=${st% *}; del=${st#* }
    printf "%-34s %8s %8s  %s\n" "$label" "${add:-0}" "${del:-0}" "$*"
}
kloc "布局分配器与变体池"      kernel/ikaslr/randomize.c
kloc "跳板运行时与执行流追踪"  kernel/ikaslr/core.c
kloc "陷阱与陈旧返回地址修正"  kernel/ikaslr/fixup.c
kloc "跨区域白名单"            kernel/ikaslr/whitelist.c
kloc "检测模块（第4章）"        kernel/ikaslr/detect.c kernel/ikaslr/xom_ept.c
kloc "验证模块（第5章 PA CFI）" kernel/ikaslr/pacfi.c
kloc "控制接口与自测/微基准"    kernel/ikaslr/control.c kernel/ikaslr/selftest.c kernel/ikaslr/bench.c kernel/ikaslr/randfuncs.c
kloc "链接脚本与段布局"        include/asm-generic/vmlinux.lds.h arch/x86/kernel/vmlinux.lds.S arch/arm64/kernel/vmlinux.lds.S
kloc "地址键旁表改造"          kernel/extable.c lib/bug.c arch/x86/mm/extable.c arch/arm64/mm/extable.c arch/x86/kernel/jump_label.c arch/arm64/kernel/jump_label.c
kloc "任务状态（嵌套深度）"    include/linux/sched.h kernel/fork.c
kloc "对外接口头"              include/linux/ikaslr.h
kloc "构建系统与 modpost"      Makefile scripts/mod/modpost.c
kloc "Kconfig"                 kernel/ikaslr/Kconfig kernel/Makefile

echo
echo "== 五、随机化范围 =="
printf "%-26s %8s  %s\n" "名单" "函数数" "候选范围"
printf "%-26s %8s  %s\n" "--------------------------" "--------" "--------"
for f in scripts/ikaslr/funcs/*.txt; do
    n=$(grep -vc '^#' "$f" 2>/dev/null) || n=0
    scope=$(grep -m1 '候选范围' "$f" 2>/dev/null | sed 's/^# *候选范围: *//')
    printf "%-26s %8s  %s\n" "$(basename "$f")" "$n" "${scope:-—}"
done

echo
echo "== 六、不计入工程量的部分（如实交代）=="
printf "%-34s %8s  %s\n" "类别" "行数" "说明"
printf "%-34s %8s  %s\n" "----------------------------------" "--------" "----"
for pair in "设计与实验文档|Documentation/crr" \
            "开发期样例|samples" \
            "启动镜像脚本|boot" \
            "CKASLR 早期原型（CONFIG 未开启）|kernel/rerand.c kernel/rerand_utils.c include/linux/rerand.h fs/read_write.c" ; do
    d="${pair#*|}"; label="${pair%|*}"
    # shellcheck disable=SC2086
    n=$(git diff --numstat "$BASE" -- $d 2>/dev/null | awk '{a+=$1} END{print a+0}')
    printf "%-34s %8s  %s\n" "$label" "$n" "$d"
done
echo "  文档是本工作的产出之一，但不是「改了多少内核代码」的答案，故单列。"
echo "  CKASLR 是更早的一个原型（syscall 驱动的重随机化），代码仍在树里但所有"
echo "  被评估的构建都没有开启它的 CONFIG，因此不计入本工作的工程量。"
