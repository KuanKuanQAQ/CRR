// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 微基准（arm64）：跳板的常驻税到底花在哪。
 *
 * 两组数，对照着读：
 *
 *   seq  —— microbench_asm.S 里的分项循环：把跳板里的一段指令原样放进计数循环，
 *           量序列本身的成本（读写内外标志、查阻断、屏障、LL/SC 加一、LSE 加一、
 *           完整进入序列带/不带屏障）。无调用无返回、分支全可预测，是下界。
 *
 *   call —— 经**真实跳板**调用本文件里被随机化的函数，与函数体逐字相同的普通
 *           函数（*_base）对照，差值即一次往返的跳板税：
 *             leaf   区域外 → F → 返回                （入口跳板，跨边界）
 *             out    再加 F 体内调用外部函数 G         （+ 出口跳板 + 标记解析）
 *             in     再加 F 体内调用被随机化的 B       （+ 区域内部调用）
 *             ind    再加 F 体内经函数指针调用 G       （+ 间接出口 thunk）
 *             poly   四个函数轮流、各两处调用点         （标记解析的两处间接分支
 *                    面对多个目标时的预测表现；out 是它的单目标最好情形）
 *
 * 被随机化的那几个函数由 LLVM pass 按固定名字自动加入名单（见 IKaslrPass.cpp 的
 * loadFuncListOrdered），不需要改 IKASLR_FUNCS 文件。以 CRR_TRAMPOLINE=n 构建时
 * 它们就是普通函数，call 组的差值应当为零——这本身是对测量方法的一次校验。
 *
 * 每项跑 batches 批、每批 iters 次，报告每次调用的纳秒数（各批的最小值与中位数）
 * 以及 PMU 的周期数、指令数、分支预测失败数（全部批次的平均；拿不到 PMU 时为 -1，
 * QEMU TCG 即如此）。
 *
 * 用法：
 *   echo "<iters> [batches]" > /proc/ikaslr/microbench     # 可选，默认 100000 9
 *   cat /proc/ikaslr/microbench > result.csv               # 运行并输出
 * `#` 开头的行是平台与配置说明，其余是 CSV。数字只有在真机上才有意义。
 */
#define pr_fmt(fmt) "ikaslr: " fmt

#include <linux/ikaslr.h>
#include <linux/kernel.h>
#include <linux/cache.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/perf_event.h>
#include <linux/preempt.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/uaccess.h>
#include <asm/cputype.h>
#include <asm/cpufeature.h>

#include "internal.h"

/* 分项循环的专用计数：与 ikaslr_cnt 同布局（64 字节一项，按 cpu<<6 寻址）。*/
struct ikaslr_mb_cnt {
	u64 v[8];
} ____cacheline_aligned;
struct ikaslr_mb_cnt ikaslr_mb_cnt[NR_CPUS];

void ikaslr_mb_loop_empty(unsigned long n);
void ikaslr_mb_loop_flag(unsigned long n);
void ikaslr_mb_loop_blocked(unsigned long n);
void ikaslr_mb_loop_dmb(unsigned long n);
void ikaslr_mb_loop_count_llsc(unsigned long n);
void ikaslr_mb_loop_count_lse(unsigned long n);
void ikaslr_mb_loop_count_plain(unsigned long n);
void ikaslr_mb_loop_enter_seq(unsigned long n);
void ikaslr_mb_loop_enter_seq_nodmb(unsigned long n);

static inline bool mb_have_lse(void)
{
	return cpus_have_final_cap(ARM64_HAS_LSE_ATOMICS);
}

/* 被测函数必须是外部链接（pass 按名字找它们、出口跳板按名字 `bl` 它们）。*/
long ikaslr_mb_ext(long x);
long ikaslr_mb_ext2(long x);
long ikaslr_mb_leaf(long x);
long ikaslr_mb_leaf_base(long x);
long ikaslr_mb_out(long x);
long ikaslr_mb_out_base(long x);
long ikaslr_mb_in(long x);
long ikaslr_mb_in_base(long x);
long ikaslr_mb_ind(long (*fn)(long), long x);
long ikaslr_mb_ind_base(long (*fn)(long), long x);

/*
 * ---- 被测函数 ----
 * 外部函数（不随机化）。`+ 1` 之类的尾巴是为了不让调用变成尾调用。
 */
noinline long ikaslr_mb_ext(long x)  { return x + 1; }
noinline long ikaslr_mb_ext2(long x) { return x + 2; }

/* 被随机化的（名字固定，见文件头）与逐字相同的对照。*/
noinline long ikaslr_mb_leaf(long x)      { return x * 3 + 1; }
noinline long ikaslr_mb_leaf_base(long x) { return x * 3 + 1; }

noinline long ikaslr_mb_out(long x)      { return ikaslr_mb_ext(x) + 1; }
noinline long ikaslr_mb_out_base(long x) { return ikaslr_mb_ext(x) + 1; }

noinline long ikaslr_mb_in(long x)      { return ikaslr_mb_leaf(x) + 1; }
noinline long ikaslr_mb_in_base(long x) { return ikaslr_mb_leaf_base(x) + 1; }

noinline long ikaslr_mb_ind(long (*fn)(long), long x)      { return fn(x) + 1; }
noinline long ikaslr_mb_ind_base(long (*fn)(long), long x) { return fn(x) + 1; }

#define MB_POLY(name)							\
long name(long x);							\
noinline long name(long x)						\
{									\
	return ikaslr_mb_ext(x) + ikaslr_mb_ext2(x ^ 5);		\
}
MB_POLY(ikaslr_mb_poly0)
MB_POLY(ikaslr_mb_poly1)
MB_POLY(ikaslr_mb_poly2)
MB_POLY(ikaslr_mb_poly3)
MB_POLY(ikaslr_mb_poly0_base)
MB_POLY(ikaslr_mb_poly1_base)
MB_POLY(ikaslr_mb_poly2_base)
MB_POLY(ikaslr_mb_poly3_base)

/* ---- 每项的循环体：n 次调用，结果串起来防止被优化掉 ---- */
static long mb_sink;

#define MB_RUN1(tag, expr)						\
static noinline void mb_run_##tag(unsigned long n)			\
{									\
	long acc = READ_ONCE(mb_sink);					\
	unsigned long i;						\
									\
	for (i = 0; i < n; i++)						\
		acc = (expr);						\
	WRITE_ONCE(mb_sink, acc);					\
}

MB_RUN1(leaf,       ikaslr_mb_leaf(acc))
MB_RUN1(leaf_base,  ikaslr_mb_leaf_base(acc))
MB_RUN1(out,        ikaslr_mb_out(acc))
MB_RUN1(out_base,   ikaslr_mb_out_base(acc))
MB_RUN1(in,         ikaslr_mb_in(acc))
MB_RUN1(in_base,    ikaslr_mb_in_base(acc))
MB_RUN1(ind,        ikaslr_mb_ind(ikaslr_mb_ext, acc))
MB_RUN1(ind_base,   ikaslr_mb_ind_base(ikaslr_mb_ext, acc))

/* poly：每次迭代四个函数各调一次，报告时按四次调用折算。*/
static noinline void mb_run_poly(unsigned long n)
{
	long acc = READ_ONCE(mb_sink);
	unsigned long i;

	for (i = 0; i < n; i += 4) {
		acc = ikaslr_mb_poly0(acc);
		acc = ikaslr_mb_poly1(acc);
		acc = ikaslr_mb_poly2(acc);
		acc = ikaslr_mb_poly3(acc);
	}
	WRITE_ONCE(mb_sink, acc);
}

static noinline void mb_run_poly_base(unsigned long n)
{
	long acc = READ_ONCE(mb_sink);
	unsigned long i;

	for (i = 0; i < n; i += 4) {
		acc = ikaslr_mb_poly0_base(acc);
		acc = ikaslr_mb_poly1_base(acc);
		acc = ikaslr_mb_poly2_base(acc);
		acc = ikaslr_mb_poly3_base(acc);
	}
	WRITE_ONCE(mb_sink, acc);
}

struct mb_case {
	const char *group;	/* seq / call */
	const char *name;
	const char *ref;	/* 报告差值时减去哪一项 */
	void (*run)(unsigned long n);
	bool need_lse;
};

static const struct mb_case mb_cases[] = {
	{ "seq",  "empty",            NULL,        ikaslr_mb_loop_empty },
	{ "seq",  "flag_rw",          "empty",     ikaslr_mb_loop_flag },
	{ "seq",  "blocked_check",    "empty",     ikaslr_mb_loop_blocked },
	{ "seq",  "dmb_ish",          "empty",     ikaslr_mb_loop_dmb },
	{ "seq",  "count_plain",      "empty",     ikaslr_mb_loop_count_plain },
	{ "seq",  "count_llsc",       "empty",     ikaslr_mb_loop_count_llsc },
	{ "seq",  "count_lse",        "empty",     ikaslr_mb_loop_count_lse, true },
	{ "seq",  "enter_seq",        "empty",     ikaslr_mb_loop_enter_seq },
	{ "seq",  "enter_seq_nodmb",  "empty",     ikaslr_mb_loop_enter_seq_nodmb },
	{ "call", "leaf_base",        NULL,        mb_run_leaf_base },
	{ "call", "leaf",             "leaf_base", mb_run_leaf },
	{ "call", "out_base",         NULL,        mb_run_out_base },
	{ "call", "out",              "out_base",  mb_run_out },
	{ "call", "in_base",          NULL,        mb_run_in_base },
	{ "call", "in",               "in_base",   mb_run_in },
	{ "call", "ind_base",         NULL,        mb_run_ind_base },
	{ "call", "ind",              "ind_base",  mb_run_ind },
	{ "call", "poly_base",        NULL,        mb_run_poly_base },
	{ "call", "poly",             "poly_base", mb_run_poly },
};

/* 每次调用的定点值（×1000），-1 表示没有。*/
struct mb_result {
	s64 ns_min, ns_med;
	s64 cycles, instr, brmiss;
	bool skipped;
};

static unsigned long mb_iters = 100000;
static unsigned int mb_batches = 9;
static DEFINE_MUTEX(mb_lock);

enum { MB_CYCLES, MB_INSTR, MB_BRMISS, MB_NR_EV };
static const u64 mb_ev_cfg[MB_NR_EV] = {
	PERF_COUNT_HW_CPU_CYCLES,
	PERF_COUNT_HW_INSTRUCTIONS,
	PERF_COUNT_HW_BRANCH_MISSES,
};

static struct perf_event *mb_open_event(u64 config)
{
	struct perf_event_attr attr = {
		.type		= PERF_TYPE_HARDWARE,
		.size		= sizeof(attr),
		.config		= config,
		.pinned		= 1,
		.exclude_user	= 1,
		.exclude_hv	= 1,
	};
	struct perf_event *ev;

	ev = perf_event_create_kernel_counter(&attr, -1, current, NULL, NULL);
	return IS_ERR(ev) ? NULL : ev;
}

static u64 mb_read_event(struct perf_event *ev)
{
	u64 enabled, running;

	return ev ? perf_event_read_value(ev, &enabled, &running) : 0;
}

static int mb_cmp_s64(const void *a, const void *b)
{
	s64 x = *(const s64 *)a, y = *(const s64 *)b;

	return (x > y) - (x < y);
}

static void mb_measure(const struct mb_case *c, struct perf_event **ev,
		       s64 *scratch, struct mb_result *r)
{
	u64 pm0[MB_NR_EV], pm1[MB_NR_EV];
	unsigned long n = mb_iters & ~3UL;	/* poly 一次迭代四个调用 */
	unsigned int b;
	int e;

	memset(r, 0, sizeof(*r));
	if (c->need_lse && !mb_have_lse()) {
		r->skipped = true;
		return;
	}

	c->run(n / 8 + 4);			/* 预热：缓存、分支预测器 */

	for (e = 0; e < MB_NR_EV; e++)
		pm0[e] = mb_read_event(ev[e]);
	for (b = 0; b < mb_batches; b++) {
		u64 t0 = ktime_get_ns();

		c->run(n);
		scratch[b] = div64_u64((ktime_get_ns() - t0) * 1000, n);
		cond_resched();
	}
	for (e = 0; e < MB_NR_EV; e++)
		pm1[e] = mb_read_event(ev[e]);

	sort(scratch, mb_batches, sizeof(*scratch), mb_cmp_s64, NULL);
	r->ns_min = scratch[0];
	r->ns_med = scratch[mb_batches / 2];

#define MB_PER_CALL(idx) \
	(ev[idx] ? (s64)div64_u64((pm1[idx] - pm0[idx]) * 1000, \
				  (u64)n * mb_batches) : -1)
	r->cycles = MB_PER_CALL(MB_CYCLES);
	r->instr  = MB_PER_CALL(MB_INSTR);
	r->brmiss = MB_PER_CALL(MB_BRMISS);
#undef MB_PER_CALL
}

static void mb_put_fixed(struct seq_file *m, s64 v)
{
	if (v < 0)
		seq_puts(m, ",-1");
	else
		seq_printf(m, ",%lld.%03lld", v / 1000, v % 1000);
}

static const struct mb_result *mb_find(const struct mb_result *res, const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mb_cases); i++)
		if (!strcmp(mb_cases[i].name, name))
			return &res[i];
	return NULL;
}

static int ikaslr_mb_show(struct seq_file *m, void *v)
{
	struct perf_event *ev[MB_NR_EV];
	struct mb_result *res;
	bool have_pmu;
	s64 *scratch;
	int i, e;

	guard(mutex)(&mb_lock);

	res = kcalloc(ARRAY_SIZE(mb_cases), sizeof(*res), GFP_KERNEL);
	scratch = kcalloc(mb_batches, sizeof(*scratch), GFP_KERNEL);
	if (!res || !scratch) {
		kfree(res);
		kfree(scratch);
		return -ENOMEM;
	}

	/* 钉在当前核上：计数落本核缓存行，PMU 计数器也绑在本任务上。*/
	migrate_disable();
	for (e = 0; e < MB_NR_EV; e++)
		ev[e] = mb_open_event(mb_ev_cfg[e]);

	for (i = 0; i < ARRAY_SIZE(mb_cases); i++)
		mb_measure(&mb_cases[i], ev, scratch, &res[i]);

	have_pmu = ev[MB_CYCLES] != NULL;
	for (e = 0; e < MB_NR_EV; e++)
		if (ev[e])
			perf_event_release_kernel(ev[e]);

	seq_puts(m, "# I-KASLR trampoline microbenchmark (per-call values)\n");
	seq_printf(m, "# midr: 0x%08x  cpu: %d  online_cpus: %u\n",
		   read_cpuid_id(), raw_smp_processor_id(), num_online_cpus());
	migrate_enable();
	seq_printf(m, "# count_mode: %s  atomic: llsc  lse_available: %d\n",
		   IS_ENABLED(CONFIG_IKASLR_COUNT_GLOBAL) ? "global" : "percpu",
		   mb_have_lse() ? 1 : 0);
	seq_printf(m, "# randomized_functions: %d  iters: %lu  batches: %u\n",
		   ikaslr_nr_funcs(), mb_iters & ~3UL, mb_batches);
	seq_printf(m, "# pmu: %s\n", have_pmu ? "yes" :
		   "no (cycles/instr/br_miss are -1)");
	seq_puts(m, "# delta_* = this row minus its ref row (the trampoline tax)\n");
	seq_puts(m, "group,case,ref,ns_min,ns_med,cycles,instr,br_miss,delta_ns_med,delta_cycles,delta_br_miss\n");

	for (i = 0; i < ARRAY_SIZE(mb_cases); i++) {
		const struct mb_case *c = &mb_cases[i];
		const struct mb_result *r = &res[i], *ref;

		if (r->skipped) {
			seq_printf(m, "%s,%s,%s,skipped\n", c->group, c->name,
				   c->ref ?: "");
			continue;
		}
		seq_printf(m, "%s,%s,%s", c->group, c->name, c->ref ?: "");
		mb_put_fixed(m, r->ns_min);
		mb_put_fixed(m, r->ns_med);
		mb_put_fixed(m, r->cycles);
		mb_put_fixed(m, r->instr);
		mb_put_fixed(m, r->brmiss);

		ref = c->ref ? mb_find(res, c->ref) : NULL;
		if (ref && !ref->skipped) {
			/* 差值可能为负（噪声），定点打印前先处理符号。*/
			s64 d[3] = { r->ns_med - ref->ns_med,
				     r->cycles < 0 ? S64_MIN : r->cycles - ref->cycles,
				     r->brmiss < 0 ? S64_MIN : r->brmiss - ref->brmiss };
			int k;

			for (k = 0; k < 3; k++) {
				if (d[k] == S64_MIN)
					seq_puts(m, ",-1");
				else
					seq_printf(m, ",%s%lld.%03lld",
						   d[k] < 0 ? "-" : "",
						   abs(d[k]) / 1000, abs(d[k]) % 1000);
			}
		} else {
			seq_puts(m, ",,,");
		}
		seq_putc(m, '\n');
	}

	kfree(res);
	kfree(scratch);
	return 0;
}

static ssize_t ikaslr_mb_write(struct file *f, const char __user *ubuf,
			       size_t len, loff_t *ppos)
{
	unsigned long iters;
	unsigned int batches = 0;
	char buf[48];
	int n;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	n = sscanf(buf, "%lu %u", &iters, &batches);
	if (n < 1)
		return -EINVAL;

	guard(mutex)(&mb_lock);
	mb_iters = clamp(iters, 64UL, 100000000UL);
	if (n == 2)
		mb_batches = clamp(batches, 1U, 101U);
	return len;
}

static int ikaslr_mb_open(struct inode *i, struct file *f)
{
	return single_open(f, ikaslr_mb_show, NULL);
}

static const struct proc_ops ikaslr_mb_ops = {
	.proc_open	= ikaslr_mb_open,
	.proc_read	= seq_read,
	.proc_write	= ikaslr_mb_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

int __init ikaslr_microbench_init(struct proc_dir_entry *dir)
{
	return proc_create("microbench", 0600, dir, &ikaslr_mb_ops) ? 0 : -ENOMEM;
}
