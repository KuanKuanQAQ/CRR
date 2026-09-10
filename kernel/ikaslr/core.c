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
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/preempt.h>
#include <linux/rcupdate.h>
#include <linux/export.h>

#include "internal.h"

struct ikaslr_tramp **ikaslr_tbl;	/* 指针数组，见 ikaslr.h 说明 */
int ikaslr_ntramp;

/*
 * ---- S1.3 精确线程追踪（论文 §3.4.5）----
 *
 * 活跃执行流集合以一个计数实现：控制流经 fixed_in 进入随机化区域时加一，
 * 经跳板返回（fixed_out / 跳板尾部）时减一。计数为零即可确定区域内既没有
 * 执行流在跑，也没有栈帧会返回到区域内——此时切换代码页是安全的，因而不必
 * 保留旧代码副本（Shuffler 式方案必须保留，见 §3.2.2）。
 *
 * 这个信息是"免费"的：跳板本来就必须被经过。
 */
static atomic_t ikaslr_active = ATOMIC_INIT(0);
/* 当前真正在区域内代码中执行的流；只用于观测，不参与安全判定（见下方说明）。*/
static atomic_t ikaslr_inside = ATOMIC_INIT(0);
static bool ikaslr_blocked;			/* 阻断标志：禁止新执行流进入 */
static DECLARE_WAIT_QUEUE_HEAD(ikaslr_zero_wq);	/* 计数归零时唤醒随机化线程 */
static DECLARE_WAIT_QUEUE_HEAD(ikaslr_unblock_wq);/* 放行时唤醒被挡住的执行流 */

static atomic_long_t ikaslr_enters;
static atomic_long_t ikaslr_backoffs;
static atomic_long_t ikaslr_forced;	/* 不可睡上下文里放弃等待、硬进入的次数 */
/* 诊断：out_leave 在阻断期间把计数加回去的次数，以及等空超时时的残余计数。*/
static atomic_long_t ikaslr_outleave_blocked;
/* 阻断期间因"本任务已在区域内"而被放行的嵌套进入次数——旧代码会在这里自我阻塞。*/
static atomic_long_t ikaslr_nested_admitted;
/* 经 fixed_out 离开区域的次数。与 enters 一起构成 E3-A 的 G 项（跨区域调用频度）。*/
static atomic_long_t ikaslr_outs;
static int ikaslr_wait_residual;
static int ikaslr_max_active;

int ikaslr_active_count(void)
{
	return atomic_read(&ikaslr_active);
}

void ikaslr_get_stats(struct ikaslr_stats *out)
{
	out->enters = atomic_long_read(&ikaslr_enters);
	out->backoffs = atomic_long_read(&ikaslr_backoffs);
	out->forced = atomic_long_read(&ikaslr_forced);
	out->outleave_blocked = atomic_long_read(&ikaslr_outleave_blocked);
	out->nested_admitted = atomic_long_read(&ikaslr_nested_admitted);
	out->outs = atomic_long_read(&ikaslr_outs);
	out->wait_residual = READ_ONCE(ikaslr_wait_residual);
	out->max_active = READ_ONCE(ikaslr_max_active);
}

/*
 * 不可睡上下文里放弃等待前的最大重试次数。每次重试是「一次原子加、一次屏障、
 * 一次原子减、一次 cpu_relax」，约几十纳秒，因此这个数量级对应百微秒级的上限——
 * 比正常的阻断窗口（关键路径实测 0.5–2 µs）宽两个数量级，又远短于
 * ikaslr_wait_region_empty() 的超时。
 */
#define IKASLR_SPIN_LIMIT 4096

/* 等待阻断解除。可能在非抢占上下文（中断、持锁）被调用，故分两条路径。*/
static void ikaslr_wait_unblocked(void)
{
	while (READ_ONCE(ikaslr_blocked)) {
		/*
		 * 只有真正可以睡的上下文才允许睡。
		 *
		 * `preemptible()` 单独用是**不够的**：在 CONFIG_PREEMPT_RCU 下
		 * `rcu_read_lock()` 并不关抢占，只是给 rcu_read_lock_nesting 加一，
		 * 因此 RCU 读端临界区里 `preemptible()` 仍为真。而 VFS 的 rcu-walk
		 * 路径查找会在 RCU 读端临界区里调用 dput/inode_permission 这类函数——
		 * 一旦它们被随机化，这里就会在 RCU 读端临界区中睡眠，内核报
		 * "Voluntary context switch within RCU read-side critical section!"
		 * （实测：随机化 VFS 函数后 6 次启动中 2 次触发）。
		 *
		 * 阻断窗口本身只有微秒量级（关键路径实测 0.5–2 µs），所以在不可睡的
		 * 上下文里自旋等待是完全可接受的。
		 */
		if (preemptible() && !rcu_preempt_depth())
			wait_event(ikaslr_unblock_wq, !READ_ONCE(ikaslr_blocked));
		else
			return;		/* 不可睡：由调用方按次数设上限，见 ikaslr_enter */
	}
}

/*
 * 进入随机化区域。
 *
 * 竞态要点：不能"先查阻断标志再加一"——在这两步之间随机化线程可能置位标志并
 * 看到计数为零，于是在本执行流已经进入的情况下开始搬移代码。因此这里先加一、
 * 再查标志；若发现已被阻断就退出并重试。随机化线程置位标志后看到的计数，
 * 必然已经把所有"先加一"的执行流计入。
 */
void ikaslr_enter(void)
{
	int cur, spins = 0;

retry:
	/*
	 * 深度与 active 严格同步递增。下面只有 depth == 1（本任务**新**进入区域）
	 * 才受阻断约束——嵌套进入不能被挡，否则就是自我阻塞，见 task_struct 里
	 * ikaslr_depth 的说明。
	 */
	current->ikaslr_depth++;
	atomic_inc(&ikaslr_active);
	smp_mb();			/* 加一 与 读标志 之间不得重排 */
	if (unlikely(READ_ONCE(ikaslr_blocked))) {
		if (current->ikaslr_depth > 1) {
			/*
			 * 嵌套进入：本任务已经在区域内，随机化方无论如何都得等它出来，
			 * 挡它只会造成自我阻塞（见 task_struct 里 ikaslr_depth 的说明）。
			 */
			if (IS_ENABLED(CONFIG_IKASLR_STATS))
				atomic_long_inc(&ikaslr_nested_admitted);
			goto entered;
		}
		/*
		 * 不可睡的上下文里只能自旋，而**自旋必须有上限**。
		 *
		 * 早先这里是无限自旋，理由是"阻断窗口只有微秒量级"。那个假设在小范围
		 * 下成立，一到真实规模就不成立了：ikaslr_wait_region_empty() 等不到空
		 * 时，阻断窗口就是它的整个超时（100 ms）。
		 *
		 * 而更要命的是死锁：被随机化的函数会出现在**关中断**的路径上——实测
		 * S3 范围里 mm/ 的 first_online_pgdat 被 quiet_vmstat() 经
		 * tick_nohz_stop_tick() 调用。两个 CPU 在这里关着中断无限自旋，于是
		 * 让阻断方超时所需的**时钟中断本身**也被卡住，整机停摆（实测：客户机
		 * 时间冻结在 1.86 s，gdb 显示 CPU0/CPU3 都停在本函数）。
		 *
		 * 因此到达上限就**带着已经加过的计数直接进入**。这样做是安全的：
		 * 计数已经加过，随机化方的"等区域变空"必然看得见，于是它会超时并
		 * 跳过本轮，不会在我们身处区域内时搬移代码。
		 *
		 * 残留窗口只有一个：随机化方**已经**读到计数为零、尚未放行的那一瞬。
		 * 此时硬进入的执行流可能落进即将退役的变体里——这正是陈旧返回地址的
		 * 场景，由退役变体的陷阱指令 + ikaslr_fixup_addr() 兜底（§3.5.5），
		 * 代价是一次陷阱，不是错误。
		 */
		if (++spins > IKASLR_SPIN_LIMIT &&
		    !(preemptible() && !rcu_preempt_depth())) {
			atomic_long_inc(&ikaslr_forced);
			goto entered;
		}
		atomic_long_inc(&ikaslr_backoffs);
		current->ikaslr_depth--;
		if (atomic_dec_and_test(&ikaslr_active) &&
		    wq_has_sleeper(&ikaslr_zero_wq))
			wake_up(&ikaslr_zero_wq);
		ikaslr_wait_unblocked();
		cpu_relax();
		goto retry;
	}
entered:

	/*
	 * 以下全部是统计，不参与机制本身。实测它们在跳板热路径上占了绝大部分
	 * 开销（§3.6.3），因此由独立的 CONFIG_IKASLR_STATS 控制——与 DEBUG 分开，
	 * 才能在开着 /proc/ikaslr/bench 的同时关掉统计来测"生产配置"的开销。
	 * 关掉后 enter 只剩「加一 + 屏障 + 查标志」。
	 */
	if (IS_ENABLED(CONFIG_IKASLR_STATS)) {
		atomic_inc(&ikaslr_inside);
		atomic_long_inc(&ikaslr_enters);
		cur = atomic_read(&ikaslr_active);
		if (cur > READ_ONCE(ikaslr_max_active))
			WRITE_ONCE(ikaslr_max_active, cur);
	}
}
EXPORT_SYMBOL_GPL(ikaslr_enter);

/* 离开随机化区域；归零时唤醒等待中的随机化线程。*/
void ikaslr_leave(void)
{
	if (IS_ENABLED(CONFIG_IKASLR_STATS))
		atomic_dec(&ikaslr_inside);
	/*
	 * 只有真的有人在等才唤醒。
	 *
	 * 无条件 wake_up() 会在每次计数归零时去拿等待队列的锁，而绝大多数时候
	 * 根本没有随机化线程在等——实测这一处就占了跳板开销的大头（关掉统计后
	 * 仍有 151 ns/次，改用 wq_has_sleeper 后见 §3.6.3）。
	 * wq_has_sleeper() 自带所需的屏障，不会漏唤醒。
	 */
	current->ikaslr_depth--;
	if (atomic_dec_and_test(&ikaslr_active) &&
	    wq_has_sleeper(&ikaslr_zero_wq))
		wake_up(&ikaslr_zero_wq);
}
EXPORT_SYMBOL_GPL(ikaslr_leave);

/*
 * ---- fixed_out：离开随机化区域（§3.4.2）----
 *
 * 注意这里对计数的处理，它牵涉一处必须讲清楚的不变式。
 *
 * ikaslr_active 计的是"会返回到随机化区域的执行流"，由 fixed_in 加一、跳板返回
 * 前减一。随机化线程等待的正是它归零——因为 §3.4.5 的安全条件是"既没有执行流在
 * 区域内运行，**也没有任何栈帧会返回到区域内**"。
 *
 * 因此 fixed_out **不减** active：随机化函数 F 经 fixed_out 调用外部函数 G 时，
 * F 的栈帧仍在栈上，其返回地址指向 F 的函数体；若此时把 active 减到零并搬移 F，
 * G 返回后就会跳回已失效的旧地址。
 *
 * fixed_out 改为维护另一个计数 inside（当前真正在区域内代码中执行的流），
 * 它只用于观测与统计，不参与随机化的安全判定。
 *
 * 这一取舍与论文 §3.4.2「fixed_out 执行 pop_thread()」的字面描述不同，
 * 详见 03-randomization.md 中的说明与两种方案的比较。
 */
static atomic_long_t ikaslr_wl_rejects;

int ikaslr_inside_count(void)
{
	return atomic_read(&ikaslr_inside);
}

void ikaslr_out_enter(void *target)
{
	/*
	 * 方案 B（作者 2026-09-09 决策）：离开随机化区域时**减少** active。
	 * 这样随机化不必等到调用外部函数的执行流返回——在真实内核路径上，
	 * 随机化函数阻塞在 I/O 里是常态，若不减计数，区域将长期非空。
	 *
	 * 代价是本执行流的栈帧仍在栈上、返回地址指向旧变体。该返回由退役变体中
	 * 填充的陷阱指令捕获，再由 fixup 把 PC 指向新变体的对应位置
	 * （kernel/ikaslr/fixup.c）。
	 */
	/* 白名单检查：目标必须是编译期登记的合法跨区域目标（§3.5.2）。*/
	if (unlikely(!ikaslr_whitelist_ok(target))) {
		atomic_long_inc(&ikaslr_wl_rejects);
		pr_warn_ratelimited("cross-region call to unlisted target %px\n",
				    target);
		/*
		 * 当前实现只告警不阻断：白名单尚未由编译器全覆盖生成，
		 * 贸然阻断会误杀合法路径。全覆盖之后应改为拒绝并上报
		 * CFI violation（与第 5 章的 PA 验证一并处理）。
		 */
	}
	if (IS_ENABLED(CONFIG_IKASLR_STATS)) {
		atomic_dec(&ikaslr_inside);
		atomic_long_inc(&ikaslr_outs);
	}
	current->ikaslr_depth--;
	if (atomic_dec_and_test(&ikaslr_active) &&
	    wq_has_sleeper(&ikaslr_zero_wq))
		wake_up(&ikaslr_zero_wq);
}
EXPORT_SYMBOL_GPL(ikaslr_out_enter);

/*
 * 从外部函数返回到随机化区域。
 *
 * 这里**不等待**阻断标志：本次返回是回到调用者的栈帧，而非新的进入。若此刻
 * 正在随机化，返回地址会落在已退役的变体上并触发陷阱，由 fixup 重定向——
 * 因此无需（也不能）在此阻塞，否则会与等待计数归零的随机化线程互相等待。
 */
void ikaslr_out_leave(void)
{
	if (IS_ENABLED(CONFIG_IKASLR_STATS) && unlikely(READ_ONCE(ikaslr_blocked)))
		atomic_long_inc(&ikaslr_outleave_blocked);
	current->ikaslr_depth++;
	atomic_inc(&ikaslr_active);
	if (IS_ENABLED(CONFIG_IKASLR_STATS))
		atomic_inc(&ikaslr_inside);
}
EXPORT_SYMBOL_GPL(ikaslr_out_leave);

unsigned long ikaslr_whitelist_rejects(void)
{
	return atomic_long_read(&ikaslr_wl_rejects);
}

void ikaslr_block_region(void)
{
	WRITE_ONCE(ikaslr_blocked, true);
	smp_mb();			/* 置标志 先于 读计数 */
}

void ikaslr_unblock_region(void)
{
	WRITE_ONCE(ikaslr_blocked, false);
	smp_mb();
	wake_up_all(&ikaslr_unblock_wq);
}

/*
 * 等待活跃集合变空。返回 0 表示已空，-ETIMEDOUT 表示超时。
 * 超时是必要的：若某个执行流长时间停留在区域内（例如阻塞在 I/O 上），
 * 随机化应当放弃本次而不是无限期挂住——放弃只损失一次随机化机会，
 * 挂住则会拖垮系统。
 */
int ikaslr_wait_region_empty(unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

	while (atomic_read(&ikaslr_active) != 0) {
		/* 同 ikaslr_wait_unblocked()：RCU 读端临界区里 preemptible() 仍为真，
		 * 不能据此判断可否睡眠。*/
		if (preemptible() && !rcu_preempt_depth()) {
			if (!wait_event_timeout(ikaslr_zero_wq,
						atomic_read(&ikaslr_active) == 0,
						deadline - jiffies))
				break;
		} else {
			if (time_after(jiffies, deadline))
				break;
			cpu_relax();
		}
	}
	{
		int left = atomic_read(&ikaslr_active);

		if (left)
			WRITE_ONCE(ikaslr_wait_residual, left);
		return left == 0 ? 0 : -ETIMEDOUT;
	}
}

int ikaslr_nr_funcs(void)
{
	return ikaslr_ntramp;
}
EXPORT_SYMBOL_GPL(ikaslr_nr_funcs);

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
	ret = ikaslr_whitelist_init();
	if (ret) {
		pr_err("whitelist init failed: %d\n", ret);
		return ret;
	}

	ret = ikaslr_fixup_init();
	if (ret)
		return ret;

	ret = ikaslr_pool_init();
	if (ret) {
		pr_err("variant pool init failed: %d\n", ret);
		return ret;
	}

	ikaslr_control_init();		/* 接口缺失不应阻止机制本身工作 */

	pr_info("registered %d randomizable function(s), rand region %lu B\n",
		ikaslr_ntramp, rand_sz);
	return 0;
}
late_initcall(ikaslr_init);
