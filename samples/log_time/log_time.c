#include "asm/ptrace.h"
#include "linux/platform_data/cros_ec_commands.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#define LOG_ENTRIES 1000000

struct trace_entry {
    u64 time_ns;
    const char *func;
    u8 type; // 0=entry, 1=exit
};

/* 用于标记是否循环完成 */
struct loop_state {
    u64 n;              /* trace_index */
    bool initialized;   /* 是否已经初始化完成 */
    u64 op_pos;         /* 输出起点 [ */
    u64 ed_pos;         /* 输出终点 ] */
    bool wrapped;       /* 是否已经从尾部回到开头 */
};

static struct trace_entry *trace_buf;
static atomic64_t trace_index = ATOMIC64_INIT(0);
static bool log_ready = false;

static void switch_log_ready(void)
{
    log_ready = !log_ready;
}

static inline u64 read_cntvct(void)
{
    u64 cnt;
    asm volatile("mrs %0, cntvct_el0" : "=r"(cnt));
    return cnt;
}

static inline u64 read_cntfrq(void)
{
    u64 freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

static inline u64 cntvct_to_ns(u64 cnt)
{
    static u64 freq;
    if (unlikely(!freq)) {
        freq = read_cntfrq();
    }
    return (cnt * 1000000000ULL) / freq;
}

void log_time(const char *func, bool is_exit)
{
    if (!log_ready) {
        return;
    }

    u64 idx = atomic64_fetch_add(1, &trace_index);
    struct trace_entry *e = &trace_buf[idx % LOG_ENTRIES];

    e->time_ns = cntvct_to_ns(read_cntvct());
    e->func = func;
    e->type = is_exit;
}
EXPORT_SYMBOL(log_time);

static ssize_t trace_write(struct file *file, const char __user *buf,
                           size_t count, loff_t *ppos)
{
    char kbuf[32];

    if (count >= sizeof(kbuf)) {
        return -EINVAL;
    }
    if (copy_from_user(kbuf, buf, count)) {
        return -EFAULT;
    }
    kbuf[count] = '\0';

    if (strncmp(kbuf, "ready", 5) == 0) {
        switch_log_ready();
        if (log_ready) {
            pr_info("log_time: switch log to ready\n");
        }
        else {
            pr_info("log_time: switch log to stop\n");
        }
    } else {
        pr_info("log_time: unknown command '%s'\n", kbuf);
    }
    
    return count;
}

static void *trace_seq_start(struct seq_file *s, loff_t *pos)
{
    struct loop_state *st = s->private;
    
    // pr_info("trace_seq_start: st->wrapped = %d\n", st->wrapped);
    // pr_info("trace_seq_start: *pos = %d\n", *pos);
    if (st->wrapped && *pos >= st->ed_pos) {
        return NULL;
    }

    if (!st->initialized) {
        st->initialized = true;
        if (st->n >= LOG_ENTRIES) {
            st->op_pos = (st->n - 1) % LOG_ENTRIES;
            st->ed_pos = (st->n - 2) % LOG_ENTRIES;
            st->wrapped = false;
        } else {
            st->op_pos = 0;
            st->ed_pos = st->n - 1;
            st->wrapped = true;
        }
        // pr_info("trace_seq_start: st->op_pos = %d\n", st->op_pos);
        // pr_info("trace_seq_start: st->ed_pos = %d\n", st->ed_pos);
        *pos = (loff_t)st->op_pos;
    }
    return pos;
}

static void *trace_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
    struct loop_state *st = s->private;
    
    ++*pos;
    if (*pos > st->ed_pos) {
        return NULL;
    }

    if (*pos == LOG_ENTRIES) {
        st->wrapped = true;
    }
    *pos = *pos % LOG_ENTRIES;
    
    return pos;
}
static void trace_seq_stop(struct seq_file *s, void *v) { }

static int trace_seq_show(struct seq_file *s, void *v)
{
    struct trace_entry *e = &trace_buf[*(loff_t*)v];
    seq_printf(s, "%llu | %llu | %s | %s\n",
               *(loff_t*)v, e->time_ns,
               e->func ? e->func : "(null)",
               e->type == 0 ? "entry" : "exit");
    return 0;
}

static const struct seq_operations trace_seq_ops = {
    .start = trace_seq_start,
    .next  = trace_seq_next,
    .stop  = trace_seq_stop,
    .show  = trace_seq_show
};

static int trace_open(struct inode *inode, struct file *file)
{   
    struct loop_state *st;
    struct seq_file *seq;
    int ret;

    st = kzalloc(sizeof(*st), GFP_KERNEL);
    if (!st) {
        return -ENOMEM;
    }

    ret = seq_open(file, &trace_seq_ops);
    if (ret) {
        kfree(st);
        return ret;
    }

    seq = file->private_data;
    st->n = atomic64_read(&trace_index);
    seq->private = st;
    return 0;
}

static int trace_seq_release(struct inode *inode, struct file *file)
{
    struct seq_file *seq = file->private_data;
    kfree(seq->private);
    return seq_release(inode, file);
}

/* 
 * echo ready > /proc/timelog 
 * cat 前需要确保 timelog 暂停
 * cat /proc/timelog
 */
static const struct proc_ops trace_proc_ops = {
    .proc_read  = seq_read,
    .proc_write = trace_write,
    .proc_open  = trace_open,
    .proc_lseek = seq_lseek,
    .proc_release = trace_seq_release,
};

static int __init log_time_init(void)
{
    trace_buf = kvzalloc(LOG_ENTRIES * sizeof(struct trace_entry), GFP_KERNEL);
    if (!trace_buf)
        return -ENOMEM;

    proc_create("timelog", 0666, NULL, &trace_proc_ops);
    pr_info("log_time: initialized, buffer size=%lu entries\n", LOG_ENTRIES);
    return 0;
}

static void __exit log_time_exit(void)
{
    remove_proc_entry("timelog", NULL);
    kvfree(trace_buf);
    pr_info("log_time: exited\n");
}

module_init(log_time_init);
module_exit(log_time_exit);

MODULE_LICENSE("GPL");
