// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 陷阱与陈旧返回地址的修正（方案 B，论文 §3.4.5 的兜底机制）。
 *
 * 退役变体与被探测函数被整体填成**陷阱指令**：陈旧的返回或对被探测函数的访问
 * 因此不会执行到任何旧代码，而是立即产生一个可被内核截获的异常；本文件在异常
 * 处理中把 PC 改写到当前变体/正常副本中的对应位置，执行流随即在新地址继续。
 * 这同时保证无副本性质——退役区里没有可用代码，全是陷阱，不构成 gadget 来源。
 *
 * 每架构用其"干净的软件断点"：
 *   x86-64：int3（0xCC，单字节陷阱）经 die notifier 的 DIE_INT3，可干净恢复；
 *   arm64 ：BRK #IKASLR_BRK_IMM 经**内核 break hook**（register_kernel_break_hook）。
 *           不用 UDF+die：arm64 的未定义指令走 die()，即使 notifier 返回 NOTIFY_STOP
 *           能恢复，也会每次打印 oops 并污染内核；BRK+break hook 才是 int3 的对应物，
 *           干净、无 oops、无污染。
 */
#define pr_fmt(fmt) "ikaslr: " fmt

#include <linux/ikaslr.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/notifier.h>
#include <linux/ptrace.h>
#include <linux/atomic.h>
#include <linux/string.h>

#include "internal.h"

#ifdef CONFIG_X86_64
#include <linux/kdebug.h>
#endif
#ifdef CONFIG_ARM64
#include <asm/debug-monitors.h>
#include <asm/insn-def.h>
#define IKASLR_BRK_IMM		0x462
#define IKASLR_BRK_INSN		(AARCH64_BREAK_MON | (IKASLR_BRK_IMM << 5))
#endif

static atomic_long_t ikaslr_fixups;
static atomic_long_t ikaslr_fixup_fails;

void ikaslr_fixup_stats(unsigned long *ok, unsigned long *fail)
{
	*ok = atomic_long_read(&ikaslr_fixups);
	*fail = atomic_long_read(&ikaslr_fixup_fails);
}

/*
 * 用陷阱指令填满 [base, base+len)。每架构用其软件断点的字节/字模式。
 * 退役变体与被探测函数都经此填充。
 */
void ikaslr_fill_traps(void *base, size_t len)
{
#ifdef CONFIG_X86_64
	memset(base, 0xcc, len);		/* int3 */
#elif defined(CONFIG_ARM64)
	u32 *p = base;
	size_t n = len / 4, i;

	for (i = 0; i < n; i++)
		p[i] = IKASLR_BRK_INSN;		/* brk #IKASLR_BRK_IMM */
	/* 尾部不足 4 字节的部分填 0（udf #0，仍会陷入），正常函数按 4 对齐少见。*/
	memset((u8 *)base + n * 4, 0, len - n * 4);
#else
	memset(base, 0, len);
#endif
}

/*
 * 核心：给定触发陷阱的地址，解析出应重定向到的目标并改写 regs 的 PC。
 * 先看是不是落在被探测函数的陷阱区（第 4 章审计），再看是不是落在退役变体里的
 * 陈旧返回。返回 true 表示已处理。
 */
static bool ikaslr_do_fixup(struct pt_regs *regs, unsigned long faulting)
{
	unsigned long target;

	if (ikaslr_detect_audit(faulting, &target))
		goto redirect;
	if (ikaslr_fixup_addr(faulting, &target))
		goto redirect;
	return false;

redirect:
	atomic_long_inc(&ikaslr_fixups);
#ifdef CONFIG_X86_64
	regs->ip = target;
#elif defined(CONFIG_ARM64)
	regs->pc = target;
#endif
	return true;
}

#ifdef CONFIG_X86_64
static int ikaslr_die_notify(struct notifier_block *nb, unsigned long val,
			     void *data)
{
	struct die_args *args = data;
	unsigned long faulting;

	if (val != DIE_INT3 || !args || !args->regs)
		return NOTIFY_DONE;
	/* int3 是陷阱：异常时 RIP 已越过它一字节，触发地址是 ip-1。*/
	faulting = args->regs->ip - 1;
	return ikaslr_do_fixup(args->regs, faulting) ? NOTIFY_STOP : NOTIFY_DONE;
}

static struct notifier_block ikaslr_die_nb = {
	.notifier_call = ikaslr_die_notify,
	.priority = 0x7fffffff,
};

int __init ikaslr_fixup_init(void)
{
	int ret = register_die_notifier(&ikaslr_die_nb);

	if (ret)
		pr_err("failed to register die notifier: %d\n", ret);
	else
		pr_info("stale-return fixup armed (int3)\n");
	return ret;
}

#elif defined(CONFIG_ARM64)
static int ikaslr_brk_fn(struct pt_regs *regs, unsigned long esr)
{
	/* BRK：PC 指向 brk 指令本身。重定向即改 PC 到目标，返回 HANDLED 继续。*/
	if (ikaslr_do_fixup(regs, regs->pc))
		return DBG_HOOK_HANDLED;
	return DBG_HOOK_ERROR;
}

static struct break_hook ikaslr_break_hook = {
	.fn = ikaslr_brk_fn,
	.imm = IKASLR_BRK_IMM,
};

int __init ikaslr_fixup_init(void)
{
	register_kernel_break_hook(&ikaslr_break_hook);
	pr_info("stale-return fixup armed (brk #%#x)\n", IKASLR_BRK_IMM);
	return 0;
}
#else
int __init ikaslr_fixup_init(void) { return 0; }
#endif
