// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 控制流审计（论文 §4.4.4）。
 *
 * 被探测函数的原地址被重映射到填满陷阱指令的区域，同时在别处保留一份正常副本。
 * 此后任何经过被探测函数的控制流转移都会触发陷阱，从而可被审计：按触发情形区分
 * "正常调用"与"gadget 使用"，后者触发一次随机化。
 *
 * 审计判据：陷阱在函数内的偏移
 * ----------------------------
 * §4.4.4 的四类判定其核心区分是"目标是不是函数入口"：
 *   - 落在**入口**（offset 0）→ 正常的整函数调用/跳转，放行到副本，不触发；
 *   - 落在**中部**（offset > 0）→ 跳进函数中间是典型的 gadget 使用模式，触发。
 *
 * 这个判据可以直接从"陷阱在函数内的偏移"读出，**不需要知道来源指令**。精确的
 * 来源指令分类（直接/间接调用、间接跳转、返回）需要 LBR(x86)/BRBE(arm64) 回溯
 * 跳转来源，属于 S2.3 的增强；本文件先用偏移判据，它抓住了四类表的核心区分，
 * 且无 LBR 依赖即可验证。差距在 04-detection.md 中如实说明。
 *
 * 与随机化的集成
 * --------------
 * 本文件先独立验证审计逻辑本身（针对固定分配的陷阱/副本对）。把"被探测"应用到
 * 随机化 live 副本、并在每次随机化后重新布设陷阱，属于 S2.5 的集成工作。
 */
#define pr_fmt(fmt) "ikaslr/detect: " fmt

#include <linux/ikaslr.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/vmalloc.h>
#include <linux/set_memory.h>
#include <linux/cacheflush.h>
#include <linux/mm.h>
#include <linux/atomic.h>
#include <linux/ptrace.h>

#include "internal.h"

#ifdef CONFIG_X86_64
#define IKASLR_TRAP_BYTE	0xcc	/* int3 */
#else
#define IKASLR_TRAP_BYTE	0x00	/* udf #0 */
#endif

#define IKASLR_MAX_PROBED	16
/* 连续多少次"返回到被探测函数"算作 ROP 特征（§4.4.4 返回指令一行）。*/
#define IKASLR_RET_STREAK	3

struct ikaslr_probed {
	unsigned long	trap;	/* 陷阱区起始（原函数当前地址） */
	unsigned long	copy;	/* 正常副本起始 */
	size_t		size;
	const char	*name;
};

static struct ikaslr_probed probed[IKASLR_MAX_PROBED];
static int nprobed;
static DEFINE_SPINLOCK(probed_lock);

static atomic_long_t audit_benign;	/* 落在入口：正常调用 */
static atomic_long_t audit_gadget;	/* 落在中部：gadget 模式 */
static atomic_long_t audit_triggers;	/* 因审计触发的随机化次数 */

void ikaslr_detect_stats(unsigned long *benign, unsigned long *gadget,
			 unsigned long *triggers)
{
	*benign = atomic_long_read(&audit_benign);
	*gadget = atomic_long_read(&audit_gadget);
	*triggers = atomic_long_read(&audit_triggers);
}

/*
 * 把一个函数标记为被探测：分配正常副本，原地址整体填陷阱。
 * copy_src 指向要复制的正常代码；trap_at 是要埋陷阱的地址（须可迁移为可写）。
 */
int ikaslr_detect_mark(const char *name, void *trap_at, void *copy_src,
		       size_t size)
{
	unsigned long flags;
	void *copy;
	int npages;

	if (nprobed >= IKASLR_MAX_PROBED)
		return -ENOSPC;

	copy = __vmalloc_node_range(PAGE_ALIGN(size), PAGE_SIZE,
				    VMALLOC_START, VMALLOC_END,
				    GFP_KERNEL, PAGE_KERNEL,
				    VM_FLUSH_RESET_PERMS, NUMA_NO_NODE,
				    __builtin_return_address(0));
	if (!copy)
		return -ENOMEM;
	npages = PAGE_ALIGN(size) >> PAGE_SHIFT;

	memcpy(copy, copy_src, size);
	flush_icache_range((unsigned long)copy, (unsigned long)copy + size);
	set_memory_ro((unsigned long)copy, npages);
	set_memory_x((unsigned long)copy, npages);

	/* 原地址填陷阱（保持可执行，必须能执行到陷阱才会触发审计）。*/
	{
		unsigned long ta = (unsigned long)trap_at & PAGE_MASK;
		int tp = (PAGE_ALIGN((unsigned long)trap_at + size) - ta) >> PAGE_SHIFT;

		set_memory_nx(ta, tp);
		set_memory_rw(ta, tp);
		memset(trap_at, IKASLR_TRAP_BYTE, size);
		flush_icache_range((unsigned long)trap_at,
				   (unsigned long)trap_at + size);
		set_memory_ro(ta, tp);
		set_memory_x(ta, tp);
	}

	spin_lock_irqsave(&probed_lock, flags);
	probed[nprobed].trap = (unsigned long)trap_at;
	probed[nprobed].copy = (unsigned long)copy;
	probed[nprobed].size = size;
	probed[nprobed].name = name;
	nprobed++;
	spin_unlock_irqrestore(&probed_lock, flags);

	pr_info("marked %s probed: trap=%px copy=%px size=%zu\n",
		name, trap_at, copy, size);
	return 0;
}

/*
 * 审计一次落在被探测函数陷阱区的控制流转移。
 * faulting 是触发陷阱的地址；成功处理则把 *newp 设为应重定向到的副本地址并返回
 * true，同时按判据决定是否触发随机化。
 */
bool ikaslr_detect_audit(unsigned long faulting, unsigned long *newp)
{
	int i;

	for (i = 0; i < nprobed; i++) {
		struct ikaslr_probed *p = &probed[i];
		unsigned long off;

		if (faulting < p->trap || faulting >= p->trap + p->size)
			continue;
		off = faulting - p->trap;
		*newp = p->copy + off;		/* 无论如何都放行到副本，避免误杀 */

		if (off == 0) {
			/* 落在入口：正常的整函数调用/跳转。放行，不触发。*/
			atomic_long_inc(&audit_benign);
		} else {
			/* 落在中部：gadget 使用模式。放行到副本，同时触发随机化。*/
			atomic_long_inc(&audit_gadget);
			atomic_long_inc(&audit_triggers);
			ikaslr_request_rerandomize();
		}
		return true;
	}
	return false;
}
