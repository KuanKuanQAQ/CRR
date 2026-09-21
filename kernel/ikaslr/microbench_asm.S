/* SPDX-License-Identifier: GPL-2.0 */
/*
 * I-KASLR 微基准——跳板指令序列的分项循环（arm64）。
 *
 * 每个例程把跳板里的一段序列原样放进一个计数循环：x0 = 迭代次数。序列与
 * tramp.S / IKaslrPass.cpp 发出的逐条相同，只是计数落在专用的 ikaslr_mb_cnt 上
 * （真实的 ikaslr_cnt 只增不减，单加 enter 会让区域永远「非空」）。
 *
 * 这些循环量的是**序列本身**在流水线里的成本：没有调用/返回、分支全可预测，
 * 因此是下界。端到端的数（含间接分支预测）由 microbench.c 里经真实跳板的调用给出。
 * 两者对照，才能把「计数贵」与「按标记返回的间接分支贵」分开。
 */
#include <linux/linkage.h>
#include <asm/asm-offsets.h>

	.text

/* 取本核的计数地址到 x9。x16 = current；破坏 x17。*/
	.macro	mb_cnt_addr
	ldr	w17, [x16, #TSK_TI_CPU]
	adrp	x9, ikaslr_mb_cnt
	add	x9, x9, :lo12:ikaslr_mb_cnt
	add	x9, x9, x17, lsl #6
	.endm

	.macro	mb_llsc
1:	ldxr	x17, [x9]
	add	x17, x17, #1
	stxr	w10, x17, [x9]
	cbnz	w10, 1b
	.endm

/* 空循环：其余各项都要减去它。*/
SYM_FUNC_START(ikaslr_mb_loop_empty)
0:	subs	x0, x0, #1
	b.ne	0b
	ret
SYM_FUNC_END(ikaslr_mb_loop_empty)

/* 读 current、读内外标志、写回——跳板两端对标志的访问。*/
SYM_FUNC_START(ikaslr_mb_loop_flag)
0:	mrs	x16, sp_el0
	ldr	w17, [x16, #TSK_TI_IKASLR_INSIDE]
	str	w17, [x16, #TSK_TI_IKASLR_INSIDE]
	subs	x0, x0, #1
	b.ne	0b
	ret
SYM_FUNC_END(ikaslr_mb_loop_flag)

/* 查阻断标志。*/
SYM_FUNC_START(ikaslr_mb_loop_blocked)
0:	adrp	x9, ikaslr_blocked
	ldr	w9, [x9, :lo12:ikaslr_blocked]
	cbnz	w9, 1f
1:	subs	x0, x0, #1
	b.ne	0b
	ret
SYM_FUNC_END(ikaslr_mb_loop_blocked)

/* 单独一条屏障。*/
SYM_FUNC_START(ikaslr_mb_loop_dmb)
0:	dmb	ish
	subs	x0, x0, #1
	b.ne	0b
	ret
SYM_FUNC_END(ikaslr_mb_loop_dmb)

/* 取本核计数地址 + LL/SC 原子加一（现行跳板的 <count>）。*/
SYM_FUNC_START(ikaslr_mb_loop_count_llsc)
	mrs	x16, sp_el0
0:	mb_cnt_addr
	mb_llsc
	subs	x0, x0, #1
	b.ne	0b
	ret
SYM_FUNC_END(ikaslr_mb_loop_count_llsc)

/* 同上，原子加一换成 LSE 的单条 ldadd（调用前须确认 CPU 支持）。*/
	.arch_extension lse
SYM_FUNC_START(ikaslr_mb_loop_count_lse)
	mrs	x16, sp_el0
	mov	x11, #1
0:	mb_cnt_addr
	ldadd	x11, x17, [x9]
	subs	x0, x0, #1
	b.ne	0b
	ret
SYM_FUNC_END(ikaslr_mb_loop_count_lse)

/* 不带原子性的加一：假想的「关抢占 + 普通读改写」上界，仅作参照，不可用于实现。*/
SYM_FUNC_START(ikaslr_mb_loop_count_plain)
	mrs	x16, sp_el0
0:	mb_cnt_addr
	ldr	x17, [x9]
	add	x17, x17, #1
	str	x17, [x9]
	subs	x0, x0, #1
	b.ne	0b
	ret
SYM_FUNC_END(ikaslr_mb_loop_count_plain)

/* 跨边界进入的完整序列：读标志、加一、屏障、置标志、查阻断（不含压栈与 bl）。*/
SYM_FUNC_START(ikaslr_mb_loop_enter_seq)
0:	mrs	x16, sp_el0
	ldr	w17, [x16, #TSK_TI_IKASLR_INSIDE]
	cbnz	w17, 2f
	mb_cnt_addr
	mb_llsc
	dmb	ish
	ldr	w17, [x16, #TSK_TI_IKASLR_INSIDE]	// 写回原值，不真的置 1
	str	w17, [x16, #TSK_TI_IKASLR_INSIDE]
2:	adrp	x9, ikaslr_blocked
	ldr	w9, [x9, :lo12:ikaslr_blocked]
	cbnz	w9, 3f
3:	subs	x0, x0, #1
	b.ne	0b
	ret
SYM_FUNC_END(ikaslr_mb_loop_enter_seq)

/* 同上但去掉屏障：估计「把屏障挪到随机化一侧」能省多少。*/
SYM_FUNC_START(ikaslr_mb_loop_enter_seq_nodmb)
0:	mrs	x16, sp_el0
	ldr	w17, [x16, #TSK_TI_IKASLR_INSIDE]
	cbnz	w17, 2f
	mb_cnt_addr
	mb_llsc
	ldr	w17, [x16, #TSK_TI_IKASLR_INSIDE]
	str	w17, [x16, #TSK_TI_IKASLR_INSIDE]
2:	adrp	x9, ikaslr_blocked
	ldr	w9, [x9, :lo12:ikaslr_blocked]
	cbnz	w9, 3f
3:	subs	x0, x0, #1
	b.ne	0b
	ret
SYM_FUNC_END(ikaslr_mb_loop_enter_seq_nodmb)
