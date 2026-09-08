// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 陈旧返回地址的修正（方案 B，论文 §3.4.5 的兜底机制）。
 *
 * 背景：fixed_out 在离开随机化区域时减少活跃计数，因此随机化可以在"某个随机化
 * 函数正阻塞在外部调用中"时进行。代价是该执行流的栈帧仍在栈上、返回地址指向
 * **已退役的变体**。
 *
 * 兜底方式：退役变体被整体填充为陷阱指令（x86 int3 / arm64 UDF）。陈旧的返回
 * 因此不会执行到任何旧代码，而是立即产生异常；本文件在异常处理中把 PC 改写到
 * 当前变体中的对应位置，执行流随即在新地址上继续。
 *
 * 这同时保证了无副本性质：退役变体里没有任何可用代码，全是陷阱指令，因而不构成
 * gadget 来源。
 */
#define pr_fmt(fmt) "ikaslr: " fmt

#include <linux/ikaslr.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/kdebug.h>
#include <linux/notifier.h>
#include <linux/ptrace.h>
#include <linux/atomic.h>
#include <linux/string.h>

#include "internal.h"

static atomic_long_t ikaslr_fixups;
static atomic_long_t ikaslr_fixup_fails;

void ikaslr_fixup_stats(unsigned long *ok, unsigned long *fail)
{
	*ok = atomic_long_read(&ikaslr_fixups);
	*fail = atomic_long_read(&ikaslr_fixup_fails);
}

static int ikaslr_die_notify(struct notifier_block *nb, unsigned long val,
			     void *data)
{
	struct die_args *args = data;
	struct pt_regs *regs;
	unsigned long faulting, target;

	if (!args || !args->regs)
		return NOTIFY_DONE;
	regs = args->regs;

#ifdef CONFIG_X86_64
	/*
	 * int3 是陷阱：异常发生时 RIP 已指向 int3 之后一字节，因此触发地址是
	 * regs->ip - 1。这一点必须算对，否则映射会整体偏一个字节。
	 */
	if (val != DIE_INT3)
		return NOTIFY_DONE;
	faulting = regs->ip - 1;
#elif defined(CONFIG_ARM64)
	/* UDF 是故障：PC 指向出错指令本身。*/
	if (!args->str || !strstr(args->str, "undefined instruction"))
		return NOTIFY_DONE;
	faulting = regs->pc;
#else
	return NOTIFY_DONE;
#endif

	/* 先看是不是落在被探测函数的陷阱区（第 4 章控制流审计）。*/
	if (ikaslr_detect_audit(faulting, &target)) {
#ifdef CONFIG_X86_64
		regs->ip = target;
#elif defined(CONFIG_ARM64)
		regs->pc = target;
#endif
		return NOTIFY_STOP;
	}

	if (!ikaslr_fixup_addr(faulting, &target)) {
		/* 不是落在退役变体里的地址：与本机制无关，交给其它处理者。*/
		return NOTIFY_DONE;
	}

	atomic_long_inc(&ikaslr_fixups);
#ifdef CONFIG_X86_64
	regs->ip = target;
#elif defined(CONFIG_ARM64)
	regs->pc = target;
#endif
	return NOTIFY_STOP;
}

static struct notifier_block ikaslr_die_nb = {
	.notifier_call = ikaslr_die_notify,
	/* 优先级高于默认处理者，确保在崩溃处理之前拿到这次异常。*/
	.priority = 0x7fffffff,
};

int __init ikaslr_fixup_init(void)
{
	int ret = register_die_notifier(&ikaslr_die_nb);

	if (ret)
		pr_err("failed to register die notifier: %d\n", ret);
	else
		pr_info("stale-return fixup armed (%s)\n",
			IS_ENABLED(CONFIG_X86_64) ? "int3" : "udf");
	return ret;
}
