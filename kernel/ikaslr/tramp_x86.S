/* SPDX-License-Identifier: GPL-2.0 */
/*
 * I-KASLR 跳板与返回标记解析的共享汇编（x86-64，论文 §3.3.2/§3.3.3）。
 *
 * 与 arm64（tramp.S）同构，差别全在「x86 的返回地址在栈上、不在链接寄存器」：
 * 返回标记不放寄存器，而是像返回地址一样 **push 到栈上**，跳板窥探栈顶来区分。
 *
 * 本文件放每次随机化都不变、全内核共享一份的例程：resolve_token、间接出口 thunk、
 * 阻断慢路径入口，以及跳板要用的偏移绝对符号。每函数入口跳板 fixed_in_F、每目标
 * 出口跳板 fixed_out_G 由 LLVM pass 逐个发出（module inline asm，见 IKaslrPass.cpp）。
 *
 * ============================================================================
 * 返回标记编码（与 arm64 相同的位布局）
 * ============================================================================
 *   marker = (k << 32) | Δ
 *     Δ = 返回点 - 函数体入口，低 32 位（实际 <1 MB）
 *     k = 函数在名单中的下标，bits[47:32]
 *     bits[63:48] = 0
 *   x86-64 内核代码地址高位恒为 1（0xffffffff8...），故 bit63=1；标记 bit63=0。
 *   跳板据此区分栈顶是真实返回地址（ret）还是标记（jmp resolve_token）。
 *   标记像返回地址一样躺在栈上：区域内调用点以 `push marker; jmp 目标` 到达跳板。
 *
 * ============================================================================
 * 计数与内外标志在跳板里的形态（协议与正确性论证见 track.c 文件头）
 * ============================================================================
 *   内外标志：thread_info.ikaslr_inside，经 current 访问：
 *       movq %gs:pcpu_hot, %r11            # current（current_task 在 pcpu_hot 偏移 0）
 *       movl IKASLR_TI_INSIDE(%r11), ...
 *   per-CPU 计数：enter/exit 各一条 `incq %gs:ikaslr_{enter,exit}_cnt`（单指令，
 *   对本核中断原子）。进入侧「加一→mfence→查阻断」，mfence 补 x86 的 store-load
 *   重排（对应 arm64 的 dmb ish）。COUNT_GLOBAL 对照实现由 pass 发 `lock incq 全局`。
 *   跳板只用 rax/rcx/rdx/rsi/rdi/r8-r11 里的临时寄存器；热路径不碰被调用者保存寄存器。
 *
 * ============================================================================
 * 每函数入口跳板 fixed_in_F（pass 逐个发出，IKaslrPass.cpp::buildFixedInAsmX86）
 * ============================================================================
 *   到达方式：区域外 `call F`（栈顶=真实返回地址，标志为 0）；区域内调用点
 *   `push marker; jmp F`（栈顶=标记，标志为 1）。stub_k 由链接期桩文件给出。
 *
 *	F:					# 保留原函数名，外部调用者不改
 *		movq	%gs:pcpu_hot, %r10	# current
 *		movl	IKASLR_TI_INSIDE(%r10), %r11d
 *		pushq	%r11			# 保存进入前的标志
 *		testl	%r11d, %r11d
 *		jnz	2f			# 标志为 1：区域内部调用，不计数
 *		incq	%gs:ikaslr_enter_cnt	# 从区域外进入
 *		mfence				# 先加一，再查阻断标志
 *		movl	$1, IKASLR_TI_INSIDE(%r10)
 *	2:	cmpl	$0, ikaslr_blocked(%rip)
 *		jne	8f
 *	3:	call	__ikaslr_stub_<k>	# 入口桩 `jmp F_body`；函数体 ret 落回此处
 *		popq	%r11			# 取回进入前的标志
 *		testl	%r11d, %r11d
 *		jnz	5f
 *		movq	%gs:pcpu_hot, %r10	# 跨边界返回：清标志、exit++、ret
 *		movl	$0, IKASLR_TI_INSIDE(%r10)
 *		incq	%gs:ikaslr_exit_cnt
 *		ret				# 栈顶=真实返回地址
 *	5:	cmpl	$0, ikaslr_blocked(%rip)	# 返回到区域内的调用者
 *		jne	9f
 *	6:	cmpq	$0, (%rsp)		# 兜底：栈顶若是真实地址（bit63=1）则 ret
 *		js	7f
 *		jmp	resolve_token		# 栈顶=标记
 *	7:	ret
 *	8:	call	__ikaslr_blocked_slow ;  jmp 3b
 *	9:	call	__ikaslr_blocked_slow ;  jmp 6b
 *
 * ============================================================================
 * 每目标出口跳板 fixed_out_G（pass 逐个发出，buildFixedOutAsmX86）
 * ============================================================================
 *   到达方式：函数体内 `push marker; jmp fixed_out_G`，栈顶=标记。`call G` 写死在
 *   只读固定区，目标集合由构造保证，无需运行期白名单查询。
 *
 *	fixed_out_G:
 *		movq	%gs:pcpu_hot, %r10
 *		movl	IKASLR_TI_INSIDE(%r10), %r11d
 *		pushq	%r11			# 保存标志
 *		testl	%r11d, %r11d
 *		jz	2f			# 未被计数的执行流：直接调用
 *		movl	$0, IKASLR_TI_INSIDE(%r10)
 *		incq	%gs:ikaslr_exit_cnt	# 离开区域；此后本执行流可被搬走
 *	2:	call	G			# 直接调用外部函数
 *		popq	%r11
 *		testl	%r11d, %r11d
 *		jz	4f
 *		movq	%gs:pcpu_hot, %r10	# 回到区域：先加一再查阻断
 *		incq	%gs:ikaslr_enter_cnt
 *		mfence
 *		movl	$1, IKASLR_TI_INSIDE(%r10)
 *		cmpl	$0, ikaslr_blocked(%rip)
 *		jne	9f
 *	4:	jmp	resolve_token		# 出口跳板总是经标记返回
 *	9:	call	__ikaslr_blocked_slow ;  jmp 4b
 *
 * ============================================================================
 * 函数体前的重入序列（prefix data，pass 附在 F_body 之前 IKASLR_X86_PREFIX 字节）
 * ============================================================================
 *   随函数体一起搬移；重入桩 `jmp F_reentry`（= jmp F_body-PREFIX）落到这里：
 *	lea	F_body(%rip), %r11	# 4C 8D 1D 06 00 00 00  当前函数体入口（PC 相对）
 *	add	%rcx, %r11		# 49 01 CB              + Δ（rcx 由 resolve 置好）
 *	jmp	*%r11			# 41 FF E3              落到 body+Δ
 *   共 13 字节，编码固定（F_body 恒为其后紧邻），故以 [13 x i8] 常量发出。
 */

#include <linux/linkage.h>
#include <asm/asm-offsets.h>

/* thread_info.ikaslr_inside 在 task 内的偏移，供 pass 发出的跳板引用。*/
	.globl	__ikaslr_ti_inside
	.set	__ikaslr_ti_inside, IKASLR_TI_INSIDE

/*
 * 本核 enter/exit 计数加一（与 pass 发出的序列、track.c 的存储一致）。
 * 默认每核一条 `incq %gs:var`；COUNT_GLOBAL 对照用带 lock 的全局计数。
 */
	.macro	IKASLR_INC_ENTER
#ifdef CONFIG_IKASLR_COUNT_GLOBAL
	lock incq ikaslr_enter_g(%rip)
#else
	incq	%gs:ikaslr_enter_cnt
#endif
	.endm
	.macro	IKASLR_INC_EXIT
#ifdef CONFIG_IKASLR_COUNT_GLOBAL
	lock incq ikaslr_exit_g(%rip)
#else
	incq	%gs:ikaslr_exit_cnt
#endif
	.endm

	.text

/*
 * resolve_token —— 把栈顶的返回标记解析回当前变体里的函数体位置。
 * 只由分支与 PC 相对计算构成，不读任何保存地址的内存（桩是被**执行**而非读取）。
 *   入口：栈顶 = (k<<32)|Δ
 *   动作：弹出标记；rcx=Δ；由 k 定位第 k 个桩的重入桩（stub_base+16k+8）并跳过去；
 *         重入桩 `jmp F_reentry` → 函数体前的重入序列 → body+Δ。
 *   只破坏 rcx/r10/r11（返回路径上 rax/rdx 保存返回值，不能碰）。
 */
SYM_CODE_START(resolve_token)
	popq	%r11			# marker
	movl	%r11d, %ecx		# rcx = Δ（低 32 位，零扩展）
	shrq	$32, %r11		# r11 = k
	andq	$0xffff, %r11
	shlq	$4, %r11		# k * 16
	leaq	__ikaslr_stubs(%rip), %r10
	addq	%r11, %r10		# &stub[k]
	addq	$8, %r10		# → 重入桩
	jmp	*%r10			# 执行 `jmp F_reentry`
SYM_CODE_END(resolve_token)

/*
 * __ikaslr_blocked_slow —— 跳板查到阻断标志后的慢路径入口（四处共用）。
 * 进入时 rax/rcx/rdx/rsi/rdi/r8-r11 可能是待交给函数体的参数或待交还的返回值，
 * 一律保存；真正的工作在 track.c::ikaslr_blocked_slow。
 */
SYM_CODE_START(__ikaslr_blocked_slow)
	pushq	%rax
	pushq	%rcx
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%r8
	pushq	%r9
	pushq	%r10
	pushq	%r11			# 9 次 push：从 8 字节错位对齐到 16 字节对齐
	call	ikaslr_blocked_slow
	popq	%r11
	popq	%r10
	popq	%r9
	popq	%r8
	popq	%rdi
	popq	%rsi
	popq	%rdx
	popq	%rcx
	popq	%rax
	ret
SYM_CODE_END(__ikaslr_blocked_slow)

/*
 * __ikaslr_indirect_out —— 函数体内**间接调用**的出口 thunk。
 *
 * 间接目标可能是普通函数（末尾 ret）。若像直接调用那样 push 标记再跳过去，它会
 * ret 到标记上崩溃。因此间接调用跳到这里：本 thunk 用**真实返回地址** call 目标
 * （返回落回本 thunk），返回后再按标记 resolve_token 回函数体。等价于目标在寄存器
 * 里的 fixed_out，计数序列与 fixed_out 相同。
 *
 *   入口：栈顶 = 标记；%r11 = 目标函数指针；参数在 rdi/rsi/rdx/rcx/r8/r9
 */
SYM_CODE_START(__ikaslr_indirect_out)
	pushq	%r11			# 保存目标（计数会用 r11）  栈:[target][marker]
	movq	%gs:pcpu_hot, %r10	# current
	movl	__ikaslr_ti_inside(%r10), %r11d
	pushq	%r11			# 保存标志  栈:[flag][target][marker]
	testl	%r11d, %r11d
	jz	1f
	movl	$0, __ikaslr_ti_inside(%r10)
	IKASLR_INC_EXIT			# 离开区域
1:	movq	8(%rsp), %r11		# 取回目标（flag 在 0、target 在 8）
	call	*%r11			# 真实返回地址调用，返回落回此处
	popq	%r11			# 标志  栈:[target][marker]
	testl	%r11d, %r11d
	jz	2f
	movq	%gs:pcpu_hot, %r10	# 回到区域：先加一再查阻断
	IKASLR_INC_ENTER
	mfence
	movl	$1, __ikaslr_ti_inside(%r10)
	cmpl	$0, ikaslr_blocked(%rip)
	jne	3f
2:	addq	$8, %rsp		# 丢弃保存的目标  栈:[marker]
	jmp	resolve_token
3:	call	__ikaslr_blocked_slow
	jmp	2b
SYM_CODE_END(__ikaslr_indirect_out)
