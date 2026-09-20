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
 * 每函数入口跳板 fixed_in_F（M1d 由 pass 逐个发出；此处给出经汇编验证的模板）
 * ============================================================================
 *   到达方式：区域外 `bl F`（x30=真实返回地址）；区域内 M1c 调用点 `br x16`
 *   （x30=返回标记）。<k> 是该函数下标，__ikaslr_stub_<k> 由链接期桩文件给出。
 *
 *	F:					// 保留原函数名，外部调用者不改
 *		sub	sp, sp, #96
 *		stp	x0, x1, [sp, #0]
 *		stp	x2, x3, [sp, #16]
 *		stp	x4, x5, [sp, #32]
 *		stp	x6, x7, [sp, #48]
 *		stp	x8, x30, [sp, #64]	// x8=sret；x30=调用者返回地址/标记
 *		str	x29, [sp, #80]
 *		add	x29, sp, #80
 *		bl	ikaslr_enter		// C 计数/阻断（参数已存栈）
 *		ldp	x0, x1, [sp, #0]
 *		ldp	x2, x3, [sp, #16]
 *		ldp	x4, x5, [sp, #32]
 *		ldp	x6, x7, [sp, #48]
 *		ldr	x8, [sp, #64]
 *		bl	__ikaslr_stub_<k>	// 入口桩 `b F_entry`；函数体返回落回此处
 *		stp	x0, x1, [sp, #0]	// 保存函数体返回值
 *		bl	ikaslr_leave
 *		ldp	x0, x1, [sp, #0]
 *		ldr	x30, [sp, #72]		// 取回调用者返回地址/标记
 *		ldr	x29, [sp, #80]
 *		add	sp, sp, #96
 *		tbz	x30, #63, 1f		// 标记 → 解析；真实地址 → ret
 *		ret
 *	1:	b	resolve_token
 *
 * ============================================================================
 * 每目标出口跳板 fixed_out_G（M1d 由 pass 逐个发出）
 * ============================================================================
 *   到达方式：M1c 调用点 `br x16`，x30=返回标记。fixed_out 是固定代码，可用
 *   PC 相对寻址直接引用外部函数 G。
 *
 *	fixed_out_G:
 *		sub	sp, sp, #96
 *		stp	x0, x1, [sp, #0]  ... stp x6,x7,[sp,#48]
 *		stp	x8, x30, [sp, #64]	// x30 = 返回标记
 *		str	x29, [sp, #80]  ;  add x29, sp, #80
 *		adrp	x0, G  ;  add x0, x0, :lo12:G	// &G 供白名单检查
 *		bl	ikaslr_out_enter	// 白名单 + 计数--
 *		ldp	x0..x7 ; ldr x8			// 恢复给 G 的参数
 *		bl	G				// 直接调用外部函数
 *		stp	x0, x1, [sp, #0]		// 保存 G 的返回值
 *		bl	ikaslr_out_leave		// 计数++
 *		ldp	x0, x1, [sp, #0]
 *		ldr	x30, [sp, #72]  ;  ldr x29, [sp, #80]  ;  add sp, sp, #96
 *		b	resolve_token		// 出口跳板总是经标记返回
 *
 * ============================================================================
 * 函数体前的重入序列（prefix data，M1d 由 pass 附在 F_body 之前 12 字节）
 * ============================================================================
 *   随函数体一起搬移；重入桩 `b F_reentry`（= b F_body-12）落到这三条指令：
 *	adr	x16, F_entry		// 0x10000070  当前函数体入口（PC 相对）
 *	add	x16, x16, x17		// 0x8b110210  + Δ（x17 由 resolve 置好）
 *	br	x16			// 0xd61f0200  落到 body+Δ
 *   三条编码固定（F_entry 恒为其后 12 字节），故以 [3 x i32] 常量发出。
 *
 * 注：当前为“冒烟正确性优先”版——计数经 bl ikaslr_enter/leave/out_enter/
 * out_leave 的 C 调用完成，需在栈上保存/恢复参数寄存器。冒烟通过后，把这几处
 * 计数改为只用 x16/x17 的内联汇编（论文 §3.3.2 的形态），才能达到表 3-3 的
 * 常驻税数字。结构（方案甲、直接 bl 桩、标记返回）不变。
 */

#include <linux/linkage.h>

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
 * __ikaslr_indirect_out —— 函数体内**间接调用**的出口 thunk。
 *
 * 间接调用的目标是运行期函数指针，可能指向普通（非随机化）函数——那种函数
 * 末尾是 `ret`，若像直接调用那样把返回标记放进 x30 再 br 过去，它会 ret 到
 * 标记值上崩溃（实测 pc = 0x16_00000174 即一枚 (k,Δ) 标记）。因此间接调用不
 * 直接跳目标，而是跳到这里：本 thunk 用**真实返回地址** blr 目标（无论目标是
 * 普通函数还是随机化函数的入口跳板都能正确返回到本 thunk），返回后再按标记
 * 解析回函数体。等价于"目标在寄存器里的 fixed_out"。
 *
 *   入口：x16 = 目标函数指针，x30 = 返回标记，参数在 x0..x7
 */
SYM_CODE_START(__ikaslr_indirect_out)
	sub	sp, sp, #96
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x30, [sp, #64]		// x8=sret；x30=返回标记
	str	x16, [sp, #88]			// 保存目标指针（C 调用会破坏 x16）
	str	x29, [sp, #80]
	add	x29, sp, #80
	bl	ikaslr_out_enter_indirect	// 离开区域：计数--（间接目标不查白名单）
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldr	x8, [sp, #64]
	ldr	x16, [sp, #88]			// 取回目标
	blr	x16				// 真实返回地址调用（返回落回本 thunk）
	stp	x0, x1, [sp, #0]		// 保存返回值
	bl	ikaslr_out_leave		// 回到区域：计数++
	ldp	x0, x1, [sp, #0]
	ldr	x30, [sp, #72]			// 取回返回标记
	ldr	x29, [sp, #80]
	add	sp, sp, #96
	b	resolve_token			// 按标记解析回函数体
SYM_CODE_END(__ikaslr_indirect_out)
