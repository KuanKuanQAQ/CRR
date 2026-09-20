#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# I-KASLR 跳转桩区生成（论文 §3.3.2 的 stub，设计对齐改造 M2）。
#
# LLVM pass 为每个被随机化函数 F（名单下标 k）发一个别名 __ikaslr_body_<k>
# 指向其函数体，并在入口跳板里 `bl __ikaslr_stub_<k>`。桩必须恰好位于
# __ikaslr_stubs + 8k，链接器无法按下标摆放分散的节，故在链接前用 nm 查出
# 哪些 k 存在，生成一份把桩按 k 顺序稠密排布的汇编（每桩 8 字节：入口桩 +
# 重入桩），缺失的 k 填 brk 占位以保持 8k 布局。
#
# 每个桩：
#   __ikaslr_stub_<k>:
#       b  __ikaslr_body_<k>          @ 入口桩：fixed_in 的 bl 落这里 -> 函数体入口
#       b  __ikaslr_body_<k> - 12     @ 重入桩：resolve_token 落这里 -> 函数体 prefix
# 随机化时改写这两条 b 的立即数即完成一个函数的索引更新（O(1)，取代 target 槽）。
#
# 用法: gen_stubs.sh <nm> <syms-source> <out.S>
#   <nm>          交叉工具链的 nm（$NM）
#   <syms-source> 含 __ikaslr_body_<k> 定义的对象/归档（vmlinux.o 或 vmlinux.a）
#   <out.S>       输出汇编
set -e

NM="$1"
SRC="$2"
OUT="$3"

# 收集所有 k（__ikaslr_body_<k> 的下标），求最大值。
ks=$("$NM" "$SRC" 2>/dev/null | sed -n 's/.*__ikaslr_body_\([0-9][0-9]*\)$/\1/p' | sort -n -u)

{
	echo '/* 自动生成，勿改。见 scripts/ikaslr/gen_stubs.sh */'
	echo '	.section .ikaslr.stubs,"ax",%progbits'
	# 2 的幂对齐：观察点 MASK 要求（§4.2/M6）。桩区起点先按页对齐，
	# 严格 2^n（覆盖整块）在 M6 随观察点一并处理。
	echo '	.balign 4096'
	echo '	.globl __ikaslr_stubs'
	echo '__ikaslr_stubs:'

	if [ -z "$ks" ]; then
		echo '	/* 本次构建无被随机化函数 */'
	else
		max=$(echo "$ks" | tail -n1)
		# 存在的 k 放进一个可 grep 的集合
		exist=" $(echo "$ks" | tr '\n' ' ') "
		k=0
		while [ "$k" -le "$max" ]; do
			case "$exist" in
			*" $k "*)
				echo "	.globl __ikaslr_stub_$k"
				echo "__ikaslr_stub_$k:"
				echo "	b __ikaslr_body_$k"
				echo "	b __ikaslr_body_$k - 12"
				;;
			*)
				# 缺失 k：8 字节 brk 占位，保持 base+8k 布局
				echo "	brk #0x100"
				echo "	brk #0x100"
				;;
			esac
			k=$((k + 1))
		done
	fi
	echo '	.globl __ikaslr_stubs_end'
	echo '__ikaslr_stubs_end:'
} > "$OUT"
