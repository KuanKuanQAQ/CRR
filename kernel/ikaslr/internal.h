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

int ikaslr_whitelist_init(void);
int ikaslr_pool_init(void);
int ikaslr_fixup_init(void);
void ikaslr_fill_traps(void *base, size_t len);

/* 第 5 章 PA CFI（arm64）。*/
struct ikaslr_fptr;
void ikaslr_pacfi_create(struct ikaslr_fptr *f, void *func, u32 type_hash);
void *ikaslr_pacfi_verify(struct ikaslr_fptr *f, int cs_id);
void ikaslr_pacfi_finalize(struct ikaslr_fptr *f);
void ikaslr_pacfi_cfi_stats(unsigned long *viol, unsigned long *hfi);
int ikaslr_control_init(void);
extern int ikaslr_rf_counter;
int ikaslr_rf_general(int a);
int ikaslr_rf_helper(int x);
struct proc_dir_entry;
int ikaslr_bench_init(struct proc_dir_entry *dir);
int ikaslr_detect_mark(const char *name, void *trap_at, void *copy_src, size_t size);
bool ikaslr_detect_audit(unsigned long faulting, unsigned long *newp);
void ikaslr_detect_stats(unsigned long *benign, unsigned long *gadget, unsigned long *triggers);

/* 把落在已退役变体中的地址映射到当前变体的对应位置。异常上下文中调用。*/
bool ikaslr_fixup_addr(unsigned long addr, unsigned long *newp);
void ikaslr_fixup_stats(unsigned long *ok, unsigned long *fail);

/* 变体池统计：上次准备耗时、因无就绪变体错过的次数、当前就绪数。*/
void ikaslr_pool_stats(u64 *prep_ns, unsigned long *missed, int *nready);

/* 当前正在随机化区域**代码内**执行的流数（区别于 active：后者含会返回区域的栈帧）。*/
int ikaslr_inside_count(void);
unsigned long ikaslr_whitelist_rejects(void);

#endif /* _KERNEL_IKASLR_INTERNAL_H */
