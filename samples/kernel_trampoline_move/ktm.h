#ifndef _KTM_H_
#define _KTM_H_

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <asm/cacheflush.h>
#include <linux/set_memory.h>
#include <linux/kdebug.h>
#include <linux/delay.h>

#define PROC_NAME    "ktm_ctl"
#define STATS_NAME   "ktm_stats"

enum ktm_state { KTM_NORMAL = 0, KTM_EXCEPTION = 1 };

/* 全局状态与统计变量 */
extern atomic_t state;
extern atomic_t in_count_func1, in_count_func2, out_count;
DECLARE_PER_CPU(int, percpu_in1); // 自带 extern
DECLARE_PER_CPU(int, percpu_in2);
extern wait_queue_head_t exception_wq;
extern wait_queue_head_t zero_wq;

/* 函数声明 */
void func0(void);
void func1(void);
void func2(void);
void func3(void);
void dse(int);

/* 汇编标签 */
extern char func1_start[], func1_end[];
extern char func2_start[], func2_end[];

/* trampoline 中使用的函数指针 */
extern void (*func1_ptr)(void);
extern void (*func2_ptr)(void);

/* 函数初始位置 */
extern unsigned long func1_orig;
extern unsigned long func2_orig;

/* 随机化前的位置 */
extern unsigned long func1_old;
extern unsigned long func2_old;

/* 随机化后的位置 */
extern unsigned long func1_new;
extern unsigned long func2_new;

/* 函数长度 */
extern unsigned long func1_len;
extern unsigned long func2_len;

int move_function_and_udf(void *func, size_t len, void **newp);
void do_exception_and_move(void);

int ktm_register_die_notifier(void);
void ktm_unregister_die_notifier(void);

#endif /* _KTM_H_ */
