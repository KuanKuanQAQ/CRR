/* SPDX-License-Identifier: GPL-2.0 */
/*
 * I-KASLR: 面向 built-in 内核代码的持续随机化（论文第 3 章）。
 *
 * 分层索引组织（论文 §3.4）：
 *   - 函数粒度内：位置无关代码消除索引（LLVM pass，见 S1.10）；
 *   - 函数粒度间：固定位置的跳板函数集中索引 —— 一次随机化只更新每函数一个
 *     target_addr，索引更新量与调用点数量无关（需求 D1，O(1)）。
 *
 * 一个被随机化的函数在源码中写成一对：
 *
 *     IKASLR_RAND_FN(ssize_t, vfs_read, struct file *f, ...)   // 函数体，可迁移
 *     {  ...真实实现...  }
 *
 *     IKASLR_TRAMP_FN(ssize_t, vfs_read, struct file *f, ...)  // fixed_in 跳板
 *     {
 *         ssize_t ret;
 *         ikaslr_enter();                                  // 进入随机化区域
 *         ret = IKASLR_TARGET(vfs_read)(f, ...);           // 经 target 间接转移
 *         ikaslr_leave();                                  // 离开
 *         return ret;
 *     }
 *
 * 跳板保留原函数名，因此所有既有调用者不需要改动；函数体改名为 <fn>_body 并落在
 * .rand.text 中。随机化时只需改写 target 槽。
 *
 * 段布局：
 *   .tramp.text.<fn>        固定地址跳板（不迁移）
 *   .rand.text.<fn>         随机化函数体（可迁移）
 *   .data..ikaslr_tramp_tbl 每函数一项 struct ikaslr_tramp
 *   .data..ikaslr_target    target 槽（随机化时唯一被改写的数据）
 */
#ifndef _LINUX_IKASLR_H
#define _LINUX_IKASLR_H

#ifndef __ASSEMBLY__

#include <linux/types.h>
#include <linux/compiler.h>

/*
 * 一个随机化函数的链接期描述符。位于 .data..ikaslr_tramp_tbl，每函数一项。
 * size 在初始化时由相邻函数体地址之差算出，其余字段由链接期确定。
 */
struct ikaslr_tramp {
	void		*tramp;	/* fixed_in 跳板入口，地址固定、永不迁移 */
	void		*body;	/* 函数体的链接期原始地址 */
	void	       **target;/* -> target 槽：随机化时唯一需要更新的索引 */
	const char	*name;	/* 函数标识，用于调试与按名迁移 */
	size_t		 size;	/* 函数体字节数（初始化时填） */
};

/* 区域与表的边界符号（链接脚本给出）。*/
extern char __rand_text_start[], __rand_text_end[];
extern char __tramp_text_start[], __tramp_text_end[];
/*
 * 表中存放的是指针而非结构本身。x86-64 会把 >=32 字节的数据对象按 32 字节
 * 对齐，而本结构 40 字节，直接排布会在表项之间留下 24 字节空洞，按 sizeof
 * 索引就会落进填充区。改存 8 字节指针即天然紧密排列，这也是内核既有表
 * （如 __start___tracepoints_ptrs）的通行做法。
 */
extern struct ikaslr_tramp *__start_ikaslr_tramp_tbl[], *__end_ikaslr_tramp_tbl[];
extern void *__start_ikaslr_target[], *__end_ikaslr_target[];
extern void *__start_ikaslr_whitelist[], *__end_ikaslr_whitelist[];

/* ---- 段注解 ---- */
#define __ikaslr_tramp_sec(fn) __section(".tramp.text." #fn) noinline
#define __ikaslr_rand_sec(fn)  __section(".rand.text." #fn) noinline

/* 随机化函数体：真实代码，落在 .rand.text，可被迁移。*/
#define IKASLR_RAND_FN(ret, fn, ...)					\
	ret fn##_body(__VA_ARGS__);					\
	ret __ikaslr_rand_sec(fn) fn##_body(__VA_ARGS__)

/*
 * fixed_in 跳板：保留原函数名与地址（固定），并发出该函数的 target 槽与表项。
 * 跳板体由调用方按上面的范式书写（enter -> 经 target 间接调用 -> leave）。
 */
#define IKASLR_TRAMP_FN(ret, fn, ...)					\
	ret fn##_body(__VA_ARGS__);					\
	ret fn(__VA_ARGS__);						\
	void *__ikaslr_target_##fn					\
		__attribute__((section(".data..ikaslr_target"))) __used	\
		= (void *)fn##_body;					\
	static struct ikaslr_tramp __ikaslr_ent_##fn = {		\
			.tramp	= (void *)fn,				\
			.body	= (void *)fn##_body,			\
			.target = &__ikaslr_target_##fn,		\
			.name	= #fn,					\
		};							\
	static struct ikaslr_tramp *__ikaslr_ptr_##fn			\
		__attribute__((section(".data..ikaslr_tramp_tbl"))) __used \
		= &__ikaslr_ent_##fn;					\
	ret __ikaslr_tramp_sec(fn) fn(__VA_ARGS__)

/*
 * 在跳板体内取该函数当前的入口地址。READ_ONCE 保证读到随机化线程写入的最新值。
 * 类型由 <fn>_body 推出，因此调用点保持完全的类型检查。
 */
#define IKASLR_TARGET(fn)						\
	((typeof(&fn##_body))READ_ONCE(__ikaslr_target_##fn))

/* target 槽在跳板体外的引用（例如 fixed_out 或调试代码）需要此声明。*/
#define IKASLR_DECLARE_TARGET(fn) extern void *__ikaslr_target_##fn

/*
 * 登记一个被随机化代码调用的外部函数为合法的跨区域目标（§3.5.2）。
 * 链接器把这些项汇总成白名单，fixed_out 在放行前查询。
 */
#define IKASLR_WHITELIST(fn)						\
	static void *__ikaslr_wl_##fn					\
		__attribute__((section(".data..ikaslr_whitelist"))) __used \
		= (void *)fn

/*
 * fixed_out 跳板：从随机化区域离开。按 §3.4.2 依次执行
 *   pop（记录离开随机化区域）-> 白名单检查 -> 转移 -> 返回后重新进入。
 * 第 5 章在同一位置叠加 PA 验证。
 */
#define IKASLR_OUT_CALL(fn, ...)					\
({									\
	typeof(fn(__VA_ARGS__)) __out_ret;				\
	ikaslr_out_enter((void *)fn);					\
	__out_ret = fn(__VA_ARGS__);					\
	ikaslr_out_leave();						\
	__out_ret;							\
})

#define IKASLR_OUT_CALL_VOID(fn, ...)					\
do {									\
	ikaslr_out_enter((void *)fn);					\
	fn(__VA_ARGS__);						\
	ikaslr_out_leave();						\
} while (0)

#ifdef CONFIG_IKASLR

/*
 * 线程追踪（S1.3）。控制流经 fixed_in 进入随机化区域时 enter、经 fixed_out 或
 * 跳板返回时 leave。论文 §3.4.5：活跃集合为空即可安全切换代码页。
 */
void ikaslr_enter(void);
void ikaslr_leave(void);

int ikaslr_nr_funcs(void);

/* 更新单个函数的 target 槽（随机化时唯一需要改写的索引）。供 S1.4 使用。*/
void ikaslr_update_target(struct ikaslr_tramp *t, void *new_body);

/*
 * 触发一次重随机化（S1.4/S1.5）。完整流程见论文 §3.4.7。
 */
int ikaslr_rerandomize(void);

/*
 * 随机化的统一触发入口（第 4 章检测模块调用）。可在任意上下文调用：
 * 处于安全点则就地执行，否则推迟到进程上下文（§3.4.6 策略二）。
 */
int ikaslr_request_rerandomize(void);

/* 白名单查询：目标是否为编译期登记的合法跨区域目标。*/
bool ikaslr_whitelist_ok(void *target);
int ikaslr_whitelist_count(void);

/*
 * fixed_out 的进出。out_enter 记录"控制流离开随机化区域去执行外部函数"
 * 并做白名单检查；out_leave 记录回到区域内。
 */
void ikaslr_out_enter(void *target);
void ikaslr_out_leave(void);

#else  /* !CONFIG_IKASLR */

static inline void ikaslr_enter(void) { }
static inline void ikaslr_leave(void) { }
static inline int ikaslr_nr_funcs(void) { return 0; }
static inline int ikaslr_rerandomize(void) { return 0; }
static inline int ikaslr_request_rerandomize(void) { return 0; }
static inline bool ikaslr_whitelist_ok(void *target) { return true; }
static inline int ikaslr_whitelist_count(void) { return 0; }
static inline void ikaslr_out_enter(void *target) { }
static inline void ikaslr_out_leave(void) { }

#endif /* CONFIG_IKASLR */

#endif /* __ASSEMBLY__ */
#endif /* _LINUX_IKASLR_H */
