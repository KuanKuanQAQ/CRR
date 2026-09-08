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

echo "== 一、对既有内核文件的改动（git diff --stat，排除新增目录）=="
EXCL=(); for d in "${NEW_DIRS[@]}"; do EXCL+=(":(exclude)$d"); done
git diff --stat "$BASE" -- . "${EXCL[@]}" | tail -40 || true
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

echo "== 三、按论文 §6.3.1 要求的分列（随实现补全）=="
cat <<'NOTE'
需要在正文给出、可由本脚本按子目录细化的分列：
  编译期 (tools/ikaslr/llvm/): 函数级PIC / 跳板生成与重定向 / PA插桩 / 静态验证
  内核侧 (kernel/ikaslr/):     布局分配器 / 跳板运行时 / 执行流追踪 /
                               检测模块 / 验证模块 / 伙伴系统改动
统计方法：git diff --stat 对照基线，排除纯格式改动。
NOTE
