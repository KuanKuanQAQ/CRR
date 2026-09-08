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

	pr_info("rerand: %s %px -> %px, %s %px -> %px (%llu ns, round %lu)\n",
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
	/* 再来一轮，验证可反复随机化（含回收上一份）。*/
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

	pr_info("PASS: dispatch + tracking + block protocol + rerandomization\n");
	return 0;
}
/* 在 core 的 late_initcall 之后运行，确保 target 槽已初始化。*/
late_initcall_sync(ikaslr_selftest_init);
