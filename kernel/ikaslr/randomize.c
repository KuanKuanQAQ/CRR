// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 无副本随机化（论文 §3.4.5、§3.4.7）。
 *
 * 一次随机化的完整流程：
 *   1. 检查互斥标志（防止嵌套；S1.5 再补入上下文判定与推迟）
 *   2. 生成新布局（打乱函数顺序 + 随机间隙）
 *   3. 在新地址准备代码页并完成函数内重定位
 *   4. 置阻断标志并等待活跃集合变空
 *   5. 更新所有 target 槽          <- 唯一涉及代码索引更新的一步，量为 O(函数数)
 *   6. 切换新旧代码页的执行权限
 *   7. 复位阻断标志
 *   8. 回收旧代码页
 *
 * 无副本性质：第 4 步确定区域内既无执行流、也无会返回到区域内的栈帧，因此第 6
 * 步之后旧代码即可立即失效——任一时刻随机化区域内只有一份可执行代码，不存在
 * Shuffler 式方案的旧副本暴露窗口（§3.2.2）。
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

#include "internal.h"

/* 每个函数体在新布局中的对齐，与函数对齐一致。*/
#define IKASLR_FN_ALIGN		16
/* 函数之间插入的随机间隙上限（字节），用于随机化相对偏移。*/
#define IKASLR_MAX_GAP		256
/* 等待活跃集合清空的上限。超时则放弃本次随机化。*/
#define IKASLR_WAIT_MS		100

/* 当前这一份可执行代码的所在。base==NULL 表示仍在内核映像的 .rand.text 中。*/
static void *ikaslr_cur_base;
static unsigned long ikaslr_cur_size;
/* 映像内的 .rand.text 是否已被去除执行权限（首次迁移后即永久失效）。*/
static bool ikaslr_image_retired;

/* 互斥：随机化不可嵌套（§3.4.6 策略一）。*/
static atomic_t ikaslr_in_progress = ATOMIC_INIT(0);

/* 统计（供 §3.6.4 的时间分解使用）。*/
static u64 ikaslr_last_ns;
static unsigned long ikaslr_rounds;

void ikaslr_rand_stats(u64 *last_ns, unsigned long *rounds)
{
	*last_ns = READ_ONCE(ikaslr_last_ns);
	*rounds = READ_ONCE(ikaslr_rounds);
}

/*
 * 生成新布局：把函数顺序 Fisher-Yates 打乱，并在函数之间插入随机间隙。
 * order[] 输出为“新布局中的排列”，off[] 为各函数在新区域内的偏移。
 */
static unsigned long ikaslr_plan(int *order, unsigned long *off)
{
	unsigned long cur = 0;
	int i;

	for (i = 0; i < ikaslr_ntramp; i++)
		order[i] = i;
	for (i = ikaslr_ntramp - 1; i > 0; i--) {
		int j = get_random_u32_below(i + 1);
		swap(order[i], order[j]);
	}

	cur = get_random_u32_below(IKASLR_MAX_GAP);
	for (i = 0; i < ikaslr_ntramp; i++) {
		cur = ALIGN(cur, IKASLR_FN_ALIGN);
		off[order[i]] = cur;
		cur += ikaslr_tbl[order[i]]->size;
		cur += get_random_u32_below(IKASLR_MAX_GAP);
	}
	return ALIGN(cur, IKASLR_FN_ALIGN);
}

/*
 * 把每个函数体复制到新区域。
 *
 * 当前手工路径下，.rand.text 中的函数体必须是自足的（不含指向区域外的
 * PC 相对引用），复制才是正确的——函数内部的相对跳转随整体搬移自动保持正确，
 * 但对外部函数的调用与全局变量的引用会失效。LLVM pass 就绪后由函数级 PIC
 * 与共享 GOT 消除这一限制（S1.7/S1.10）。见 03-randomization.md 的说明。
 */
static void ikaslr_copy_bodies(void *dst, const unsigned long *off)
{
	int i;

	for (i = 0; i < ikaslr_ntramp; i++) {
		struct ikaslr_tramp *t = ikaslr_tbl[i];

		memcpy(dst + off[i], READ_ONCE(*t->target), t->size);
	}
	flush_icache_range((unsigned long)dst,
			   (unsigned long)dst + ikaslr_cur_size);
}

int ikaslr_rerandomize(void)
{
	unsigned long *off = NULL;
	int *order = NULL;
	void *nbase = NULL;
	void *obase;
	unsigned long nsize;
	u64 t0;
	int i, ret = 0;

	if (!ikaslr_ntramp)
		return 0;

	/* 1. 互斥：已有一次随机化在进行则直接返回（§3.4.6 策略一）。*/
	if (atomic_cmpxchg(&ikaslr_in_progress, 0, 1) != 0)
		return -EBUSY;

	t0 = ktime_get_ns();

	order = kcalloc(ikaslr_ntramp, sizeof(*order), GFP_KERNEL);
	off = kcalloc(ikaslr_ntramp, sizeof(*off), GFP_KERNEL);
	if (!order || !off) {
		ret = -ENOMEM;
		goto out;
	}

	/* 2. 生成新布局。*/
	nsize = ikaslr_plan(order, off);

	/* 3. 分配新代码页并复制（此时尚未阻断，不影响正在执行的流）。*/
	nbase = __vmalloc_node_range(nsize, IKASLR_FN_ALIGN,
				     VMALLOC_START, VMALLOC_END,
				     GFP_KERNEL, PAGE_KERNEL_EXEC,
				     VM_FLUSH_RESET_PERMS, NUMA_NO_NODE,
				     __builtin_return_address(0));
	if (!nbase) {
		ret = -ENOMEM;
		goto out;
	}
	obase = ikaslr_cur_base;
	ikaslr_cur_size = nsize;
	ikaslr_copy_bodies(nbase, off);

	/* 新代码页降为只读可执行（W^X）。*/
	set_memory_ro((unsigned long)nbase, PAGE_ALIGN(nsize) >> PAGE_SHIFT);

	/* 4. 阻断入口并等待活跃集合变空。*/
	ikaslr_block_region();
	ret = ikaslr_wait_region_empty(IKASLR_WAIT_MS);
	if (ret) {
		/* 有执行流长时间停留在区域内：放弃本次，保持原状。*/
		ikaslr_unblock_region();
		pr_warn("rerandomize: region not empty within %d ms, skipping\n",
			IKASLR_WAIT_MS);
		vfree(nbase);
		nbase = NULL;
		goto out;
	}

	/*
	 * 5. 更新所有 target 槽 —— 一次随机化中唯一的代码索引更新。
	 * 更新量等于函数数量，与这些函数有多少调用点无关（需求 D1）。
	 */
	for (i = 0; i < ikaslr_ntramp; i++)
		ikaslr_update_target(ikaslr_tbl[i], nbase + off[i]);
	smp_wmb();

	/* 6/7. 切换完成，放行。*/
	ikaslr_cur_base = nbase;
	ikaslr_unblock_region();
	nbase = NULL;			/* 已交接，勿在 out 处释放 */

	/*
	 * 8. 让旧的那一份不再可执行 —— 无副本性质的关键一步（§3.4.5）。
	 *
	 * 旧份若在 vmalloc 中，直接回收（VM_FLUSH_RESET_PERMS 会复位权限）；
	 * 若是内核映像里的 .rand.text，则不能回收，改为去除执行权限。链接脚本
	 * 已把 .rand.text 按页对齐并独占整页（前后各有 ALIGN(PAGE_SIZE)），
	 * 因此置 NX 不会波及其它代码。
	 *
	 * 此时活跃集合曾为空且 target 已指向新副本，旧副本不可能再被进入；
	 * 这一步之后随机化区域内只剩一份可执行代码。
	 */
	if (obase) {
		vfree(obase);
	} else if (!ikaslr_image_retired) {
		unsigned long start = (unsigned long)__rand_text_start;
		unsigned long npages =
			(PAGE_ALIGN((unsigned long)__rand_text_end) - start) >> PAGE_SHIFT;

		if (npages && !set_memory_nx(start, npages)) {
			ikaslr_image_retired = true;
			pr_info("retired in-image .rand.text (%lu page(s), now NX)\n",
				npages);
		} else if (npages) {
			pr_warn("failed to retire in-image .rand.text; "
				"an executable stale copy remains\n");
		}
	}

	WRITE_ONCE(ikaslr_last_ns, ktime_get_ns() - t0);
	WRITE_ONCE(ikaslr_rounds, READ_ONCE(ikaslr_rounds) + 1);

out:
	kfree(order);
	kfree(off);
	if (nbase)
		vfree(nbase);
	atomic_set(&ikaslr_in_progress, 0);
	return ret;
}
EXPORT_SYMBOL_GPL(ikaslr_rerandomize);
