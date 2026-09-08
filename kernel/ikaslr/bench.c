// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 微观开销测量（论文 §3.6.3）。仅 CONFIG_IKASLR_DEBUG。
 *
 * §3.6.3 要求把跳板引入的额外开销拆成三部分：跳板本身的间接转移、
 * enter/leave 计数、以及白名单查询。本文件用四档配置把它们分离出来：
 *
 *   A 直接调用       —— 未被随机化的等价函数，静态直接调用（基线）
 *   B 仅间接转移     —— 经 target 槽间接调用函数体，不含计数
 *   C 完整跳板       —— fixed_in：计数 + 经 target 间接转移
 *   D 白名单查询     —— 单次 ikaslr_whitelist_ok() 的代价
 *
 *   间接转移开销 = B - A
 *   计数开销     = C - B
 *   白名单开销   = D
 *
 * 读 /proc/ikaslr/bench 即运行一轮并返回结果。
 *
 * 测量口径：每档跑 IKASLR_BENCH_ITERS 次取总耗时，先做预热消除冷缓存影响；
 * 关中断以避免被抢占/中断干扰。结果为每次调用的平均纳秒数。
 */
#define pr_fmt(fmt) "ikaslr: " fmt

#include <linux/ikaslr.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/irqflags.h>
#include <linux/preempt.h>
#include <linux/init.h>

#include "internal.h"

#define IKASLR_BENCH_ITERS	200000

/*
 * 结果必须被真正消费，否则编译器会把整个测量循环优化掉。
 * 首次实现时基线档测出 200000 次共 119 ns —— 循环已被删除，数字无意义。
 */
static volatile int ikaslr_bench_sink;

/* 基线：与被随机化函数体等价、但不参与随机化的函数。*/
static noinline int ikaslr_bench_plain(int a, int b)
{
	int i, acc = a;

	for (i = 0; i < b; i++)
		acc += 1;
	return acc;
}

/* 被随机化的自测函数（selftest.c 中定义）。*/
int ikaslr_st_add(int a, int b);

static u64 bench_direct(void)
{
	u64 t0;
	int i, s = 0;

	for (i = 0; i < 1000; i++)		/* 预热 */
		s += ikaslr_bench_plain(ikaslr_bench_sink, 1);
	t0 = ktime_get_ns();
	for (i = 0; i < IKASLR_BENCH_ITERS; i++)
		s += ikaslr_bench_plain(ikaslr_bench_sink, 1);
	ikaslr_bench_sink = s;
	return ktime_get_ns() - t0;
}

static u64 bench_indirect(void)
{
	int (*fn)(int, int);
	u64 t0;
	int i, s = 0;

	if (!ikaslr_nr_funcs())
		return 0;
	for (i = 0; i < 1000; i++) {
		fn = (int (*)(int, int))READ_ONCE(*ikaslr_tbl[0]->target);
		s += fn(ikaslr_bench_sink, 1);
	}
	t0 = ktime_get_ns();
	for (i = 0; i < IKASLR_BENCH_ITERS; i++) {
		/* 每次都重读 target，与跳板中的 volatile load 口径一致。*/
		fn = (int (*)(int, int))READ_ONCE(*ikaslr_tbl[0]->target);
		s += fn(ikaslr_bench_sink, 1);
	}
	ikaslr_bench_sink = s;
	return ktime_get_ns() - t0;
}

static u64 bench_trampoline(void)
{
	u64 t0;
	int i, s = 0;

	for (i = 0; i < 1000; i++)
		s += ikaslr_st_add(ikaslr_bench_sink, 1);
	t0 = ktime_get_ns();
	for (i = 0; i < IKASLR_BENCH_ITERS; i++)
		s += ikaslr_st_add(ikaslr_bench_sink, 1);
	ikaslr_bench_sink = s;
	return ktime_get_ns() - t0;
}

static u64 bench_whitelist(void)
{
	void *t = (void *)ikaslr_bench_plain;
	u64 t0;
	int i;
	bool r = false;

	for (i = 0; i < 1000; i++)
		r |= ikaslr_whitelist_ok(t);
	t0 = ktime_get_ns();
	for (i = 0; i < IKASLR_BENCH_ITERS; i++)
		r |= ikaslr_whitelist_ok(t);
	ikaslr_bench_sink = r;
	return ktime_get_ns() - t0;
}

static int ikaslr_bench_show(struct seq_file *m, void *v)
{
	u64 a, b, c, d;
	unsigned long flags;

	/* 关抢占与中断，避免调度与中断混入测量。*/
	preempt_disable();
	local_irq_save(flags);
	a = bench_direct();
	b = bench_indirect();
	c = bench_trampoline();
	d = bench_whitelist();
	local_irq_restore(flags);
	preempt_enable();

	seq_printf(m, "stats_enabled        %d\n",
		   IS_ENABLED(CONFIG_IKASLR_STATS));
	seq_printf(m, "iterations           %d\n", IKASLR_BENCH_ITERS);
	seq_printf(m, "A_direct_ns          %llu\n", a / IKASLR_BENCH_ITERS);
	seq_printf(m, "B_indirect_ns        %llu\n", b / IKASLR_BENCH_ITERS);
	seq_printf(m, "C_trampoline_ns      %llu\n", c / IKASLR_BENCH_ITERS);
	seq_printf(m, "D_whitelist_ns       %llu\n", d / IKASLR_BENCH_ITERS);
	seq_puts(m, "\n");
	seq_printf(m, "indirect_overhead_ns %lld\n",
		   (long long)(b - a) / IKASLR_BENCH_ITERS);
	seq_printf(m, "counting_overhead_ns %lld\n",
		   (long long)(c - b) / IKASLR_BENCH_ITERS);
	seq_printf(m, "trampoline_total_ns  %lld\n",
		   (long long)(c - a) / IKASLR_BENCH_ITERS);
	seq_puts(m, "\n");
	seq_printf(m, "A_total_ns           %llu\n", a);
	seq_printf(m, "B_total_ns           %llu\n", b);
	seq_printf(m, "C_total_ns           %llu\n", c);
	seq_printf(m, "D_total_ns           %llu\n", d);
	return 0;
}

static int ikaslr_bench_open(struct inode *i, struct file *f)
{
	return single_open(f, ikaslr_bench_show, NULL);
}

static const struct proc_ops ikaslr_bench_ops = {
	.proc_open	= ikaslr_bench_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

int __init ikaslr_bench_init(struct proc_dir_entry *dir)
{
	if (!proc_create("bench", 0400, dir, &ikaslr_bench_ops))
		return -ENOMEM;
	return 0;
}
