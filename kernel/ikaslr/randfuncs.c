// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR：可迁移的「一般函数」示例与验证（论文 §3.4.3，S1.6/S1.7）。
 *
 * selftest.c 里的自测函数是**自足**的（不调外部函数、不引用全局变量），
 * 因此天然可搬。真实内核函数不是这样：`vfs_read` 既调别的函数也访问全局数据。
 * 本文件放的就是这样一个函数，用来验证一般函数确实能被迁移。
 *
 * 为什么本文件要单独用 -mcmodel=large 编译
 * ----------------------------------------
 * 函数体搬走之后，**任何指向区域外的 PC 相对引用都会失效**。内核默认的
 * -mcmodel=kernel 恰恰把外部引用编成 PC 相对：
 *
 *     add  %edi,0x0(%rip)      # R_X86_64_PC32  g_var
 *     call ext_fn              # R_X86_64_PLT32 ext_fn
 *
 * 换成 -mcmodel=large -fno-pic 之后，外部引用变成 64 位绝对地址：
 *
 *     movabs $g_var,%rbx       # R_X86_64_64
 *     movabs $ext_fn,%rax      # R_X86_64_64
 *     call   *%rax
 *
 * 绝对地址不随代码位置改变，函数内部的跳转仍是 PC 相对但随整体搬移自动保持
 * 正确——于是整个函数体可以逐字节搬到任意地址。代价是代码变大（movabs 10 字节）
 * 与多一次间接调用。
 *
 * 架构差异（实测，见 03-randomization.md）
 * ---------------------------------------
 * x86-64 有 64 位绝对立即数，因此"函数级位置无关"几乎是白送的，**不需要 GOT**。
 * arm64 没有：`adrp` 是 PC 相对的页寻址，`bl` 是 PC 相对的 ±128MB 跳转。
 * arm64 上 -mcmodel=large 会把全局地址放进**随函数一起搬走的字面量池**（可用），
 * 但 `bl` 仍是 PC 相对，**必须改成经字面量池的间接调用**。
 * 也就是说：论文 §3.4.3 的共享 GOT / 间接层在 arm64 上是刚需，在 x86-64 上是
 * 可选的优化（省代码体积）。这一点论文当前版本没有区分。
 */
#define pr_fmt(fmt) "ikaslr/randfuncs: " fmt

#include <linux/ikaslr.h>
#include <linux/kernel.h>
#include <linux/printk.h>

#include "internal.h"

/* 一个全局变量：随机化函数会引用它，用于验证全局引用在迁移后仍正确。*/
int ikaslr_rf_counter;

/*
 * 一个位于**非随机化区域**的外部函数，代表随机化代码要调用的内核其余部分。
 * 它必须被登记进白名单，否则 fixed_out 会告警。
 */
int ikaslr_rf_helper(int x);
IKASLR_WHITELIST(ikaslr_rf_helper);

/*
 * 「一般函数」：既引用全局变量，又经 fixed_out 调用非随机化区域的函数。
 * 这两件事正是自足函数所没有、而真实内核函数普遍具备的。
 */
IKASLR_RAND_FN(int, ikaslr_rf_general, int a)
{
	int r;

	ikaslr_rf_counter += a;			/* 全局变量引用 */
	r = IKASLR_OUT_CALL(ikaslr_rf_helper, a);  /* 经 fixed_out 的跨区域调用 */
	return r + ikaslr_rf_counter;
}

IKASLR_TRAMP_FN(int, ikaslr_rf_general, int a)
{
	int ret;

	ikaslr_enter();
	ret = IKASLR_TARGET(ikaslr_rf_general)(a);
	ikaslr_leave();
	return ret;
}
