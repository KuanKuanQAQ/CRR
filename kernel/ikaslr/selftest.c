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

#include <linux/preempt.h>

#include "internal.h"
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

/* S1.2：跳板派发 —— 调用是否经跳板到达函数体且结果正确。*/
static int __init test_dispatch(void)
{
	int a = ikaslr_st_add(40, 2);
	int m = ikaslr_st_mul(6, 7);

	pr_info("dispatch: add(40,2)=%d mul(6,7)=%d nr_funcs=%d\n",
		a, m, ikaslr_nr_funcs());
	if (a != 42 || m != 42) {
		pr_err("FAIL(dispatch): wrong result through trampoline\n");
		return -EINVAL;
	}
	return 0;
}

/*
 * S1.3：线程追踪 —— 进出计数必须配平。跳板在返回前调用 ikaslr_leave()，
 * 因此调用结束后活跃计数必须回到调用前的值。
 */
static int __init test_tracking_balance(void)
{
	int before = ikaslr_active_count();
	int after;

	ikaslr_st_add(1, 1);
	ikaslr_st_mul(2, 3);
	after = ikaslr_active_count();

	pr_info("tracking: active before=%d after=%d\n", before, after);
	if (before != 0 || after != 0) {
		pr_err("FAIL(tracking): enter/leave not balanced (%d -> %d)\n",
		       before, after);
		return -EINVAL;
	}
	return 0;
}

/*
 * S1.3：阻断/等待协议 —— 区域为空时 wait_region_empty 应立即成功；
 * 阻断期间新的进入会被挡住，放行后恢复正常。
 */
static int __init test_block_protocol(void)
{
	struct ikaslr_stats s0, s1;
	int ret;

	ikaslr_get_stats(&s0);

	ikaslr_block_region();
	ret = ikaslr_wait_region_empty(100);
	if (ret) {
		ikaslr_unblock_region();
		pr_err("FAIL(block): region not empty while idle: %d\n", ret);
		return ret;
	}
	/* 此刻正是"切换代码页安全"的时刻（S1.4 将在此迁移函数体）。*/
	ikaslr_unblock_region();

	/* 放行后调用应恢复正常。*/
	if (ikaslr_st_add(20, 22) != 42) {
		pr_err("FAIL(block): call broken after unblock\n");
		return -EINVAL;
	}
	ikaslr_get_stats(&s1);
	pr_info("block: wait_empty ok; enters %lu->%lu backoffs=%lu max_active=%d\n",
		s0.enters, s1.enters, s1.backoffs, s1.max_active);
	return 0;
}

/*
 * S1.4：无副本随机化 —— 迁移函数体后，调用应仍然正确，且地址必须真的变了。
 * 这是需求 D1 的端到端验证：调用者一行未改，只更新了 target 槽。
 */
static int __init test_rerandomize(void)
{
	void *a_before, *m_before, *a_after, *m_after;
	u64 ns; unsigned long rounds;
	int ret;

	u64 prep_ns; unsigned long missed; int nready;

	a_before = READ_ONCE(*ikaslr_tbl[0]->target);
	m_before = READ_ONCE(*ikaslr_tbl[1]->target);

	ret = ikaslr_rerandomize();
	if (ret) {
		pr_err("FAIL(rerand): rerandomize returned %d\n", ret);
		return ret;
	}

	a_after = READ_ONCE(*ikaslr_tbl[0]->target);
	m_after = READ_ONCE(*ikaslr_tbl[1]->target);
	ikaslr_rand_stats(&ns, &rounds);
	ikaslr_pool_stats(&prep_ns, &missed, &nready);
	pr_info("pool: prep=%llu ns, missed=%lu, ready=%d\n", prep_ns, missed, nready);

	pr_info("rerand: %s %px -> %px, %s %px -> %px (critical path %llu ns, round %lu)\n",
		ikaslr_tbl[0]->name, a_before, a_after,
		ikaslr_tbl[1]->name, m_before, m_after, ns, rounds);

	if (a_after == a_before || m_after == m_before) {
		pr_err("FAIL(rerand): body address did not change\n");
		return -EINVAL;
	}
	/* 迁移后经同一个跳板调用，结果必须不变。*/
	if (ikaslr_st_add(40, 2) != 42 || ikaslr_st_mul(6, 7) != 42) {
		pr_err("FAIL(rerand): wrong result after relocation\n");
		return -EINVAL;
	}
	/* 再来一轮，验证可反复随机化（含变体回收）。补充变体是异步的，先等它就绪。*/
	ikaslr_defer_flush();
	ret = ikaslr_rerandomize();
	if (ret) {
		pr_err("FAIL(rerand): second round returned %d\n", ret);
		return ret;
	}
	if (ikaslr_st_add(1, 41) != 42 || ikaslr_st_mul(21, 2) != 42) {
		pr_err("FAIL(rerand): wrong result after second round\n");
		return -EINVAL;
	}
	pr_info("rerand: second round ok, now at %px / %px\n",
		READ_ONCE(*ikaslr_tbl[0]->target),
		READ_ONCE(*ikaslr_tbl[1]->target));

	/*
	 * 无副本性质（§3.6.5）：迁移后不应再存在旧代码的可执行副本。
	 * 当前 target 必须已离开内核映像的 .rand.text 区间。
	 */
	for (ret = 0; ret < ikaslr_nr_funcs(); ret++) {
		void *cur = READ_ONCE(*ikaslr_tbl[ret]->target);

		if ((char *)cur >= __rand_text_start &&
		    (char *)cur < __rand_text_end) {
			pr_err("FAIL(no-copy): %s still inside image .rand.text\n",
			       ikaslr_tbl[ret]->name);
			return -EINVAL;
		}
	}
	pr_info("no-copy: all bodies left the image region\n");
	return 0;
}

/*
 * S1.5：触发入口与推迟策略。
 *
 * 关键路径已是零分配、不睡眠，因此原子上下文中**区域为空时就地完成**；
 * 只有区域非空（否则要自旋等待、拉长中断延迟）才推迟到进程上下文。
 */
static int __init test_deferred(void)
{
	unsigned long c0, c1;
	u64 avg, max;
	void *before, *after;

	/* (a) 原子上下文 + 区域为空 -> 应就地完成，不计入推迟。*/
	ikaslr_defer_flush();
	ikaslr_defer_stats(&c0, &avg, &max);
	before = READ_ONCE(*ikaslr_tbl[0]->target);

	preempt_disable();
	ikaslr_request_rerandomize();
	preempt_enable();

	ikaslr_defer_stats(&c1, &avg, &max);
	after = READ_ONCE(*ikaslr_tbl[0]->target);
	if (c1 != c0) {
		pr_err("FAIL(defer): empty region in atomic ctx should run inline\n");
		return -EINVAL;
	}
	if (after == before) {
		pr_err("FAIL(defer): inline atomic randomization did not happen\n");
		return -EINVAL;
	}
	pr_info("defer: atomic ctx + empty region -> inline (%px -> %px)\n",
		before, after);

	/* (b) 原子上下文 + 区域非空 -> 应推迟。用 enter() 制造一个"区域内执行流"。*/
	ikaslr_defer_flush();
	ikaslr_defer_stats(&c0, &avg, &max);
	before = READ_ONCE(*ikaslr_tbl[0]->target);

	ikaslr_enter();			/* 假装有执行流停留在区域内 */
	preempt_disable();
	ikaslr_request_rerandomize();
	preempt_enable();
	ikaslr_defer_stats(&c1, &avg, &max);
	ikaslr_leave();			/* 执行流离开，推迟的那次即可完成 */

	if (c1 != c0 + 1) {
		pr_err("FAIL(defer): non-empty region in atomic ctx should defer (%lu->%lu)\n",
		       c0, c1);
		return -EINVAL;
	}

	ikaslr_defer_flush();
	after = READ_ONCE(*ikaslr_tbl[0]->target);
	ikaslr_defer_stats(&c1, &avg, &max);
	pr_info("defer: deferred %lu time(s), window avg=%llu ns max=%llu ns\n",
		c1, avg, max);
	if (after == before) {
		pr_err("FAIL(defer): deferred randomization did not run\n");
		return -EINVAL;
	}
	if (ikaslr_st_add(40, 2) != 42 || ikaslr_st_mul(6, 7) != 42) {
		pr_err("FAIL(defer): wrong result after deferred round\n");
		return -EINVAL;
	}
	return 0;
}

/* 一个"外部函数"，代表随机化代码要调用的非随机化区域目标。*/
static noinline int ikaslr_st_external(int x)
{
	return x + 1;
}
IKASLR_WHITELIST(ikaslr_st_external);

/*
 * S1.8：fixed_out 与白名单。
 *
 * 这里直接测机制本身，而不是从一个被随机化的函数体里发起跨区域调用——因为在
 * 编译器插件就绪前，含外部调用的函数体不是位置无关代码，无法被迁移（见
 * 03-randomization.md）。等 S1.10a/b 之后再把两者串起来。
 */
static int __init test_fixed_out(void)
{
	int inside_before, inside_mid, inside_after;
	int r;

	if (!ikaslr_whitelist_ok((void *)ikaslr_st_external)) {
		pr_err("FAIL(whitelist): registered target rejected\n");
		return -EINVAL;
	}
	if (ikaslr_whitelist_ok((void *)&ikaslr_st_external + 0x12345)) {
		pr_err("FAIL(whitelist): unlisted target accepted\n");
		return -EINVAL;
	}

	inside_before = ikaslr_inside_count();
	ikaslr_out_enter((void *)ikaslr_st_external);
	inside_mid = ikaslr_inside_count();
	r = ikaslr_st_external(41);
	ikaslr_out_leave();
	inside_after = ikaslr_inside_count();

	pr_info("fixed_out: whitelist=%d entries, inside %d->%d->%d, call=%d, rejects=%lu\n",
		ikaslr_whitelist_count(), inside_before, inside_mid,
		inside_after, r, ikaslr_whitelist_rejects());

	if (inside_mid != inside_before - 1 || inside_after != inside_before) {
		pr_err("FAIL(fixed_out): inside accounting wrong\n");
		return -EINVAL;
	}
	if (r != 42) {
		pr_err("FAIL(fixed_out): external call returned %d\n", r);
		return -EINVAL;
	}
	return 0;
}

/*
 * S1.11：陈旧返回地址的兜底（方案 B）。
 *
 * 直接构造最关键的场景：记下某函数体的当前地址，随机化一轮使该变体退役并被
 * 填充为陷阱指令，然后**按旧地址调用它**。若兜底机制正确，执行会在旧地址上
 * 触发陷阱、被改写 PC 到新变体的对应位置，并返回正确结果——这正是"外部调用
 * 返回到已迁移函数"时发生的事。
 */
static int __init test_stale_return(void)
{
	int (*stale)(int, int);
	unsigned long ok0, fail0, ok1, fail1;
	void *old, *new;
	int r;

	ikaslr_defer_flush();
	old = READ_ONCE(*ikaslr_tbl[0]->target);	/* ikaslr_st_add 当前地址 */
	ikaslr_fixup_stats(&ok0, &fail0);

	if (ikaslr_rerandomize()) {
		pr_err("FAIL(stale): rerandomize failed\n");
		return -EINVAL;
	}
	/* 等待退役变体被填充陷阱（在关键路径之外异步完成）。*/
	ikaslr_defer_flush();

	new = READ_ONCE(*ikaslr_tbl[0]->target);
	if (new == old) {
		pr_err("FAIL(stale): body did not move\n");
		return -EINVAL;
	}

	/* 按旧地址调用：应触发陷阱并被重定向到新地址。*/
	stale = (int (*)(int, int))old;
	r = stale(40, 2);

	ikaslr_fixup_stats(&ok1, &fail1);
	pr_info("stale: called retired %px -> got %d (fixups %lu->%lu, fails %lu->%lu, now %px)\n",
		old, r, ok0, ok1, fail0, fail1, new);

	if (ok1 <= ok0) {
		pr_err("FAIL(stale): no fixup was performed\n");
		return -EINVAL;
	}
	if (r != 42) {
		pr_err("FAIL(stale): redirected call returned %d\n", r);
		return -EINVAL;
	}
	return 0;
}

static int __init ikaslr_selftest_init(void)
{
	int ret;

	ret = test_dispatch();
	if (ret)
		return ret;
	ret = test_tracking_balance();
	if (ret)
		return ret;
	ret = test_block_protocol();
	if (ret)
		return ret;
	ret = test_rerandomize();
	if (ret)
		return ret;
	ret = test_deferred();
	if (ret)
		return ret;
	ret = test_fixed_out();
	if (ret)
		return ret;
	ret = test_stale_return();
	if (ret)
		return ret;

	pr_info("PASS: dispatch + tracking + block + rerand + defer + fixed_out + stale-return\n");
	return 0;
}
/* 在 core 的 late_initcall 之后运行，确保 target 槽已初始化。*/
late_initcall_sync(ikaslr_selftest_init);
