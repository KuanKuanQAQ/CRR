// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 持续随机化执行机制——运行时核心（论文第 3 章）。
 *
 * S1.1 建立子系统骨架与区域发现；S1.2 建立跳板表与 target 槽的运行时管理，
 * 即需求 D1（O(1) 索引更新）的落地：一次随机化只需为每个被迁移的函数改写一个
 * target 槽，与该函数有多少调用点无关。
 *
 * 后续步骤：
 *   S1.3 精确线程追踪（ikaslr_enter/leave 的真实实现）
 *   S1.4 无副本随机化（迁移函数体 + 更新 target + 切页权限）
 *   S1.5 非抢占上下文的推迟随机化
 *   S1.6 函数指针语义分离   S1.7 共享 GOT   S1.8 白名单   S1.9 控制接口
 */
#define pr_fmt(fmt) "ikaslr: " fmt

#include <linux/ikaslr.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/sort.h>

static struct ikaslr_tramp **ikaslr_tbl;	/* 指针数组，见 ikaslr.h 说明 */
static int ikaslr_ntramp;

int ikaslr_nr_funcs(void)
{
	return ikaslr_ntramp;
}
EXPORT_SYMBOL_GPL(ikaslr_nr_funcs);

/* ---- S1.3 线程追踪：此处仍为桩，真实实现在下一步 ---- */
void ikaslr_enter(void) { }
void ikaslr_leave(void) { }

/*
 * 更新一个函数的 target 槽 —— 随机化时"唯一需要更新的索引"。
 * 之所以是 O(1)：调用者始终调用固定地址的跳板，跳板经 target 间接转移，
 * 因此函数体搬到哪里都只影响这一个槽，与调用点数量无关（论文 §3.4.2）。
 */
static void ikaslr_set_target(struct ikaslr_tramp *t, void *addr)
{
	WRITE_ONCE(*t->target, addr);
}

/* 供 S1.4 使用：把全部 target 指回给定布局。此处先提供按项更新的原语。*/
void ikaslr_update_target(struct ikaslr_tramp *t, void *new_body)
{
	ikaslr_set_target(t, new_body);
}

static int cmp_by_body(const void *a, const void *b)
{
	unsigned long x = (unsigned long)(*(struct ikaslr_tramp * const *)a)->body;
	unsigned long y = (unsigned long)(*(struct ikaslr_tramp * const *)b)->body;

	return (x > y) - (x < y);
}

/*
 * 初始化跳板表：按函数体地址排序后，用相邻项之差得出每个函数体的大小
 * （最后一项以 .rand.text 末尾定界），并把每个 target 槽初始化为函数体的
 * 链接期地址。排序是必要的——表项的链接顺序未必与代码的链接顺序一致。
 */
static int __init ikaslr_build_table(void)
{
	int i;

	unsigned long bytes = (unsigned long)__end_ikaslr_tramp_tbl -
			      (unsigned long)__start_ikaslr_tramp_tbl;

	ikaslr_tbl = __start_ikaslr_tramp_tbl;
	ikaslr_ntramp = __end_ikaslr_tramp_tbl - __start_ikaslr_tramp_tbl;
	if (!ikaslr_ntramp)
		return 0;

	/*
	 * 跨度自检：表项必须紧密排列，否则按 sizeof 索引会落进填充区。
	 * 曾经踩到过——编译器把表项对齐到 32 字节而结构只有 40 字节，
	 * 第二项的实际位置比 tbl[1] 靠后 24 字节。
	 */
	if (bytes != (unsigned long)ikaslr_ntramp * sizeof(*ikaslr_tbl)) {
		pr_err("trampoline table stride mismatch: %lu bytes for %d entries of %zu\n",
		       bytes, ikaslr_ntramp, sizeof(*ikaslr_tbl));
		return -EINVAL;
	}

	sort(ikaslr_tbl, ikaslr_ntramp, sizeof(*ikaslr_tbl), cmp_by_body, NULL);

	for (i = 0; i < ikaslr_ntramp; i++) {
		struct ikaslr_tramp *t = ikaslr_tbl[i];
		unsigned long end;

		if (i < ikaslr_ntramp - 1)
			end = (unsigned long)ikaslr_tbl[i + 1]->body;
		else
			end = (unsigned long)__rand_text_end;

		if (end <= (unsigned long)t->body) {
			pr_err("%s: bad layout (body=%px end=%lx)\n",
			       t->name, t->body, end);
			return -EINVAL;
		}
		t->size = end - (unsigned long)t->body;
		ikaslr_set_target(t, t->body);

		if (IS_ENABLED(CONFIG_IKASLR_DEBUG))
			pr_info("  [%d] %-24s tramp=%px body=%px size=%zu target=%px\n",
				i, t->name, t->tramp, t->body, t->size, t->target);
	}
	return 0;
}

static int __init ikaslr_init(void)
{
	unsigned long rand_sz = __rand_text_end - __rand_text_start;
	unsigned long tramp_sz = __tramp_text_end - __tramp_text_start;
	int ret;

	pr_info("regions: .rand.text=[%px,%px) %lu B, .tramp.text=[%px,%px) %lu B\n",
		__rand_text_start, __rand_text_end, rand_sz,
		__tramp_text_start, __tramp_text_end, tramp_sz);

	ret = ikaslr_build_table();
	if (ret) {
		pr_err("init failed: %d\n", ret);
		return ret;
	}
	if (!ikaslr_ntramp) {
		pr_info("no randomizable functions registered "
			"(no annotations / LLVM pass not wired)\n");
		return 0;
	}
	pr_info("registered %d randomizable function(s), rand region %lu B\n",
		ikaslr_ntramp, rand_sz);
	return 0;
}
late_initcall(ikaslr_init);
