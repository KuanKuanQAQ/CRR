// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 跨区域转移白名单（论文 §3.5.2、§5.4.4）。
 *
 * 白名单约束"从随机化区域到非随机化区域"的合法目标。构建发生在编译链接阶段：
 * 每个被随机化代码调用的外部函数用 IKASLR_WHITELIST() 登记一项，链接器汇总到
 * .data..ikaslr_whitelist 段；本文件在初始化时把它装入哈希表以保证常数时间查询，
 * 随后把该段置为只读。
 *
 * 运行时 fixed_out 在放行控制流之前查询白名单。第 5 章在同一位置再叠加 PA 验证，
 * 二者共同构成跨区域转移的双重检查。
 */
#define pr_fmt(fmt) "ikaslr: " fmt

#include <linux/ikaslr.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/hash.h>
#include <linux/set_memory.h>
#include <linux/mm.h>

#include "internal.h"

/* 哈希桶数取 2 的幂，装载因子控制在较低水平（§5.5.4）。*/
static struct hlist_head *wl_buckets;
static unsigned int wl_bits;
static int wl_count;

struct wl_node {
	struct hlist_node hnode;
	unsigned long addr;
};

static unsigned int wl_hash(unsigned long addr)
{
	return hash_long(addr, wl_bits);
}

/*
 * 查询：目标是否为编译期登记的合法外部函数。
 * 热路径（每次跨区域调用一次），因此只做哈希桶内的线性比较。
 */
bool ikaslr_whitelist_ok(void *target)
{
	unsigned long addr = (unsigned long)target;
	struct wl_node *n;

	if (unlikely(!wl_buckets))
		return true;	/* 尚未初始化：不阻断启动早期的调用 */

	hlist_for_each_entry(n, &wl_buckets[wl_hash(addr)], hnode)
		if (n->addr == addr)
			return true;
	return false;
}
EXPORT_SYMBOL_GPL(ikaslr_whitelist_ok);

int ikaslr_whitelist_count(void)
{
	return wl_count;
}

int __init ikaslr_whitelist_init(void)
{
	void **p;
	int n = __end_ikaslr_whitelist - __start_ikaslr_whitelist;
	unsigned int nbuckets;

	if (n <= 0) {
		pr_info("whitelist: empty (no cross-region callees registered)\n");
		return 0;
	}

	/* 桶数取 >= 2n 的 2 的幂，使平均链长 < 1。*/
	wl_bits = max(2, ilog2(roundup_pow_of_two(max(n * 2, 4))));
	nbuckets = 1u << wl_bits;
	wl_buckets = kcalloc(nbuckets, sizeof(*wl_buckets), GFP_KERNEL);
	if (!wl_buckets)
		return -ENOMEM;

	for (p = __start_ikaslr_whitelist; p < __end_ikaslr_whitelist; p++) {
		struct wl_node *node;

		if (!*p)
			continue;
		/*
		 * 跨模块去重：同一个外部函数（如 _raw_spin_lock）会被每个引用它的
		 * 编译单元各登记一项，编译器只能做到模块内去重。这里靠已插入的部分
		 * 做一次查重，既省内存也让 /proc/ikaslr/stats 的计数有意义。
		 */
		if (ikaslr_whitelist_ok(*p))
			continue;
		node = kmalloc(sizeof(*node), GFP_KERNEL);
		if (!node)
			return -ENOMEM;
		node->addr = (unsigned long)*p;
		hlist_add_head(&node->hnode, &wl_buckets[wl_hash(node->addr)]);
		wl_count++;
	}

	pr_info("whitelist: %d entr%s in %u buckets\n",
		wl_count, wl_count == 1 ? "y" : "ies", nbuckets);
	return 0;
}
