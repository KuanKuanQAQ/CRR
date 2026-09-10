// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 控制与观测接口（procfs）。
 *
 *   /proc/ikaslr/stats     只读：区域、变体池、随机化、推迟、兜底、白名单统计
 *   /proc/ikaslr/trigger   只写：写入任意内容触发一次随机化
 *   /proc/ikaslr/layout    只读：每个随机化函数的当前地址（调试用）
 *
 * 这些接口供第 4 章的按需触发实验与第 6 章的系统级实验驱动。
 *
 * 安全提示：layout 会暴露随机化区域的当前布局，等同于泄露随机化结果，
 * 因此仅在 CONFIG_IKASLR_DEBUG 下提供，且权限限定为 0400（仅 root 可读）。
 * 部署配置中不应启用。
 */
#define pr_fmt(fmt) "ikaslr: " fmt

#include <linux/ikaslr.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/init.h>

#include "internal.h"

static int ikaslr_stats_show(struct seq_file *m, void *v)
{
	struct ikaslr_stats ts;
	u64 last_ns, prep_ns;
	unsigned long rounds, missed, dcount, fok, ffail;
	u64 davg, dmax;
	int nready;

	ikaslr_get_stats(&ts);
	ikaslr_rand_stats(&last_ns, &rounds);
	ikaslr_pool_stats(&prep_ns, &missed, &nready);
	ikaslr_defer_stats(&dcount, &davg, &dmax);
	ikaslr_fixup_stats(&fok, &ffail);
	{
		unsigned long ab, ag, at;

		ikaslr_detect_stats(&ab, &ag, &at);
		seq_printf(m, "audit_benign       %lu\n", ab);
		seq_printf(m, "audit_gadget       %lu\n", ag);
		seq_printf(m, "audit_triggers     %lu\n", at);
		{
			unsigned long bn, bm, gn, gm;

			ikaslr_detect_latency(&bn, &bm, &gn, &gm);
			/* 累计纳秒与最大值；平均值由 用户态 除以对应计数得到，
			 * 分位数靠多次采样差分——见 perf/13-detect-cost.sh。*/
			seq_printf(m, "audit_benign_ns    %lu\n", bn);
			seq_printf(m, "audit_benign_max   %lu\n", bm);
			seq_printf(m, "audit_gadget_ns    %lu\n", gn);
			seq_printf(m, "audit_gadget_max   %lu\n", gm);
		}
	}

	seq_printf(m, "functions          %d\n", ikaslr_nr_funcs());
	seq_printf(m, "rand_region_bytes  %lu\n",
		   (unsigned long)(__rand_text_end - __rand_text_start));
	seq_printf(m, "tramp_region_bytes %lu\n",
		   (unsigned long)(__tramp_text_end - __tramp_text_start));
	seq_printf(m, "whitelist_entries  %d\n", ikaslr_whitelist_count());
	seq_printf(m, "whitelist_rejects  %lu\n", ikaslr_whitelist_rejects());
	seq_puts(m, "\n");
	seq_printf(m, "active             %d\n", ikaslr_active_count());
	seq_printf(m, "inside             %d\n", ikaslr_inside_count());
	seq_printf(m, "enters             %lu\n", ts.enters);
	seq_printf(m, "outs               %lu\n", ts.outs);
	seq_printf(m, "enter_backoffs     %lu\n", ts.backoffs);
	seq_printf(m, "enter_forced       %lu\n", ts.forced);
	seq_printf(m, "outleave_blocked   %lu\n", ts.outleave_blocked);
	seq_printf(m, "nested_admitted    %lu\n", ts.nested_admitted);
	seq_printf(m, "wait_residual      %d\n", ts.wait_residual);
	seq_printf(m, "max_active         %d\n", ts.max_active);
	seq_puts(m, "\n");
	seq_printf(m, "rounds             %lu\n", rounds);
	seq_printf(m, "critical_path_ns   %llu\n", last_ns);
	{
		u64 w, u, r;

		ikaslr_rand_phases(&w, &u, &r);
		seq_printf(m, "  cp_wait_ns       %llu\n", w);
		seq_printf(m, "  cp_update_ns     %llu\n", u);
		seq_printf(m, "  cp_remap_ns      %llu\n", r);
	}
	seq_printf(m, "prepare_ns         %llu\n", prep_ns);
	seq_printf(m, "variants_ready     %d\n", nready);
	seq_printf(m, "rounds_missed      %lu\n", missed);
	seq_puts(m, "\n");
	seq_printf(m, "deferred           %lu\n", dcount);
	seq_printf(m, "defer_avg_ns       %llu\n", davg);
	seq_printf(m, "defer_max_ns       %llu\n", dmax);
	seq_puts(m, "\n");
	seq_printf(m, "stale_fixups       %lu\n", fok);
	seq_printf(m, "stale_fixup_fails  %lu\n", ffail);
	return 0;
}

static int ikaslr_stats_open(struct inode *i, struct file *f)
{
	return single_open(f, ikaslr_stats_show, NULL);
}

static const struct proc_ops ikaslr_stats_ops = {
	.proc_open	= ikaslr_stats_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static ssize_t ikaslr_trigger_write(struct file *f, const char __user *buf,
				    size_t len, loff_t *ppos)
{
	int ret = ikaslr_request_rerandomize();

	if (ret && ret != -EAGAIN && ret != -EBUSY)
		return ret;
	return len;
}

static const struct proc_ops ikaslr_trigger_ops = {
	.proc_write	= ikaslr_trigger_write,
};

#ifdef CONFIG_IKASLR_DEBUG
static int ikaslr_layout_show(struct seq_file *m, void *v)
{
	int i;

	for (i = 0; i < ikaslr_nr_funcs(); i++)
		seq_printf(m, "%-32s %px %zu\n", ikaslr_tbl[i]->name,
			   READ_ONCE(*ikaslr_tbl[i]->target), ikaslr_tbl[i]->size);
	return 0;
}

static int ikaslr_layout_open(struct inode *i, struct file *f)
{
	return single_open(f, ikaslr_layout_show, NULL);
}

static const struct proc_ops ikaslr_layout_ops = {
	.proc_open	= ikaslr_layout_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};
#endif

int __init ikaslr_control_init(void)
{
	struct proc_dir_entry *dir;

	dir = proc_mkdir("ikaslr", NULL);
	if (!dir)
		return -ENOMEM;

	if (!proc_create("stats", 0444, dir, &ikaslr_stats_ops))
		return -ENOMEM;
	if (!proc_create("trigger", 0200, dir, &ikaslr_trigger_ops))
		return -ENOMEM;
#ifdef CONFIG_IKASLR_DEBUG
	/* 布局等同于泄露随机化结果，仅调试配置提供且仅 root 可读。*/
	if (!proc_create("layout", 0400, dir, &ikaslr_layout_ops))
		return -ENOMEM;
	ikaslr_bench_init(dir);		/* §3.6.3 微观开销测量 */
#endif
	pr_info("procfs interface at /proc/ikaslr\n");
	return 0;
}
