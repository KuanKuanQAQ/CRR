// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 无副本随机化与代码变体池（论文 §3.4.5、§3.4.7）。
 *
 * 关键路径零分配
 * --------------
 * 随机化的关键路径（阻断 → 等待清空 → 改写 target → 放行）**不做任何内存分配、
 * 不调用可能睡眠的函数**。新的代码变体在此之前就已经准备好：变体池预先分配若干
 * 份等大的可执行缓冲区，后台把"下一份"排布好并置为 RO+X，随机化时只是把每个
 * target 槽指过去。
 *
 * 这一点是必要的：只有关键路径不睡眠，随机化才可能在中断返回路径一类的原子
 * 上下文中完成（§3.4.6）。分配与 set_memory_* 都会睡眠，把它们留在关键路径上
 * 会同时带来延迟与上下文限制。
 *
 * 变体状态机：
 *   FREE ──prepare(可睡眠)──> READY ──switch(原子)──> LIVE ──switch──> RETIRED
 *     ^                                                                  │
 *     └──────────────── recycle（环形复用，最旧者优先）──────────────────┘
 */
#define pr_fmt(fmt) "ikaslr: " fmt

#include <linux/ikaslr.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/random.h>
#include <linux/set_memory.h>
#include <linux/cacheflush.h>
#include <linux/mm.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/preempt.h>
#include <linux/log2.h>
#include <linux/minmax.h>
#include <linux/pgtable.h>
#include <asm/tlbflush.h>

#include "internal.h"

#define IKASLR_FN_ALIGN		16
#define IKASLR_MAX_GAP		256
#define IKASLR_WAIT_MS		100
/* 变体份数。>=3 才能做到"一份在用、一份就绪、至少一份可回收"。*/
#define IKASLR_NR_VARIANTS	4



enum ikaslr_var_state {
	VAR_FREE,	/* 空闲，可被 prepare */
	VAR_READY,	/* 已排布好并置 RO+X，等待启用 */
	VAR_LIVE,	/* 当前正在使用 */
	VAR_RETIRED,	/* 已退役，可能仍有陈旧返回地址指向它 */
};

struct ikaslr_variant {
	void			*base;
	unsigned long		 cap;	/* 分配容量（字节） */
	unsigned long		 used;	/* 本次排布实际占用 */
	unsigned long		*off;	/* 每个函数在本变体内的偏移 */
	enum ikaslr_var_state	 state;
	bool			 poisoned;/* 退役后陷阱是否已就位（填充或重映射）*/
	unsigned long		 round;	/* 成为 LIVE / RETIRED 时的轮次 */

	/*
	 * 陷阱影像（§3.5.5 的"预建 int3 页 + 切换时改映射"方案）。
	 * trap_base 是与 base 等大的并行区，在 prepare 中预填成"函数处 int3、其余为 0"
	 * 并置 RO+X（连同直映射别名一并修正，避免切换时出现跨别名 W+X）。切换退役时
	 * 只把 base 的页表项重指到 trap_pg（不改权限），窗口即刻关死。
	 */
	void			*trap_base;
	struct page		**code_pg;/* base 自身的物理页（分配时捕获，用于回指）*/
	struct page		**trap_pg; /* trap_base 的物理页 */
	bool			 trapped; /* base 当前是否已重指到 trap_pg */
};

static struct ikaslr_variant ikaslr_vars[IKASLR_NR_VARIANTS];
static struct ikaslr_variant *ikaslr_live;
static bool ikaslr_pool_ready;
static bool ikaslr_image_retired;

static DEFINE_SPINLOCK(ikaslr_pool_lock);	/* 保护变体状态迁移 */
static atomic_t ikaslr_in_progress = ATOMIC_INIT(0);

static u64 ikaslr_last_ns;		/* 关键路径耗时（不含准备） */
/* 关键路径三段分解（E3-3／E3-7）：等待活跃集合清空 / 更新 target 槽 / 改映射到陷阱镜像 */
static u64 ikaslr_wait_ns, ikaslr_update_ns, ikaslr_remap_ns;
static u64 ikaslr_last_prep_ns;		/* 一次变体准备的耗时 */
static unsigned long ikaslr_rounds;
static unsigned long ikaslr_no_ready;	/* 因无就绪变体而错过的次数 */

void ikaslr_rand_stats(u64 *last_ns, unsigned long *rounds)
{
	*last_ns = READ_ONCE(ikaslr_last_ns);
	*rounds = READ_ONCE(ikaslr_rounds);
}

void ikaslr_rand_phases(u64 *wait_ns, u64 *update_ns, u64 *remap_ns)
{
	*wait_ns = READ_ONCE(ikaslr_wait_ns);
	*update_ns = READ_ONCE(ikaslr_update_ns);
	*remap_ns = READ_ONCE(ikaslr_remap_ns);
}

void ikaslr_pool_stats(u64 *prep_ns, unsigned long *missed, int *nready)
{
	unsigned long flags;
	int i, n = 0;

	spin_lock_irqsave(&ikaslr_pool_lock, flags);
	for (i = 0; i < IKASLR_NR_VARIANTS; i++)
		if (ikaslr_vars[i].state == VAR_READY)
			n++;
	spin_unlock_irqrestore(&ikaslr_pool_lock, flags);

	*prep_ns = READ_ONCE(ikaslr_last_prep_ns);
	*missed = READ_ONCE(ikaslr_no_ready);
	*nready = n;
}

/*
 * 变体页的权限切换，始终维持 W^X。
 *
 * 顺序很重要：不能在页面仍可执行时直接加写权限，否则会出现 W+X，内核的 CPA
 * 会报 "W^X violation"。因此写之前先去掉执行权限，写完之后先只读再加回执行。
 */
static int ikaslr_make_writable(void *base, unsigned long npages)
{
	int ret = set_memory_nx((unsigned long)base, npages);

	if (ret)
		return ret;
	return set_memory_rw((unsigned long)base, npages);
}

static int ikaslr_make_exec(void *base, unsigned long npages)
{
	int ret = set_memory_ro((unsigned long)base, npages);

	if (ret)
		return ret;
	return set_memory_x((unsigned long)base, npages);
}

/*
 * 把一段 vmalloc 虚地址 [va, va+npages) 的页表项重指到 pages[]，权限置 RO+X。
 * 只改物理页号、不改权限位——pages[] 的直映射别名已由 set_memory_ro/x 预先设为
 * RO+X（见 ikaslr_build_trap_image），故此处不产生跨别名 W+X。
 */
struct ikaslr_remap_arg {
	unsigned long	 va;
	struct page	**pages;
};

static int ikaslr_remap_pte(pte_t *pte, unsigned long addr, void *data)
{
	struct ikaslr_remap_arg *a = data;
	unsigned long idx = (addr - a->va) >> PAGE_SHIFT;

	set_pte_at(&init_mm, addr, pte,
		   pfn_pte(page_to_pfn(a->pages[idx]), PAGE_KERNEL_ROX));
	return 0;
}

static int ikaslr_remap(void *va, struct page **pages, unsigned long npages)
{
	struct ikaslr_remap_arg arg = { .va = (unsigned long)va, .pages = pages };
	unsigned long len = npages << PAGE_SHIFT;
	int ret;

	ret = apply_to_existing_page_range(&init_mm, (unsigned long)va, len,
					   ikaslr_remap_pte, &arg);
	if (ret)
		return ret;
	flush_tlb_kernel_range((unsigned long)va, (unsigned long)va + len);
	flush_icache_range((unsigned long)va, (unsigned long)va + len);
	return 0;
}

/*
 * 预建一个变体的陷阱影像：把 trap_base 填成"每个函数处 int3/BRK、其余为 0"，
 * 再置 RO+X（连同直映射别名）。在 prepare 里、关键路径之外完成，因此切换退役时
 * 只需一次纯页表重指 + TLB 刷新，无需 set_memory、无需写内存、窗口不再存在。
 */
static int ikaslr_build_trap_image(struct ikaslr_variant *v)
{
	unsigned long npages = v->cap >> PAGE_SHIFT;
	int i, ret;

	ret = ikaslr_make_writable(v->trap_base, npages);
	if (ret)
		return ret;
	memset(v->trap_base, 0, v->used);
	for (i = 0; i < ikaslr_ntramp; i++)
		ikaslr_fill_traps(v->trap_base + v->off[i], ikaslr_tbl[i]->size);
	flush_icache_range((unsigned long)v->trap_base,
			   (unsigned long)v->trap_base + v->used);
	/* RO+X：set_memory 会一并把 trap_pg 的直映射别名设为 RO+X。*/
	return ikaslr_make_exec(v->trap_base, npages);
}

/*
 * 单个变体所需的最大容量，必须 >= ikaslr_plan() 任何一次排布的 used。
 *
 * plan 对每个函数消耗：对齐 cur 的填充（< IKASLR_FN_ALIGN）+ 原始 size +
 * 一段随机间隙（< IKASLR_MAX_GAP）；开头一段随机间隙，结尾再对齐一次。
 * 因此每个函数要预留 IKASLR_FN_ALIGN（覆盖对齐填充）+ ALIGN(size,16)（覆盖
 * 原始 size）+ IKASLR_MAX_GAP（覆盖间隙），另加开头间隙与结尾对齐各一份。
 *
 * 早期版本按 ALIGN(size,16)+MAX_GAP 计，漏掉了每函数最多 15 字节的对齐填充：
 * 函数少时被 PAGE_ALIGN 的整页余量掩盖，但函数数超过约 (PAGE_SIZE/16) 时
 * plan 的 used 会超过 cap，memset/memcpy 就会写到分配之外。
 */
static unsigned long ikaslr_var_capacity(void)
{
	unsigned long cap = IKASLR_MAX_GAP + IKASLR_FN_ALIGN;
	int i;

	for (i = 0; i < ikaslr_ntramp; i++)
		cap += IKASLR_FN_ALIGN +
		       ALIGN(ikaslr_tbl[i]->size, IKASLR_FN_ALIGN) +
		       IKASLR_MAX_GAP;
	return PAGE_ALIGN(cap);
}

/* 为一个变体排布新布局：Fisher-Yates 打乱顺序 + 随机间隙。*/
static void ikaslr_plan(struct ikaslr_variant *v)
{
	unsigned long cur;
	int *order;
	int i;

	order = kcalloc(ikaslr_ntramp, sizeof(*order), GFP_KERNEL);
	if (!order) {
		/* 退化为顺序排布，仍插入随机间隙。*/
		cur = get_random_u32_below(IKASLR_MAX_GAP);
		for (i = 0; i < ikaslr_ntramp; i++) {
			cur = ALIGN(cur, IKASLR_FN_ALIGN);
			v->off[i] = cur;
			cur += ikaslr_tbl[i]->size + get_random_u32_below(IKASLR_MAX_GAP);
		}
		v->used = ALIGN(cur, IKASLR_FN_ALIGN);
		return;
	}

	for (i = 0; i < ikaslr_ntramp; i++)
		order[i] = i;
	for (i = ikaslr_ntramp - 1; i > 0; i--) {
		/*
		 * 随机下标必须先算进变量再交换。
		 *
		 * 写成 swap(order[i], order[get_random_u32_below(i + 1)]) 是错的：
		 * 内核的 swap() 是宏，会把参数展开多次，于是随机数被取两次、得到两个
		 * 不同的下标——那不是交换，而是破坏排列，产生重复项。结果是某个函数的
		 * off[] 从未被赋值、保留上一轮的旧偏移，与本轮另一个函数的槽位重叠，
		 * 表现为随机化之后函数返回错误结果或崩溃（间歇性，取决于随机数）。
		 */
		int j = get_random_u32_below(i + 1);

		swap(order[i], order[j]);
	}

	/* 先清零：若排列有缺项，会立刻表现为偏移 0 而不是沿用上一轮的旧值。*/
	memset(v->off, 0, ikaslr_ntramp * sizeof(*v->off));

	cur = get_random_u32_below(IKASLR_MAX_GAP);
	for (i = 0; i < ikaslr_ntramp; i++) {
		cur = ALIGN(cur, IKASLR_FN_ALIGN);
		v->off[order[i]] = cur;
		cur += ikaslr_tbl[order[i]]->size;
		cur += get_random_u32_below(IKASLR_MAX_GAP);
	}
	v->used = ALIGN(cur, IKASLR_FN_ALIGN);
	kfree(order);
}

/*
 * 准备一个变体：排布 + 复制函数体 + 置 RO+X。
 * 可能睡眠（set_memory_*），只能在进程上下文调用 —— 这正是要把它移出关键路径的原因。
 *
 * **始终从内核映像里的原始函数体复制，而不是从当前变体。**
 * 早期实现以 ikaslr_live 为源，结果引入了竞态：准备在工作队列里跑，而
 * ikaslr_rerandomize() 可能同时把 live 换掉并退役它，另一个工作项又会把已退役的
 * 变体整体填成陷阱指令——于是可能把半个陷阱页拷进新变体，表现为随机化之后函数
 * 返回错误结果。实测确实间歇性触发。
 *
 * 以映像为唯一源头即消除该竞态，且不会累积复制误差。映像内的 .rand.text 首次
 * 迁移后已被置 NX，只可读不可执行，因此不构成可用的旧代码副本；其可读性由第 4 章
 * 的只执行内存机制覆盖。
 */
static int ikaslr_prepare(struct ikaslr_variant *v)
{
	unsigned long npages = v->cap >> PAGE_SHIFT;
	u64 t0 = ktime_get_ns();
	int i, ret;

	/*
	 * 若该变体退役时被重指到陷阱影像（trapped），先把 base 的页表项指回它自己的
	 * 代码页，否则下面的写会落到 trap_pg 上。回收复用变体的常规路径。
	 */
	if (v->trapped) {
		ret = ikaslr_remap(v->base, v->code_pg, npages);
		if (ret)
			return ret;
		v->trapped = false;
	}

	/* 变体在 READY/RETIRED 时是 RO+X，改写前先去执行权限再放开写权限。*/
	ret = ikaslr_make_writable(v->base, npages);
	if (ret)
		return ret;

	ikaslr_plan(v);
	/*
	 * 防御：排布结果绝不能超过分配容量，否则下面的 memset/memcpy 会越界写。
	 * ikaslr_var_capacity() 已保证 used <= cap，这里在任何写之前再兜一次底，
	 * 以免将来改动 plan/capacity 时静默越界。
	 */
	if (WARN_ONCE(v->used > v->cap,
		      "ikaslr: layout used %lu > cap %lu (%d fns), aborting prepare\n",
		      v->used, v->cap, ikaslr_ntramp)) {
		ikaslr_make_exec(v->base, npages);
		return -ENOSPC;
	}
	memset(v->base, 0, v->used);
	v->poisoned = false;

	for (i = 0; i < ikaslr_ntramp; i++)
		memcpy(v->base + v->off[i], ikaslr_tbl[i]->body,
		       ikaslr_tbl[i]->size);
	flush_icache_range((unsigned long)v->base,
			   (unsigned long)v->base + v->used);

	ret = ikaslr_make_exec(v->base, npages);
	if (ret)
		return ret;

	/*
	 * 预建本变体的陷阱影像，供它将来退役时的一步重映射使用（§3.5.5）。
	 * 失败不致命：切换退役时会回退到旧的"填充"路径。
	 */
	if (v->trap_base && ikaslr_build_trap_image(v))
		pr_warn_ratelimited("trap image build failed for variant %px\n", v->base);

	WRITE_ONCE(ikaslr_last_prep_ns, ktime_get_ns() - t0);
	return 0;
}

/* 取一个可用于准备的变体：优先 FREE，否则回收最旧的 RETIRED。*/
static struct ikaslr_variant *ikaslr_take_free(void)
{
	struct ikaslr_variant *oldest = NULL;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&ikaslr_pool_lock, flags);
	for (i = 0; i < IKASLR_NR_VARIANTS; i++) {
		if (ikaslr_vars[i].state == VAR_FREE) {
			/* 标记为 RETIRED 以占住它：两个补充工作项不能拿到同一份。*/
			ikaslr_vars[i].state = VAR_RETIRED;
			ikaslr_vars[i].poisoned = true;	/* 内容无效，无需填陷阱 */
			spin_unlock_irqrestore(&ikaslr_pool_lock, flags);
			return &ikaslr_vars[i];
		}
	}
	/*
	 * 无空闲：回收最旧的已退役变体。
	 * 前提假设：经过 IKASLR_NR_VARIANTS-2 轮之后，不再有栈帧会返回到该变体。
	 * 这是一个**有界但非零风险**的假设，S1.11 的 int3 兜底会把"返回到已回收
	 * 变体"变成可检测的事件而非静默错误。
	 */
	for (i = 0; i < IKASLR_NR_VARIANTS; i++) {
		if (ikaslr_vars[i].state != VAR_RETIRED)
			continue;
		if (!oldest || ikaslr_vars[i].round < oldest->round)
			oldest = &ikaslr_vars[i];
	}
	spin_unlock_irqrestore(&ikaslr_pool_lock, flags);
	return oldest;
}

/*
 * 退役变体填充陷阱指令（方案 B）。
 *
 * 切换时活跃集合为空，保证没有执行流**正在**退役变体中执行；但可能有执行流
 * 正在外部函数里，其栈帧的返回地址仍指向退役变体（fixed_out 已减计数）。
 * 这些返回由填充的陷阱指令捕获，再由 ikaslr_fixup_addr() 把 PC 指向新变体中的
 * 对应位置。
 *
 * 可能睡眠（set_memory_*），只在工作队列中调用。
 */
static int ikaslr_poison(struct ikaslr_variant *v)
{
	unsigned long npages = v->cap >> PAGE_SHIFT;
	int ret;

	ret = ikaslr_make_writable(v->base, npages);
	if (ret)
		return ret;
	ikaslr_fill_traps(v->base, v->used);
	flush_icache_range((unsigned long)v->base,
			   (unsigned long)v->base + v->used);
	/* 保持可执行：必须能执行到陷阱指令才会触发 fixup。*/
	return ikaslr_make_exec(v->base, npages);
}

/*
 * 使退役变体陷阱化（§3.5.5）。优先走"重映射到预建陷阱影像"——纯页表重指 + 一次
 * TLB 刷新，无 set_memory、无写内存，可在切换后同步完成、窗口即关。仅在没有陷阱
 * 影像或重映射失败时，退回旧的就地填充路径。
 */
static int ikaslr_retire_trap(struct ikaslr_variant *v)
{
	unsigned long npages = v->cap >> PAGE_SHIFT;

	if (v->trap_base && !v->trapped) {
		u64 tr = ktime_get_ns();
		int rc = ikaslr_remap(v->base, v->trap_pg, npages);

		WRITE_ONCE(ikaslr_remap_ns, ktime_get_ns() - tr);
		if (!rc) {
			v->trapped = true;
			return 0;
		}
		return -EAGAIN;	/* 重映射失败：留给 refill_work 重试，勿半途改填充 */
	}
	return ikaslr_poison(v);
}

/*
 * 把一个落在已退役变体中的地址映射到当前变体中的对应位置。
 *
 * 在异常上下文中调用，因此**不取锁**：变体数组是定长的，状态变化稀少，
 * 竞态最坏结果是本次映射失败（会被如实上报），而不是数据损坏。
 */
bool ikaslr_fixup_addr(unsigned long addr, unsigned long *newp)
{
	struct ikaslr_variant *live = READ_ONCE(ikaslr_live);
	int i, f;

	if (!live)
		return false;

	for (i = 0; i < IKASLR_NR_VARIANTS; i++) {
		struct ikaslr_variant *v = &ikaslr_vars[i];
		unsigned long base = (unsigned long)READ_ONCE(v->base);
		unsigned long delta;

		if (!base || v == live)
			continue;
		if (addr < base || addr >= base + v->used)
			continue;
		if (READ_ONCE(v->state) != VAR_RETIRED) {
			/* 落在非退役变体中：说明该变体已被回收复用，无法安全映射。*/
			pr_warn_ratelimited("stale return into recycled variant %px\n",
					    (void *)addr);
			return false;
		}

		delta = addr - base;
		for (f = 0; f < ikaslr_ntramp; f++) {
			unsigned long fo = v->off[f];

			if (delta < fo || delta >= fo + ikaslr_tbl[f]->size)
				continue;
			*newp = (unsigned long)live->base + live->off[f] +
				(delta - fo);
			return true;
		}
		/* 落在函数之间的随机间隙里：不是合法的返回地址。*/
		return false;
	}
	return false;
}

/* 后台：把一个变体准备成 READY。*/
static void ikaslr_refill_work_fn(struct work_struct *w)
{
	struct ikaslr_variant *v;
	unsigned long flags;
	int i;

	/* 先给尚未填充陷阱的退役变体填上（关键路径之外，可睡眠）。*/
	for (i = 0; i < IKASLR_NR_VARIANTS; i++) {
		struct ikaslr_variant *r = &ikaslr_vars[i];

		if (r->state == VAR_RETIRED && !r->poisoned) {
			if (!ikaslr_retire_trap(r))
				r->poisoned = true;
		}
	}

	v = ikaslr_take_free();
	if (!v)
		return;
	if (ikaslr_prepare(v))
		return;
	spin_lock_irqsave(&ikaslr_pool_lock, flags);
	v->state = VAR_READY;
	spin_unlock_irqrestore(&ikaslr_pool_lock, flags);
}
static DECLARE_WORK(ikaslr_refill_work, ikaslr_refill_work_fn);

/*
 * 关键路径：切换到已就绪的变体。
 * 全程不分配、不睡眠 —— 只有阻断、等待、改写 target、放行。
 */
int ikaslr_rerandomize(void)
{
	struct ikaslr_variant *next = NULL, *old;
	unsigned long flags;
	u64 t0;
	int i, ret = 0;

	if (!ikaslr_ntramp || !READ_ONCE(ikaslr_pool_ready))
		return 0;

	if (atomic_cmpxchg(&ikaslr_in_progress, 0, 1) != 0)
		return -EBUSY;

	t0 = ktime_get_ns();

	/* 取一份就绪变体。没有就绪变体则本次放弃并催促补充。*/
	spin_lock_irqsave(&ikaslr_pool_lock, flags);
	for (i = 0; i < IKASLR_NR_VARIANTS; i++) {
		if (ikaslr_vars[i].state == VAR_READY) {
			next = &ikaslr_vars[i];
			break;
		}
	}
	spin_unlock_irqrestore(&ikaslr_pool_lock, flags);

	if (!next) {
		WRITE_ONCE(ikaslr_no_ready, READ_ONCE(ikaslr_no_ready) + 1);
		schedule_work(&ikaslr_refill_work);
		ret = -EAGAIN;
		goto out;
	}

	/* 阻断入口并等待活跃集合变空。*/
	{
		u64 tw = ktime_get_ns();

		ikaslr_block_region();
		ret = ikaslr_wait_region_empty(IKASLR_WAIT_MS);
		WRITE_ONCE(ikaslr_wait_ns, ktime_get_ns() - tw);
	}
	if (ret) {
		ikaslr_unblock_region();
		pr_warn_ratelimited("region not empty within %d ms, skipping round\n",
				    IKASLR_WAIT_MS);
		goto out;
	}

	/*
	 * 唯一的代码索引更新：每函数一个 target 槽。
	 * 更新量与这些函数有多少调用点无关（需求 D1）。
	 */
	{
		u64 tu = ktime_get_ns();

		for (i = 0; i < ikaslr_ntramp; i++)
			ikaslr_update_target(ikaslr_tbl[i],
					     next->base + next->off[i]);
		smp_wmb();
		WRITE_ONCE(ikaslr_update_ns, ktime_get_ns() - tu);
	}

	/*
	 * 调试用完整性自检：切换之后，每个 target 指向处的开头若干字节必须与
	 * 映像里该函数体的开头一致。不一致说明要么变体内容被写坏，要么 target
	 * 指到了错误的偏移——两者都会表现为"随机化之后函数返回错误结果"。
	 */
	if (IS_ENABLED(CONFIG_IKASLR_DEBUG)) {
		for (i = 0; i < ikaslr_ntramp; i++) {
			const u8 *want = ikaslr_tbl[i]->body;
			const u8 *got = next->base + next->off[i];
			size_t n = min_t(size_t, 8, ikaslr_tbl[i]->size);

			/*
			 * 打印全部被比较的 n 个字节，而不是只打头 4 个。
			 * 否则头 4 字节相同（例如 endbr64 f3 0f 1e fa）而第 5~8
			 * 字节不同时，日志会显示成 "want ...==got ..." 的假象，
			 * 让真实的不一致无法从日志里看出来。
			 */
			if (memcmp(want, got, n)) {
				pr_err("integrity: %s target=%px off=%lu content mismatch over %zu B\n"
				       "  want %*ph\n  got  %*ph\n",
				       ikaslr_tbl[i]->name, got, next->off[i], n,
				       (int)n, want, (int)n, got);
			}
		}
	}

	old = ikaslr_live;
	spin_lock_irqsave(&ikaslr_pool_lock, flags);
	next->state = VAR_LIVE;
	next->round = ++ikaslr_rounds;
	if (old) {
		old->state = VAR_RETIRED;
		old->round = ikaslr_rounds;
	}
	ikaslr_live = next;
	spin_unlock_irqrestore(&ikaslr_pool_lock, flags);

	ikaslr_unblock_region();
	WRITE_ONCE(ikaslr_last_ns, ktime_get_ns() - t0);

	/*
	 * 关键路径到此结束。以下是可睡眠的善后工作。
	 *
	 * 填陷阱要尽早：在填上之前，刚退役的变体里仍是**可执行的有效旧代码**，
	 * 构成一个旧副本暴露窗口。因此只要当前在进程上下文，就地同步填掉，
	 * 把窗口关死；原子上下文中只能交给工作队列，窗口长度取决于其调度。
	 */
	if (old && preemptible()) {
		if (!ikaslr_retire_trap(old))
			old->poisoned = true;
	}
	schedule_work(&ikaslr_refill_work);

out:
	atomic_set(&ikaslr_in_progress, 0);
	return ret;
}
EXPORT_SYMBOL_GPL(ikaslr_rerandomize);

/*
 * 初始化变体池：分配全部变体，准备第一份并启用，随后退役内核映像内的
 * .rand.text（置 NX），再准备第二份作为就绪变体。
 */
int __init ikaslr_pool_init(void)
{
	unsigned long cap;
	int i, ret;

	if (!ikaslr_ntramp)
		return 0;

	cap = ikaslr_var_capacity();
	for (i = 0; i < IKASLR_NR_VARIANTS; i++) {
		struct ikaslr_variant *v = &ikaslr_vars[i];

		v->cap = cap;
		v->off = kcalloc(ikaslr_ntramp, sizeof(*v->off), GFP_KERNEL);
		if (!v->off)
			return -ENOMEM;
		/*
		 * 以可读写、不可执行分配（PAGE_KERNEL 而非 PAGE_KERNEL_EXEC）：
		 * 分配出来就是 RWX 同样构成 W^X 违规。执行权限在 prepare 写完
		 * 之后才加上。
		 */
		v->base = __vmalloc_node_range(cap, PAGE_SIZE,
					       VMALLOC_START, VMALLOC_END,
					       GFP_KERNEL, PAGE_KERNEL,
					       VM_FLUSH_RESET_PERMS, NUMA_NO_NODE,
					       __builtin_return_address(0));
		if (!v->base)
			return -ENOMEM;

		/*
		 * 陷阱影像并行区（§3.5.5）。分配失败不致命：trap_base 为空时
		 * 退役走旧的填充路径，功能不变、只是没有零窗口优化。
		 */
		v->trap_base = __vmalloc_node_range(cap, PAGE_SIZE,
						    VMALLOC_START, VMALLOC_END,
						    GFP_KERNEL, PAGE_KERNEL,
						    VM_FLUSH_RESET_PERMS, NUMA_NO_NODE,
						    __builtin_return_address(0));
		if (v->trap_base) {
			unsigned long np = cap >> PAGE_SHIFT, k;

			v->code_pg = kcalloc(np, sizeof(*v->code_pg), GFP_KERNEL);
			v->trap_pg = kcalloc(np, sizeof(*v->trap_pg), GFP_KERNEL);
			if (!v->code_pg || !v->trap_pg) {
				vfree(v->trap_base);
				v->trap_base = NULL;
			} else {
				for (k = 0; k < np; k++) {
					v->code_pg[k] = vmalloc_to_page(v->base + (k << PAGE_SHIFT));
					v->trap_pg[k] = vmalloc_to_page(v->trap_base + (k << PAGE_SHIFT));
				}
			}
		}
		v->state = VAR_FREE;
	}

	/* 第一份：从内核映像复制并启用。*/
	ret = ikaslr_prepare(&ikaslr_vars[0]);
	if (ret)
		return ret;
	for (i = 0; i < ikaslr_ntramp; i++)
		ikaslr_update_target(ikaslr_tbl[i],
				     ikaslr_vars[0].base + ikaslr_vars[0].off[i]);
	ikaslr_vars[0].state = VAR_LIVE;
	ikaslr_vars[0].round = ++ikaslr_rounds;
	ikaslr_live = &ikaslr_vars[0];

	/*
	 * 退役映像内的 .rand.text：置 NX，使随机化区域内不再存在第二份可执行代码
	 * （无副本性质，§3.4.5）。链接脚本已把该段按页对齐并独占整页。
	 */
	if (!ikaslr_image_retired) {
		unsigned long start = (unsigned long)__rand_text_start;
		unsigned long np = (PAGE_ALIGN((unsigned long)__rand_text_end) -
				    start) >> PAGE_SHIFT;

		if (np && !set_memory_nx(start, np)) {
			ikaslr_image_retired = true;
			pr_info("retired in-image .rand.text (%lu page(s), now NX)\n", np);
		}
	}

	/* 第二份：预备就绪，使首次随机化即可走零分配的关键路径。*/
	ret = ikaslr_prepare(&ikaslr_vars[1]);
	if (ret)
		return ret;
	ikaslr_vars[1].state = VAR_READY;

	WRITE_ONCE(ikaslr_pool_ready, true);
	pr_info("variant pool: %d variants x %lu B, live=%px ready=%px\n",
		IKASLR_NR_VARIANTS, cap, ikaslr_vars[0].base, ikaslr_vars[1].base);
	return 0;
}

/*
 * ---- S1.5 触发入口与推迟策略（论文 §3.4.6）----
 *
 * 关键路径现在已经是**零分配、不睡眠**的（见文件开头），因此
 * ikaslr_rerandomize() 本身可以在中断返回路径一类的原子上下文中完成——这正是
 * §3.4.6 与【图 3-6】所要求的。推迟因此**不再是正确性要求，而是延迟策略**：
 *
 * 唯一仍可能长时间停留的是 ikaslr_wait_region_empty()。在原子上下文中它只能
 * 自旋，把等待时间直接转成中断延迟。因此这里在原子上下文中只给一个很短的等待
 * 预算：立即拿到空区域就地完成，否则推迟到进程上下文，避免拉长中断延迟。
 */

static DEFINE_PER_CPU(bool, ikaslr_deferred);
static atomic_long_t ikaslr_defer_count;
static atomic_long_t ikaslr_defer_ns_total;
static u64 ikaslr_defer_max_ns;
static u64 ikaslr_defer_req_ns;

static void ikaslr_defer_work_fn(struct work_struct *w)
{
	u64 waited = ktime_get_ns() - READ_ONCE(ikaslr_defer_req_ns);

	atomic_long_add(waited, &ikaslr_defer_ns_total);
	if (waited > READ_ONCE(ikaslr_defer_max_ns))
		WRITE_ONCE(ikaslr_defer_max_ns, waited);

	this_cpu_write(ikaslr_deferred, false);
	ikaslr_rerandomize();
}
static DECLARE_WORK(ikaslr_defer_work, ikaslr_defer_work_fn);

/* 原子上下文中允许的自旋等待预算（微秒级），避免拉长中断延迟。*/
#define IKASLR_ATOMIC_WAIT_US	50

static bool ikaslr_context_ok(void)
{
	return preemptible() && !in_interrupt() && !irqs_disabled();
}

/*
 * 随机化的统一触发入口（第 4 章检测模块调用）。
 * 可在任意上下文调用：进程上下文就地完成；原子上下文中若区域已空则同样就地
 * 完成（关键路径不睡眠），否则推迟，避免自旋拖长中断延迟。
 */
int ikaslr_request_rerandomize(void)
{
	if (ikaslr_context_ok())
		return ikaslr_rerandomize();

	/*
	 * 原子上下文：区域已经空的话可以直接做完（关键路径原子安全）。
	 * 否则不在这里等 —— 推迟到进程上下文。
	 */
	if (ikaslr_active_count() == 0)
		return ikaslr_rerandomize();

	this_cpu_write(ikaslr_deferred, true);
	atomic_long_inc(&ikaslr_defer_count);
	WRITE_ONCE(ikaslr_defer_req_ns, ktime_get_ns());
	schedule_work(&ikaslr_defer_work);
	return 0;
}
EXPORT_SYMBOL_GPL(ikaslr_request_rerandomize);

void ikaslr_defer_flush(void)
{
	flush_work(&ikaslr_defer_work);
	flush_work(&ikaslr_refill_work);
}

void ikaslr_defer_stats(unsigned long *count, u64 *avg_ns, u64 *max_ns)
{
	unsigned long n = atomic_long_read(&ikaslr_defer_count);

	*count = n;
	*avg_ns = n ? atomic_long_read(&ikaslr_defer_ns_total) / n : 0;
	*max_ns = READ_ONCE(ikaslr_defer_max_ns);
}
