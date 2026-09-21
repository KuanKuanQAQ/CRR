/* SPDX-License-Identifier: GPL-2.0 */
/*
 * I-KASLR 跳板与返回标记解析的共享汇编（arm64，论文 §3.3.2/§3.3.3）。
 *
 * 本文件只放**每次随机化都不变、且全内核共享一份**的例程：resolve_token。
 * 每函数的入口跳板 fixed_in_F、每目标的出口跳板 fixed_out_G 由 LLVM pass 按
 * 方案甲逐个发出（module inline asm，见 tools/ikaslr/llvm/IKaslrPass.cpp，M1d）。
 *
 * ============================================================================
 * 返回标记编码（M1c 已落地，见 IKaslrPass.cpp::rewriteCallSitesAArch64）
 * ============================================================================
 *   x30 = (k << 32) | Δ
 *     Δ   = 返回点 - 函数体入口，低 32 位（实际 <1 MB）；由两条 adr 相减求得
 *     k   = 函数在 IKASLR_FUNCS 名单中的下标，bits[47:32]（movk #k,lsl #32）
 *     bits[63:48] = 0
 *   真实内核返回地址 bits[63:48] = 全 1（内核地址）。因此 `tbz x30,#63` 一条指令
 *   即可区分：bit63=0 → 标记（经 resolve_token 返回）；=1 → 真实地址（ret）。
 *
 * ============================================================================
 * 执行流追踪在跳板里的形态（论文 §3.3.2；协议与正确性论证见 track.c 文件头）
 * ============================================================================
 *   跳板只用 x16/x17 与 x9/x10（函数边界上均为可破坏的临时寄存器），不碰参数与
 *   返回值寄存器，因此热路径上没有任何寄存器保存/恢复，栈上只压 16 字节：
 *   进入前的内外标志 + 调用者的返回地址或标记。
 *
 *   <count enter> / <count exit> 指下面这段（exit 把符号换成 ikaslr_cnt+8）：
 *		ldr	w17, [x16, :lo12:__ikaslr_ti_cpu]	// x16 = current
 *		adrp	x9, ikaslr_cnt
 *		add	x9, x9, :lo12:ikaslr_cnt
 *		add	x9, x9, x17, lsl #6		// 本核的那一对计数
 *	1:	ldxr	x17, [x9]
 *		add	x17, x17, #1
 *		stxr	w10, x17, [x9]
 *		cbnz	w10, 1b
 *   CONFIG_IKASLR_COUNT_GLOBAL（对照实现）下没有第 1、4 两条，所有核共用第 0 对。
 *
 *   thread_info 里两个字段的偏移依赖内核配置，而每函数跳板由 LLVM pass 发出、
 *   拿不到 asm-offsets.h。做法是本文件把偏移定义成**绝对符号**
 *   （__ikaslr_ti_inside / __ikaslr_ti_cpu），跳板以 `:lo12:` 重定位引用，
 *   链接器把立即数填成偏移本身。
 *
 * ============================================================================
 * 每函数入口跳板 fixed_in_F（pass 逐个发出，IKaslrPass.cpp::buildFixedInAsm）
 * ============================================================================
 *   到达方式：区域外 `bl F`（x30=真实返回地址，标志为 0）；区域内调用点 `br x16`
 *   （x30=返回标记，标志为 1）。
 *
 *	F:					// 保留原函数名，外部调用者不改
 *		mrs	x16, sp_el0
 *		ldr	w17, [x16, :lo12:__ikaslr_ti_inside]
 *		stp	x17, x30, [sp, #-16]!	// 进入前的标志 + 返回地址/标记
 *		cbnz	w17, 2f			// 区域内部的调用：不计数
 *		<count enter>
 *		dmb	ish			// 先加一，再查阻断标志
 *		mov	w17, #1
 *		str	w17, [x16, :lo12:__ikaslr_ti_inside]
 *	2:	adrp	x9, ikaslr_blocked
 *		ldr	w9, [x9, :lo12:ikaslr_blocked]
 *		cbnz	w9, 8f
 *	3:	bl	__ikaslr_stub_<k>	// 入口桩 `b F_entry`；函数体返回落回此处
 *		ldr	w17, [sp]
 *		cbnz	w17, 5f
 *		mrs	x16, sp_el0		// 跨边界返回：清标志、exit++、ret
 *		str	wzr, [x16, :lo12:__ikaslr_ti_inside]
 *		<count exit>
 *		ldp	x17, x30, [sp], #16
 *		ret
 *	5:	adrp	x9, ikaslr_blocked	// 返回到区域内的调用者
 *		ldr	w9, [x9, :lo12:ikaslr_blocked]
 *		cbnz	w9, 9f
 *	6:	ldp	x17, x30, [sp], #16
 *		tbz	x30, #63, 7f
 *		ret				// 兜底，见下
 *	7:	b	resolve_token
 *	8:	bl	__ikaslr_blocked_slow ;  b 3b
 *	9:	bl	__ikaslr_blocked_slow ;  b 6b
 *
 *   `tbz x30,#63` 是相对论文清单多出的一条兜底：标志为 1 时 x30 理应是标记，但
 *   若某条异常入口没有经过 entry.S 里的「存标志并清零」（SDEI、el1t 之类），处理
 *   程序会带着继承来的标志 1 以真实返回地址调用 F。此时计数不受影响（被打断的
 *   执行流本就在计数内），只需别把真实地址当标记解析。
 *
 * ============================================================================
 * 每目标出口跳板 fixed_out_G（pass 逐个发出，buildFixedOutAsm）
 * ============================================================================
 *   到达方式：函数体内调用点 `br x16`，x30=返回标记。`bl G` 写死在只读的固定区，
 *   目标集合由构造保证，不需要运行期白名单查询。
 *
 *	fixed_out_G:
 *		mrs	x16, sp_el0
 *		ldr	w17, [x16, :lo12:__ikaslr_ti_inside]
 *		stp	x17, x30, [sp, #-16]!
 *		cbz	w17, 2f			// 未被计数的执行流：直接调用
 *		str	wzr, [x16, :lo12:__ikaslr_ti_inside]
 *		<count exit>			// 离开区域；此后本执行流可被搬走
 *	2:	bl	G
 *		ldr	w17, [sp]
 *		cbz	w17, 4f
 *		mrs	x16, sp_el0
 *		<count enter>			// 回到区域：同样先加一再查阻断
 *		dmb	ish
 *		mov	w17, #1
 *		str	w17, [x16, :lo12:__ikaslr_ti_inside]
 *		adrp	x9, ikaslr_blocked
 *		ldr	w9, [x9, :lo12:ikaslr_blocked]
 *		cbnz	w9, 9f
 *	4:	ldp	x17, x30, [sp], #16
 *		b	resolve_token
 *	9:	bl	__ikaslr_blocked_slow ;  b 4b
 *
 * ============================================================================
 * 函数体前的重入序列（prefix data，pass 附在 F_body 之前 12 字节）
 * ============================================================================
 *   随函数体一起搬移；重入桩 `b F_reentry`（= b F_body-12）落到这三条指令：
 *	adr	x16, F_entry		// 0x10000070  当前函数体入口（PC 相对）
 *	add	x16, x16, x17		// 0x8b110210  + Δ（x17 由 resolve 置好）
 *	br	x16			// 0xd61f0200  落到 body+Δ
 *   三条编码固定（F_entry 恒为其后 12 字节），故以 [3 x i32] 常量发出。
 */

#include <linux/linkage.h>
#include <asm/asm-offsets.h>

/* thread_info 字段偏移，供 pass 发出的跳板以 :lo12: 引用（见文件头）。*/
	.globl	__ikaslr_ti_inside
	.set	__ikaslr_ti_inside, TSK_TI_IKASLR_INSIDE
	.globl	__ikaslr_ti_cpu
	.set	__ikaslr_ti_cpu, TSK_TI_CPU

/*
 * 本核计数加一。x16 = current；破坏 x9、x10、x17。
 * \sym 取 ikaslr_cnt（enter）或 ikaslr_cnt+8（exit）。
 */
	.macro	ikaslr_count, sym
#ifndef CONFIG_IKASLR_COUNT_GLOBAL
	ldr	w17, [x16, #TSK_TI_CPU]
#endif
	adrp	x9, \sym
	add	x9, x9, :lo12:\sym
#ifndef CONFIG_IKASLR_COUNT_GLOBAL
	add	x9, x9, x17, lsl #6
#endif
1:	ldxr	x17, [x9]
	add	x17, x17, #1
	stxr	w10, x17, [x9]
	cbnz	w10, 1b
	.endm

	.text

/*
 * resolve_token —— 把 x30 中的返回标记解析回当前变体里的函数体位置。
 * 只由分支与 PC 相对计算构成，不读任何保存地址的内存。
 *   入口：x30 = (k<<32)|Δ ，x17 可被覆盖
 *   动作：x17=Δ；k 定位第 k 个跳转桩的重入桩（stub_base+8k+4），跳过去；
 *         重入桩 `b F_reentry` → 函数体前的重入序列 → body+Δ。
 */
SYM_CODE_START(resolve_token)
	ubfx	x17, x30, #0, #32		// x17 = Δ
	ubfx	x16, x30, #32, #16		// x16 = k
	adrp	x9, __ikaslr_stubs
	add	x9, x9, :lo12:__ikaslr_stubs
	add	x16, x9, x16, lsl #3		// stub_base + 8k（入口桩）
	add	x16, x16, #4			// +4 → 重入桩 `b F_reentry`
	br	x16
SYM_CODE_END(resolve_token)

/*
 * __ikaslr_blocked_slow —— 跳板查到阻断标志后的慢路径入口。
 *
 * 四处阻断检查共用（入口跳板进入/返回、出口跳板返回、间接出口返回）。进入时
 * x0..x8 里可能是尚未交给函数体的参数，也可能是待交还调用者的返回值，一律保存；
 * 真正的工作（撤销计数、等待放行、重新进入）在 C 里，见 track.c::ikaslr_blocked_slow。
 * 返回后跳板从阻断检查之后继续。
 */
SYM_CODE_START(__ikaslr_blocked_slow)
	stp	x29, x30, [sp, #-96]!
	mov	x29, sp
	stp	x0, x1, [sp, #16]
	stp	x2, x3, [sp, #32]
	stp	x4, x5, [sp, #48]
	stp	x6, x7, [sp, #64]
	str	x8, [sp, #80]
	bl	ikaslr_blocked_slow
	ldp	x0, x1, [sp, #16]
	ldp	x2, x3, [sp, #32]
	ldp	x4, x5, [sp, #48]
	ldp	x6, x7, [sp, #64]
	ldr	x8, [sp, #80]
	ldp	x29, x30, [sp], #96
	ret
SYM_CODE_END(__ikaslr_blocked_slow)

/*
 * __ikaslr_indirect_out —— 函数体内**间接调用**的出口 thunk。
 *
 * 间接调用的目标是运行期函数指针，可能指向普通（非随机化）函数——那种函数
 * 末尾是 `ret`，若像直接调用那样把返回标记放进 x30 再 br 过去，它会 ret 到
 * 标记值上崩溃（实测 pc = 0x16_00000174 即一枚 (k,Δ) 标记）。因此间接调用不
 * 直接跳目标，而是跳到这里：本 thunk 用**真实返回地址** blr 目标（无论目标是
 * 普通函数还是随机化函数的入口跳板都能正确返回到本 thunk），返回后再按标记
 * 解析回函数体。等价于"目标在寄存器里的 fixed_out"，计数序列与 fixed_out 相同。
 *
 *   入口：x16 = 目标函数指针，x30 = 返回标记，参数在 x0..x7
 */
SYM_CODE_START(__ikaslr_indirect_out)
	mov	x11, x16			// 目标；x16 要腾出来放 current
	mrs	x16, sp_el0
	ldr	w17, [x16, #TSK_TI_IKASLR_INSIDE]
	stp	x17, x30, [sp, #-16]!		// 进入前的标志 + 返回标记
	cbz	w17, 2f
	str	wzr, [x16, #TSK_TI_IKASLR_INSIDE]
	ikaslr_count ikaslr_cnt+8		// 离开区域：exit++
2:	blr	x11				// 真实返回地址调用（返回落回本 thunk）
	ldr	w17, [sp]
	cbz	w17, 4f
	mrs	x16, sp_el0
	ikaslr_count ikaslr_cnt			// 回到区域：enter++，再查阻断
	dmb	ish
	mov	w17, #1
	str	w17, [x16, #TSK_TI_IKASLR_INSIDE]
	adrp	x9, ikaslr_blocked
	ldr	w9, [x9, :lo12:ikaslr_blocked]
	cbnz	w9, 9f
4:	ldp	x17, x30, [sp], #16
	b	resolve_token			// 按标记解析回函数体
9:	bl	__ikaslr_blocked_slow
	b	4b
SYM_CODE_END(__ikaslr_indirect_out)
