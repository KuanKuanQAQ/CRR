// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 自测函数（仅 CONFIG_IKASLR_DEBUG）。
 *
 * 目的：给随机化机制提供真实的被随机化对象，使 S1.2 之后的每一步（跳板、
 * 线程追踪、迁移、推迟随机化）都能在 QEMU 中端到端验证，而不必等到把
 * fs/read_write.c 一类的真实内核路径改造完成。
 *
 * 这些函数刻意写得自足（不调用外部函数、不引用全局变量），因为在 LLVM pass
 * 就绪之前，.rand.text 中的函数体并非位置无关代码，迁移需要重定位；自足的
 * 函数体只含 PC 相对的内部跳转，是当前手工路径下可安全迁移的子集。
 * 这一限制见 03-randomization.md 与 PROGRESS.md 的 S1.4 备注。
 */
#define pr_fmt(fmt) "ikaslr/selftest: " fmt

#include <linux/ikaslr.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/errno.h>

/* ---- 被随机化函数一：整数运算，自足 ---- */
IKASLR_RAND_FN(int, ikaslr_st_add, int a, int b)
{
	int i, acc = a;

	for (i = 0; i < b; i++)
		acc += 1;
	return acc;
}

IKASLR_TRAMP_FN(int, ikaslr_st_add, int a, int b)
{
	int ret;

	ikaslr_enter();
	ret = IKASLR_TARGET(ikaslr_st_add)(a, b);
	ikaslr_leave();
	return ret;
}

/* ---- 被随机化函数二：另一个自足函数，用于验证多函数布局与大小推导 ---- */
IKASLR_RAND_FN(int, ikaslr_st_mul, int a, int b)
{
	int i, acc = 0;

	for (i = 0; i < b; i++)
		acc += a;
	return acc;
}

IKASLR_TRAMP_FN(int, ikaslr_st_mul, int a, int b)
{
	int ret;

	ikaslr_enter();
	ret = IKASLR_TARGET(ikaslr_st_mul)(a, b);
	ikaslr_leave();
	return ret;
}

static int __init ikaslr_selftest_init(void)
{
	int a = ikaslr_st_add(40, 2);
	int m = ikaslr_st_mul(6, 7);

	pr_info("add(40,2)=%d mul(6,7)=%d nr_funcs=%d\n",
		a, m, ikaslr_nr_funcs());
	if (a != 42 || m != 42) {
		pr_err("FAIL: trampoline dispatch returned wrong result\n");
		return -EINVAL;
	}
	pr_info("PASS: calls reached the bodies through their trampolines\n");
	return 0;
}
/* 在 core 的 late_initcall 之后运行，确保 target 槽已初始化。*/
late_initcall_sync(ikaslr_selftest_init);
