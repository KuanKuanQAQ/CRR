#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# I-KASLR 跳转桩区生成（论文 §3.3.2 的 stub，设计对齐改造 M2）。
#
# LLVM pass 为每个被随机化函数 F（名单下标 k）发一个别名 __ikaslr_body_<k>
# 指向其函数体，并在入口跳板里调用 __ikaslr_stub_<k>。桩必须恰好位于
# __ikaslr_stubs + STRIDE*k，链接器无法按下标摆放分散的节，故在链接前用 nm 查出
# 哪些 k 存在，生成一份把桩按 k 顺序稠密排布的汇编，缺失的 k 填陷阱占位。
#
# 每个桩（一对：入口桩 + 重入桩）：
#   arm64（STRIDE=8，PREFIX=12）：
#       b  __ikaslr_body_<k>          @ 入口桩：fixed_in 的 bl 落这里 -> 函数体入口
#       b  __ikaslr_body_<k> - 12     @ 重入桩：resolve_token 落这里 -> 函数体 prefix
#   x86-64（STRIDE=16，PREFIX=13）：
#       jmp __ikaslr_body_<k>         @ 入口桩（+0），5 字节 + 3 字节 int3 补齐到 +8
#       jmp __ikaslr_body_<k> - 13    @ 重入桩（+8），5 字节 + 3 字节 int3 补齐到 +16
# 随机化时改写这两条分支的立即数即完成一个函数的索引更新（O(1)，取代 target 槽）。
#
# 用法: gen_stubs.sh <nm> <syms-source> <out.S> <arch>
set -e

NM="$1"
SRC="$2"
OUT="$3"
ARCH="$4"

ks=$("$NM" "$SRC" 2>/dev/null | sed -n 's/.*__ikaslr_body_\([0-9][0-9]*\)$/\1/p' | sort -n -u)

emit_stub() {	# $1 = k
	if [ "$ARCH" = arm64 ]; then
		echo "	b __ikaslr_body_$1"
		echo "	b __ikaslr_body_$1 - 12"
	else	# x86
		echo "	jmp __ikaslr_body_$1"
		echo "	.byte 0xcc, 0xcc, 0xcc"
		echo "	jmp __ikaslr_body_$1 - 13"
		echo "	.byte 0xcc, 0xcc, 0xcc"
	fi
}

emit_hole() {
	if [ "$ARCH" = arm64 ]; then
		echo "	brk #0x100"
		echo "	brk #0x100"
	else
		echo "	.fill 16, 1, 0xcc"
	fi
}

{
	echo '/* 自动生成，勿改。见 scripts/ikaslr/gen_stubs.sh */'
	echo '	.section .ikaslr.stubs,"ax",%progbits'
	# 2 的幂对齐：观察点/PKS 覆盖整块的要求（§4.2/M6）先按页对齐起步。
	echo '	.balign 4096'
	echo '	.globl __ikaslr_stubs'
	echo '__ikaslr_stubs:'

	if [ -z "$ks" ]; then
		echo '	/* 本次构建无被随机化函数 */'
	else
		max=$(echo "$ks" | tail -n1)
		exist=" $(echo "$ks" | tr '\n' ' ') "
		k=0
		while [ "$k" -le "$max" ]; do
			case "$exist" in
			*" $k "*)
				echo "	.globl __ikaslr_stub_$k"
				echo "__ikaslr_stub_$k:"
				emit_stub "$k"
				;;
			*)
				emit_hole
				;;
			esac
			k=$((k + 1))
		done
	fi
	echo '	.globl __ikaslr_stubs_end'
	echo '__ikaslr_stubs_end:'
} > "$OUT"
