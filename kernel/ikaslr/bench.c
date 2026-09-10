// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 微观开销测量（论文 §3.6.3）。仅 CONFIG_IKASLR_DEBUG。
 *
 * §3.6.3 要求把跳板引入的额外开销拆成三部分：跳板本身的间接转移、
 * enter/leave 计数、以及白名单查询。本文件用五档配置把它们分离出来：
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
static noinline int ikaslr_bench_plain(int a, int b);

/*
 * 把基线函数登记进白名单。两个用处：
 *   · 档 D 才测得到白名单**命中**路径——fixed_out 实际走的就是命中，
 *     而未登记的目标只会走"扫完桶返回 false"，那是误报路径不是热路径；
 *   · 档 E 需要一个合法的跨区域目标，否则每次迭代都会打一条告警。
 */
IKASLR_WHITELIST(ikaslr_bench_plain);

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

/* 档 D：单次白名单查询。hit=true 测命中（fixed_out 的实际路径），否则测未命中。*/
static u64 bench_whitelist(bool hit)
{
	void *t = hit ? (void *)ikaslr_bench_plain : (void *)&ikaslr_bench_sink;
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

/*
 * 档 E：完整的 fixed_out 序列——离开随机化区域的全部代价。
 *
 *     ikaslr_out_enter(target);   // 白名单查询 + 减计数
 *     target(...);                // 经绝对地址的间接转移
 *     ikaslr_out_leave();         // 加计数
 *
 * 外层先 ikaslr_enter() 再 ikaslr_leave()，两个理由：
 *   · fixed_out 在真实代码里**只可能**从区域内部执行，口径要一致；
 *   · 否则 out_enter 会把活跃计数减成负数，与并发的随机化线程互相干扰
 *     （ikaslr_wait_region_empty 判的是"是否等于零"）。
 *
 * E − B 即"白名单查询 + 一对进出计数"的净开销；E − D 去掉白名单后即计数部分。
 */
static u64 bench_fixed_out(void)
{
	int (*fn)(int, int) = ikaslr_bench_plain;
	void *t = (void *)ikaslr_bench_plain;
	u64 t0;
	int i, s = 0;

	ikaslr_enter();
	for (i = 0; i < 1000; i++) {
		ikaslr_out_enter(t);
		s += fn(ikaslr_bench_sink, 1);
		ikaslr_out_leave();
	}
	t0 = ktime_get_ns();
	for (i = 0; i < IKASLR_BENCH_ITERS; i++) {
		ikaslr_out_enter(t);
		s += fn(ikaslr_bench_sink, 1);
		ikaslr_out_leave();
	}
	t0 = ktime_get_ns() - t0;
	ikaslr_leave();
	ikaslr_bench_sink = s;
	return t0;
}

static int ikaslr_bench_show(struct seq_file *m, void *v)
{
	u64 a, b, c, dh, dm, e;
	unsigned long flags;

	/* 关抢占与中断，避免调度与中断混入测量。*/
	preempt_disable();
	local_irq_save(flags);
	a  = bench_direct();
	b  = bench_indirect();
	c  = bench_trampoline();
	dh = bench_whitelist(true);
	dm = bench_whitelist(false);
	e  = bench_fixed_out();
	local_irq_restore(flags);
	preempt_enable();

	/*
	 * 一律以**皮秒**报每次迭代的耗时。这些量在十纳秒量级，按整数纳秒相除会把
	 * 有效数字全部丢掉（例如 10 ns 与 10.9 ns 都报成 10），而开销模型要拿它们
	 * 相减，误差会被放大。
	 */
#define PER_ITER_PS(x)	((x) * 1000ull / IKASLR_BENCH_ITERS)
#define DIFF_PS(x, y)	(((long long)(x) - (long long)(y)) * 1000ll / \
			 IKASLR_BENCH_ITERS)

	seq_printf(m, "stats_enabled        %d\n",
		   IS_ENABLED(CONFIG_IKASLR_STATS));
	seq_printf(m, "iterations           %d\n", IKASLR_BENCH_ITERS);
	seq_puts(m, "\n# 各档每次迭代耗时（皮秒）\n");
	seq_printf(m, "A_direct_ps          %llu\n", PER_ITER_PS(a));
	seq_printf(m, "B_indirect_ps        %llu\n", PER_ITER_PS(b));
	seq_printf(m, "C_fixed_in_ps        %llu\n", PER_ITER_PS(c));
	seq_printf(m, "D_whitelist_hit_ps   %llu\n", PER_ITER_PS(dh));
	seq_printf(m, "D_whitelist_miss_ps  %llu\n", PER_ITER_PS(dm));
	seq_printf(m, "E_fixed_out_ps       %llu\n", PER_ITER_PS(e));

	seq_puts(m, "\n# 分解（皮秒／次）\n");
	seq_printf(m, "indirect_transfer_ps %lld\n", DIFF_PS(b, a));
	seq_printf(m, "enter_leave_count_ps %lld\n", DIFF_PS(c, b));
	seq_printf(m, "fixed_in_total_ps    %lld\n", DIFF_PS(c, a));
	/*
	 * fixed_out 的净开销以「档 B（同样的间接转移，但不含任何记账）」为基线，
	 * 这样减出来的就是 out_enter+out_leave 本身：白名单查询 + 一对进出计数。
	 */
	seq_printf(m, "fixed_out_total_ps   %lld\n", DIFF_PS(e, b));
	seq_printf(m, "out_count_only_ps    %lld\n", DIFF_PS(e, b) -
		   (long long)PER_ITER_PS(dh));

	seq_puts(m, "\n# 原始总耗时（纳秒）\n");
	seq_printf(m, "A_total_ns           %llu\n", a);
	seq_printf(m, "B_total_ns           %llu\n", b);
	seq_printf(m, "C_total_ns           %llu\n", c);
	seq_printf(m, "D_hit_total_ns       %llu\n", dh);
	seq_printf(m, "D_miss_total_ns      %llu\n", dm);
	seq_printf(m, "E_total_ns           %llu\n", e);
#undef PER_ITER_PS
#undef DIFF_PS
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
