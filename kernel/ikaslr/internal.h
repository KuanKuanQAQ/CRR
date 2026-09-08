/* SPDX-License-Identifier: GPL-2.0 */
/*
 * I-KASLR 子系统内部接口（不对外暴露）。
 * 随机化流程（S1.4）经这些原语与线程追踪（S1.3）协作。
 */
#ifndef _KERNEL_IKASLR_INTERNAL_H
#define _KERNEL_IKASLR_INTERNAL_H

#include <linux/ikaslr.h>

/* 跳板表（初始化后有效）。*/
extern struct ikaslr_tramp **ikaslr_tbl;
extern int ikaslr_ntramp;

/*
 * 阻断/放行随机化区域的入口。论文 §3.4.5 的无副本随机化时序：
 *   ikaslr_block_region()      —— 置阻断标志，fixed_in 不再放新执行流进入
 *   ikaslr_wait_region_empty() —— 等待活跃集合变空
 *   ...此刻切换代码页是安全的...
 *   ikaslr_unblock_region()    —— 复位标志并唤醒等待者
 */
void ikaslr_block_region(void);
void ikaslr_unblock_region(void);
int  ikaslr_wait_region_empty(unsigned int timeout_ms);

/* 当前位于随机化区域内的执行流数量。*/
int ikaslr_active_count(void);

/* 调试/实验统计（S4.1 使用）。*/
struct ikaslr_stats {
	unsigned long enters;	/* 累计进入次数 */
	unsigned long backoffs;	/* 因阻断而回退重试的次数 */
	int	      max_active;/* 观察到的最大并发数 */
};
void ikaslr_get_stats(struct ikaslr_stats *out);

/* 随机化统计：上一次耗时（ns）与累计轮数（§3.6.4 时间分解）。*/
void ikaslr_rand_stats(u64 *last_ns, unsigned long *rounds);

/* 推迟随机化统计：次数、平均/最长推迟窗口（§3.6.6）。*/
void ikaslr_defer_stats(unsigned long *count, u64 *avg_ns, u64 *max_ns);
void ikaslr_defer_flush(void);

#endif /* _KERNEL_IKASLR_INTERNAL_H */
