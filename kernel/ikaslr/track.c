// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 执行流追踪——arm64 与 x86-64 共用（论文 §3.3.2 / §3.4.5，交接文档 §4.1）。
 * 本文件是与平台无关的那一半（计数存储、判空、阻断慢路径、自测）；热路径的加一/
 * 置标志/查阻断由各平台跳板内联完成（arm64 见 tramp.S，x86 见 tramp_x86.S 与 pass）。
 *
 * 判据「随机化区域内没有执行流」由三样东西合成：
 *
 *   1. 每任务内外标志 thread_info.ikaslr_inside：执行流是否已被计入。区域内部
 *      的调用（标志为 1）不碰计数；EL1 异常入口把标志存进 pt_regs 并清零、返回时
 *      恢复（entry.S），因此中断处理程序的进出单独计数，不会记到被打断的执行流
 *      头上。
 *
 *   2. 每 CPU 一对**只增不减**的计数 enter / exit（SRCU 式）。跨边界进入时
 *      enter++，离开时 exit++；被阻断的瞬态进入用再加一次 exit 撤销。
 *
 *      为什么不是「每 CPU 一个可加可减的计数、随机化线程求和」：执行流会在 A 核
 *      加一、迁移到 B 核后减一；随机化线程按核序扫描时可能读到 A=0（加一之前）
 *      与 B=−1（减一之后），这个 −1 抵消掉另一条真实停留在区域内的执行流的 +1，
 *      总和误读为零——在区域非空时搬走代码。（本文件的自测用一组影子计数器统计
 *      这种误判，见 ikaslr_track_selftest。）
 *
 *      双计数 + **先读 exit 再读 enter** 没有这个问题，论证同
 *      kernel/rcu/srcutree.c 的 srcu_readers_active_idx_check()：
 *        · 计入 X=Σexit 的每次离开，其配对的进入都发生在它之前，因而也发生在
 *          enter 扫描开始之前，必被计入 E=Σenter ——故 E ≥ X，且多出来的部分
 *          恰是「已进入、在 exit 扫描结束时尚未离开」的执行流；
 *        · 未被 E 计入的进入必然发生在置阻断标志（及其后的屏障）之后，该执行流
 *          在自己的屏障之后读阻断标志必为真，于是撤销并等待。
 *      所以 E == X 当且仅当区域内没有已被放行的执行流。计数为 64 位无符号、只比较
 *      相等，回绕不影响正确性。
 *
 *      跳板不关抢占：计数用原子指令（ldxr/stxr）更新，执行流在「取 CPU 号」与
 *      「加一」之间被迁移也只是把这一次记在原来那个核上——求和不变，只损失一次
 *      缓存局部性。这与 SRCU 的 NMI 安全读端（raw_cpu_ptr + atomic_long_inc）
 *      是同一做法。
 *
 *   3. 阻断标志 ikaslr_blocked。进入一侧「先加一、屏障、再查标志」，随机化一侧
 *      「先置标志、屏障、再读计数」（Dekker）。
 *
 * 热路径（加一、置标志、查阻断）全部由汇编跳板内联完成，只用 x16/x17 与
 * x9/x10：每函数入口跳板与每目标出口跳板由 LLVM pass 发出
 * （tools/ikaslr/llvm/IKaslrPass.cpp），共享例程在 tramp.S。本文件只有：
 * 计数与标志的存储、随机化一侧的判空、以及被阻断时的慢路径。
 *
 * CONFIG_IKASLR_COUNT_GLOBAL 让所有核共用 ikaslr_cnt[0]——这是「全局共享计数」
 * 的对照实现，仅用于多核扩展性对比实验（交接文档 §4.1 配套实验）。
 */
#define pr_fmt(fmt) "ikaslr: " fmt

#include <linux/ikaslr.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/cache.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/preempt.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/wait.h>
#include <linux/export.h>
#include <linux/kthread.h>
#include <linux/slab.h>

#include "internal.h"

/*
 * 计数存储两套形态：
 *   arm64 —— ikaslr_cnt[NR_CPUS] 数组，跳板按 `ikaslr_cnt + (cpu<<6)` 寻址、用
 *            ldxr/stxr 加一（arm64 percpu 汇编较绕，故用 cpu 索引的数组）。
 *   x86-64 —— 真 percpu 变量 ikaslr_enter_cnt/ikaslr_exit_cnt，跳板用单条
 *            `incq %gs:var` 加一（x86 percpu 一条指令即原子于本核中断）。
 * 两者的判空求和都在下面的 __ikaslr_sum 里，语义一致（先读 exit 再读 enter）。
 * COUNT_GLOBAL 对照实现：所有核共用一处计数（x86 用带 lock 的全局，arm64 用第 0 项）。
 */
#ifdef CONFIG_X86_64
DEFINE_PER_CPU(u64, ikaslr_enter_cnt);
DEFINE_PER_CPU(u64, ikaslr_exit_cnt);
u64 ikaslr_enter_g, ikaslr_exit_g;	/* COUNT_GLOBAL 对照：lock incq 的全局计数 */

static u64 ikaslr_sum_field(bool exit)
{
	u64 v = 0;
	int cpu;

	if (IS_ENABLED(CONFIG_IKASLR_COUNT_GLOBAL))
		return exit ? READ_ONCE(ikaslr_exit_g) : READ_ONCE(ikaslr_enter_g);
	for_each_possible_cpu(cpu)
		v += READ_ONCE(*per_cpu_ptr(exit ? &ikaslr_exit_cnt
					        : &ikaslr_enter_cnt, cpu));
	return v;
}
static inline void ikaslr_bump_enter(void)
{
	if (IS_ENABLED(CONFIG_IKASLR_COUNT_GLOBAL))
		WRITE_ONCE(ikaslr_enter_g, READ_ONCE(ikaslr_enter_g) + 1);
	else
		this_cpu_inc(ikaslr_enter_cnt);
}
static inline void ikaslr_bump_exit(void)
{
	if (IS_ENABLED(CONFIG_IKASLR_COUNT_GLOBAL))
		WRITE_ONCE(ikaslr_exit_g, READ_ONCE(ikaslr_exit_g) + 1);
	else
		this_cpu_inc(ikaslr_exit_cnt);
}
#else
struct ikaslr_cnt {
	atomic64_t enter;
	atomic64_t exit;
} ____cacheline_aligned;

struct ikaslr_cnt ikaslr_cnt[NR_CPUS];
static_assert(sizeof(struct ikaslr_cnt) == 64);
static_assert(offsetof(struct ikaslr_cnt, exit) == 8);

static inline struct ikaslr_cnt *ikaslr_this_cnt(void)
{
	if (IS_ENABLED(CONFIG_IKASLR_COUNT_GLOBAL))
		return &ikaslr_cnt[0];
	/* 只是局部性提示，被迁移也无妨（见文件头），故不关抢占。*/
	return &ikaslr_cnt[raw_smp_processor_id()];
}
static u64 ikaslr_sum_field(bool exit)
{
	u64 v = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		v += atomic64_read(exit ? &ikaslr_cnt[cpu].exit
					: &ikaslr_cnt[cpu].enter);
	return v;
}
static inline void ikaslr_bump_enter(void) { atomic64_inc(&ikaslr_this_cnt()->enter); }
static inline void ikaslr_bump_exit(void)  { atomic64_inc(&ikaslr_this_cnt()->exit); }
#endif

/* 阻断标志。跳板以 32 位读取；只读居多，不与计数同行。*/
u32 ikaslr_blocked __read_mostly;

static DECLARE_WAIT_QUEUE_HEAD(ikaslr_unblock_wq);

static atomic_long_t ikaslr_backoffs;	/* 因阻断而撤销计数并等待的次数 */
static atomic_long_t ikaslr_forced;	/* 不可睡上下文里放弃等待、带计数硬进入 */
static long ikaslr_wait_residual;
static long ikaslr_max_active;

/*
 * 自测的「慢扫描」档：在相邻两个核的读取之间睡一小会儿，模拟随机化线程在扫描
 * 中途被抢占或被中断打断（核数多时扫描本身也更长）。判空协议的正确性不能依赖
 * 扫描足够快，朴素方案的误判也正是在这种时候出现。
 */
static inline void ikaslr_scan_gap(bool slow)
{
	if (IS_ENABLED(CONFIG_IKASLR_TRACK_SELFTEST) && slow)
		usleep_range(500, 1000);
}

/* 两个和及其差。顺序固定：先 exit、屏障、后 enter（论证见文件头）。*/
static u64 __ikaslr_sum(u64 *enters, u64 *exits, bool slow)
{
	u64 x = ikaslr_sum_field(/*exit=*/true);
	u64 e;

	ikaslr_scan_gap(slow);		/* 慢扫描档：拉长两次求和之间的窗口 */
	smp_mb();
	e = ikaslr_sum_field(/*exit=*/false);
	if (enters)
		*enters = e;
	if (exits)
		*exits = x;
	return e - x;
}

static u64 ikaslr_sum(u64 *enters, u64 *exits)
{
	return __ikaslr_sum(enters, exits, false);
}

/*
 * 只有在阻断标志已置位（ikaslr_block_region() 之后）时，返回 0 才意味着区域为空
 * 且会保持为空；否则只是一次观测值。
 */
int ikaslr_active_count(void)
{
	return (int)ikaslr_sum(NULL, NULL);
}

int ikaslr_inside_count(void)
{
	return ikaslr_active_count();
}

unsigned long ikaslr_whitelist_rejects(void)
{
	/*
	 * 出口跳板对外部函数是一条编译期写死的 `bl G`，位于只读的固定区，目标无法
	 * 在运行期被改成白名单之外的函数——直接调用的白名单由构造保证，不需要运行期
	 * 查询，也就不存在拒绝。
	 */
	return 0;
}

void ikaslr_get_stats(struct ikaslr_stats *out)
{
	u64 e, x;
	long act = (long)ikaslr_sum(&e, &x);

	if (act > READ_ONCE(ikaslr_max_active))
		WRITE_ONCE(ikaslr_max_active, act);

	memset(out, 0, sizeof(*out));
	out->enters = e;	/* 跨边界进入：入口跳板进入 + 出口跳板返回 */
	out->outs = x;		/* 跨边界离开：入口跳板返回 + 出口跳板离开 + 撤销 */
	out->backoffs = atomic_long_read(&ikaslr_backoffs);
	out->forced = atomic_long_read(&ikaslr_forced);
	out->wait_residual = (int)READ_ONCE(ikaslr_wait_residual);
	out->max_active = (int)READ_ONCE(ikaslr_max_active);
}

static inline bool ikaslr_can_sleep(void)
{
	/*
	 * preemptible() 单独不够：PREEMPT_RCU 下 rcu_read_lock() 不关抢占，而 VFS 的
	 * rcu-walk 会在 RCU 读端临界区里调用被随机化的函数。
	 */
	return preemptible() && !rcu_preempt_depth();
}

/* 不可睡上下文里放弃等待前的最大重试次数，理由见下。*/
#define IKASLR_SPIN_LIMIT 4096

/*
 * 跳板在四处查阻断标志（论文 §3.3.2）：两个跨边界入站方向（入口跳板进入、出口
 * 跳板返回），以及区域内部调用的进入与返回。四处在被拦住时状态相同——本执行流
 * 已被计入（标志为 1）、手里只有返回标记而没有任何区域内的真实地址——因此共用
 * 这一条慢路径：撤销计数（标志清零、exit++）、等待放行、再按「加一、屏障、置
 * 标志、查阻断」重新进入。撤销期间区域可以被搬走，返回时 resolve_token 按标记
 * 解析到新位置。
 *
 * 由 tramp.S 的 __ikaslr_blocked_slow 在保存全部参数/返回值寄存器后调用。
 *
 * 不可睡的上下文里只能自旋，而自旋必须有上限：被随机化的函数会出现在关中断的
 * 路径上，而随机化方等不到空时阻断窗口就是它的整个超时——两个核关着中断无限
 * 自旋会把让对方超时所需的时钟中断也卡住。到达上限就**带着已加过的计数**进入：
 * 随机化方的判空必然看得见这次进入，于是超时并跳过本轮，不会在我们身处区域内时
 * 搬移代码。
 */
/*
 * 随机化线程自身：持有 block 的那个任务。它在阻断窗口里可能调用被随机化的函数
 * （x86 的 text_poke 改桩路径就会），若也让它在 blocked_slow 里等 unblock 就是
 * 自我死锁——放它的只有它自己（实测 S3 x86 全体 sleeping 死锁）。放行是安全的：
 * 阻断窗口内 live 与 ready 两份变体都可执行，退役旧变体在 unblock 之后、其调用早已
 * 同步返回。arm64 的写桩器不碰随机化函数，故那边这条分支不触发。
 */
struct task_struct *ikaslr_rand_task;

asmlinkage void ikaslr_blocked_slow(void)
{
	struct thread_info *ti = current_thread_info();
	int spins = 0;

	if (current == READ_ONCE(ikaslr_rand_task))
		return;			/* 随机化线程自身：放行，不等待 */

	for (;;) {
		/* 顺序与跳板一致：清标志先于 exit++，加一先于置标志。*/
		WRITE_ONCE(ti->ikaslr_inside, 0);
		ikaslr_bump_exit();
		atomic_long_inc(&ikaslr_backoffs);

		if (ikaslr_can_sleep())
			wait_event(ikaslr_unblock_wq, !READ_ONCE(ikaslr_blocked));
		else
			cpu_relax();

		ikaslr_bump_enter();
		smp_mb();
		WRITE_ONCE(ti->ikaslr_inside, 1);
		if (likely(!READ_ONCE(ikaslr_blocked)))
			return;
		if (!ikaslr_can_sleep() && ++spins > IKASLR_SPIN_LIMIT) {
			atomic_long_inc(&ikaslr_forced);
			return;
		}
	}
}

/*
 * C 形态的进出（与汇编跳板同一协议）。汇编跳板不调用它们；留给以宏手工标注的
 * 函数（ikaslr.h 的 IKASLR_TRAMP_FN / IKASLR_OUT_CALL）与自测使用。返回进入前的
 * 标志，调用方原样传回 leave —— 对应跳板压在自己栈帧里的那个值。
 */
static inline u32 __ikaslr_enter(void)
{
	struct thread_info *ti = current_thread_info();
	u32 was = READ_ONCE(ti->ikaslr_inside);

	if (!was) {
		ikaslr_bump_enter();
		smp_mb();
		WRITE_ONCE(ti->ikaslr_inside, 1);
	}
	if (unlikely(READ_ONCE(ikaslr_blocked)))
		ikaslr_blocked_slow();
	return was;
}

static inline void __ikaslr_leave(u32 was)
{
	struct thread_info *ti = current_thread_info();

	if (!was) {
		WRITE_ONCE(ti->ikaslr_inside, 0);
		ikaslr_bump_exit();
	} else if (unlikely(READ_ONCE(ikaslr_blocked))) {
		ikaslr_blocked_slow();
	}
}

/*
 * 旧接口没有地方传「进入前的标志」，这里用 task_struct 里现成的 ikaslr_depth
 * 记嵌套层数：只有最外层（depth 0→1 / 1→0）跨边界。
 */
void ikaslr_enter(void)
{
	if (current->ikaslr_depth++ == 0)
		__ikaslr_enter();
}
EXPORT_SYMBOL_GPL(ikaslr_enter);

void ikaslr_leave(void)
{
	if (--current->ikaslr_depth == 0)
		__ikaslr_leave(0);
}
EXPORT_SYMBOL_GPL(ikaslr_leave);

void ikaslr_out_enter(void *target)
{
	struct thread_info *ti = current_thread_info();

	if (READ_ONCE(ti->ikaslr_inside)) {
		WRITE_ONCE(ti->ikaslr_inside, 0);
		ikaslr_bump_exit();
	}
}
EXPORT_SYMBOL_GPL(ikaslr_out_enter);

void ikaslr_out_leave(void)
{
	/* 宏形态的调用方必在区域内（depth > 0），回来时重新计入。*/
	if (current->ikaslr_depth)
		__ikaslr_enter();
}
EXPORT_SYMBOL_GPL(ikaslr_out_leave);

void ikaslr_block_region(void)
{
	WRITE_ONCE(ikaslr_rand_task, current);	/* 本任务在窗口内豁免自阻塞 */
	WRITE_ONCE(ikaslr_blocked, 1);
	smp_mb();			/* 置标志 先于 读计数 */
}

void ikaslr_unblock_region(void)
{
	WRITE_ONCE(ikaslr_blocked, 0);
	WRITE_ONCE(ikaslr_rand_task, NULL);
	smp_mb();
	wake_up_all(&ikaslr_unblock_wq);
}

/*
 * 等待区域变空。返回 0 表示已空，-ETIMEDOUT 表示超时（放弃本轮）。
 *
 * 只增不减的计数没有「归零」这个事件可以挂唤醒，因此这里是轮询。区域非空只有
 * 两种原因：别的核上有执行流正在区域内跑（微秒内就会出来），或者有任务在区域内
 * 被抢占（得让出 CPU 它才能跑完）。前者短自旋即可，后者必须睡——单核上尤其如此。
 */
int ikaslr_wait_region_empty(unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	unsigned int spins = 0;
	u64 left;

	while ((left = ikaslr_sum(NULL, NULL)) != 0) {
		if (time_after(jiffies, deadline)) {
			WRITE_ONCE(ikaslr_wait_residual, (long)left);
			return -ETIMEDOUT;
		}
		if (++spins > 64 && ikaslr_can_sleep())
			usleep_range(20, 50);
		else
			cpu_relax();
	}
	return 0;
}

#ifdef CONFIG_IKASLR_TRACK_SELFTEST
/*
 * ---- 判空协议自测（交接文档 §9.3）----
 *
 * 多个内核线程在各核上并发进出并**强制迁移**（进入后换核再离开），检查线程反复
 * 「阻断 → 判空」。真值由一个只在放行之后加一、离开之前减一的全局原子量给出：
 * 阻断期间没有新的放行，真值只减不增，所以判空之后读到真值非零就是一次确凿的
 * 误判（区域非空却判为空）。
 *
 * 同时维护一组影子计数器，实现文件头所说的朴素方案（每核一个可加可减的计数、
 * 按核序求和），统计它在同样的交错下误判了多少次——这既是论文里「为什么不能用
 * 朴素做法」的实测依据，也说明本自测确实造得出那种交错（否则零误判没有说服力）。
 *
 * 自测线程直接调用 C 形态的进出，与真实跳板共用同一组计数与阻断标志；系统里
 * 真实的跨区域调用照常发生，只会让「判空成功」更少，不影响误判的判定。
 */
static atomic_t st_truth;
static atomic64_t st_naive[NR_CPUS];
static bool st_stop;

static unsigned int st_migrate(unsigned int cpu)
{
	cpu = cpumask_next(cpu, cpu_online_mask);
	if (cpu >= nr_cpu_ids)
		cpu = cpumask_first(cpu_online_mask);
	set_cpus_allowed_ptr(current, cpumask_of(cpu));
	return cpu;
}

static int st_worker(void *arg)
{
	unsigned int cpu = (unsigned long)arg % nr_cpu_ids;
	bool stay = ((unsigned long)arg & 3) == 1;	/* 每 4 个线程 1 个 */
	u32 was;

	while (!READ_ONCE(st_stop)) {
		/*
		 * 影子的朴素协议：同样「先加一、屏障、再查阻断」，被拦则减一撤销。
		 * 撤销前换核，造出文件头说的那种「A 核加一、B 核减一」的瞬态进入。
		 */
		for (;;) {
			atomic64_inc(&st_naive[raw_smp_processor_id()]);
			smp_mb();
			if (!READ_ONCE(ikaslr_blocked))
				break;
			cpu = st_migrate(cpu);
			atomic64_dec(&st_naive[raw_smp_processor_id()]);
			wait_event(ikaslr_unblock_wq,
				   !READ_ONCE(ikaslr_blocked) || READ_ONCE(st_stop));
			if (READ_ONCE(st_stop))
				goto out;
		}

		was = __ikaslr_enter();
		atomic_inc(&st_truth);		/* 已被两种协议放行 */

		cpu = st_migrate(cpu);		/* 强制迁移：在另一个核上离开 */
		/*
		 * 四分之一的线程在区域内多待一会儿（相当于在区域内被抢占），其余快进
		 * 快出、制造瞬态进入。停留时长与检查周期同量级是有意的：全是快进快出
		 * 时扫描期间几乎没有执行流在内，停留过长（实测 2–4 ms × 半数线程）则
		 * 区域从不为空、一次「空」的判定都没有——两头的「零误判」都是空话。
		 */
		if (stay)
			usleep_range(300, 600);

		atomic_dec(&st_truth);
		__ikaslr_leave(was);
		atomic64_dec(&st_naive[raw_smp_processor_id()]);
		cond_resched();
	}
out:
	while (!kthread_should_stop())
		schedule_timeout_interruptible(1);
	return 0;
}

int ikaslr_track_selftest(unsigned int ms, struct ikaslr_track_result *res)
{
	unsigned int nthreads = min_t(unsigned int, 2 * num_online_cpus(), 64);
	struct task_struct **th;
	unsigned long deadline;
	unsigned int i;
	int cpu;

	memset(res, 0, sizeof(*res));
	th = kcalloc(nthreads, sizeof(*th), GFP_KERNEL);
	if (!th)
		return -ENOMEM;

	WRITE_ONCE(st_stop, false);
	for (i = 0; i < nthreads; i++) {
		th[i] = kthread_run(st_worker, (void *)(unsigned long)i,
				    "ikaslr-st/%u", i);
		if (IS_ERR(th[i])) {
			th[i] = NULL;
			break;
		}
	}
	res->threads = i;

	deadline = jiffies + msecs_to_jiffies(ms);
	while (time_before(jiffies, deadline)) {
		/* 快慢扫描交替；慢扫描见 ikaslr_scan_gap()。*/
		bool slow = res->checks & 1;
		s64 naive = 0;

		/* 朴素方案：同样先阻断，再按核序对可加可减的计数求和。*/
		ikaslr_block_region();
		for_each_possible_cpu(cpu) {
			naive += atomic64_read(&st_naive[cpu]);
			ikaslr_scan_gap(slow);
		}
		if (naive == 0) {
			res->naive_empty++;
			if (atomic_read(&st_truth) != 0)
				res->naive_wrong++;
		}
		ikaslr_unblock_region();
		usleep_range(1000, 2000);	/* 放行间隔，见下 */

		/* 双计数：独立的一轮阻断，与朴素方案面对同样的并发。*/
		ikaslr_block_region();
		res->checks++;
		if (__ikaslr_sum(NULL, NULL, slow) == 0) {
			res->empty++;
			if (atomic_read(&st_truth) != 0)
				res->wrong++;
		}
		ikaslr_unblock_region();
		/*
		 * 两轮阻断之间必须留出放行间隔：早先这里只有 cond_resched()，阻断窗口
		 * 首尾相接，工作线程几乎进不了区域（实测 8 线程下 80% 的判空为空），
		 * 「从不误判」就没被真正考验。
		 */
		usleep_range(1000, 2000);
	}

	WRITE_ONCE(st_stop, true);
	wake_up_all(&ikaslr_unblock_wq);
	for (i = 0; i < nthreads; i++)
		if (th[i])
			kthread_stop(th[i]);
	kfree(th);

	pr_info("track selftest: %u threads, %lu checks, srcu empty=%lu wrong=%lu, naive empty=%lu wrong=%lu -> %s\n",
		res->threads, res->checks, res->empty, res->wrong,
		res->naive_empty, res->naive_wrong,
		res->wrong ? "FAIL" : "PASS");
	return res->wrong ? -EINVAL : 0;
}
#endif /* CONFIG_IKASLR_TRACK_SELFTEST */
