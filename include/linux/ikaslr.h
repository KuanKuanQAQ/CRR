/* SPDX-License-Identifier: GPL-2.0 */
/*
 * I-KASLR: 面向 built-in 内核代码的持续随机化（论文第 3 章）。
 *
 * 本头文件是第 3 章"如何随机化"机制的公共接口：随机化区域/跳板区域的段注解、
 * 运行时描述符、以及供 fs/ 等被随机化子系统与运行时核心使用的 API。
 *
 * 分层索引组织（论文 §3.4）：
 *   - 函数粒度内：位置无关代码（由 LLVM pass 生成，见 tools/ikaslr，S1.10）；
 *   - 函数粒度间：固定位置的跳板函数集中索引，一次随机化只更新 target_addr。
 *
 * 段布局复用既有基础设施（include/asm-generic/vmlinux.lds.h）：
 *   .rand.text.<fn>        随机化函数体，可迁移
 *   .tramp.text.<fn>       固定地址跳板
 *   .data..rand_ptr_tbl    每函数一项，指向函数体，按链接顺序
 *   .data..tramp_ptr_tbl   每函数一项，指向跳板，按链接顺序
 * 两张指针表同步遍历即可配对跳板与函数体，并由相邻项地址差得出函数体大小。
 */
#ifndef _LINUX_IKASLR_H
#define _LINUX_IKASLR_H

/* ---- 段注解（手工路径；LLVM pass 就绪后可由编译器自动施加，见 S1.10）---- */
#define __ikaslr_tramp(fn) __section(".tramp.text." #fn) noinline
#define __ikaslr_rand(fn)  __section(".rand.text." #fn) noinline

#define __ikaslr_tramp_ptr(fn)						\
	static void *__ikaslr_tramp_ptr_##fn				\
		__attribute__((section(".data..tramp_ptr_tbl"))) __used = &fn
#define __ikaslr_rand_ptr(fn)						\
	static void *__ikaslr_rand_ptr_##fn				\
		__attribute__((section(".data..rand_ptr_tbl"))) __used = &fn

#ifndef __ASSEMBLY__

#include <linux/types.h>

/* 区域边界与指针表边界符号（由链接脚本给出）。*/
extern char __rand_text_start[], __rand_text_end[];
extern char __tramp_text_start[], __tramp_text_end[];
extern void *__start_tramp_ptr_tbl[], *__end_tramp_ptr_tbl[];
extern void *__start_rand_ptr_tbl[], *__end_rand_ptr_tbl[];

/*
 * 一个随机化函数的运行时描述符（在初始化时由两张指针表构建）。
 * 论文 §3.4.2：随机化时"唯一需要更新的"就是 target ——索引更新量 O(1)。
 */
struct ikaslr_func {
	void	*tramp;		/* fixed_in 跳板入口，位置固定、永不迁移 */
	void	*body;		/* 函数体当前地址（随机化后更新） */
	size_t	 size;		/* 函数体字节数（由相邻指针表项之差得出） */
	/* target/whitelist/计数等字段随 S1.2–S1.8 补入 */
};

#ifdef CONFIG_IKASLR

/* 已注册的随机化函数数量（初始化后有效）。*/
int ikaslr_nr_funcs(void);

/*
 * 线程追踪钩子（S1.3 实现；此处为供跳板调用的稳定符号）。
 * enter: 控制流经 fixed_in 进入随机化区域；leave: 经 fixed_out 离开。
 */
void ikaslr_enter(unsigned int idx);
void ikaslr_leave(unsigned int idx);

/*
 * 触发一次重随机化（S1.4/S1.5 实现）。返回 0 表示已完成或已推迟。
 * 完整流程见论文 §3.4.7：检查互斥→生成布局→准备新页→阻断并等待活跃集合清空
 * →更新各 target_addr→切换页权限→复位→回收旧页。
 */
int ikaslr_rerandomize(void);

#else  /* !CONFIG_IKASLR */

static inline int ikaslr_nr_funcs(void) { return 0; }
static inline void ikaslr_enter(unsigned int idx) { }
static inline void ikaslr_leave(unsigned int idx) { }
static inline int ikaslr_rerandomize(void) { return 0; }

#endif /* CONFIG_IKASLR */

#endif /* __ASSEMBLY__ */
#endif /* _LINUX_IKASLR_H */
