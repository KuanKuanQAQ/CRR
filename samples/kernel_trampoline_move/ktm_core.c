#include "ktm.h"
#include <linux/errno.h>
#include <linux/seq_file.h>
#include <linux/bitmap.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Runkuan Li");
MODULE_DESCRIPTION("Prototype trampolines + move/test module (split)");

atomic_t state = ATOMIC_INIT(KTM_NORMAL);
atomic_t in_count_func1 = ATOMIC_INIT(0);
atomic_t in_count_func2 = ATOMIC_INIT(0);
atomic_t out_count = ATOMIC_INIT(0);
DEFINE_PER_CPU(int, percpu_in1);
DEFINE_PER_CPU(int, percpu_in2);
wait_queue_head_t exception_wq;
wait_queue_head_t zero_wq;

/* proc write handler
 * echo call > /proc/ktm_ctl
 * echo exception > /proc/ktm_ctl
*/
static ssize_t ktm_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    char kbuf[32];

    if (count >= sizeof(kbuf))
        return -EINVAL;
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;
    kbuf[count] = '\0';

    if (strncmp(kbuf, "call", 4) == 0) {
        pr_info("KTM: proc requested call -> invoking func0\n");
        func0();
    } else if (strncmp(kbuf, "exception", 9) == 0) {
        pr_info("KTM: proc requested exception sequence (todo)\n");
        do_exception_and_move();
    } else {
        pr_info("KTM: unknown command '%s'\n", kbuf);
    }

    return count;
}

static const struct proc_ops ktm_fops = {
    .proc_write = ktm_write,
};

static ssize_t ktm_stats_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    char kbuf[256];
    int len;

    len = snprintf(kbuf, sizeof(kbuf),
                   "state=%d in1=%d in2=%d out=%d percpu_in1=%d percpu_in2=%d\n",
                   atomic_read(&state), atomic_read(&in_count_func1), atomic_read(&in_count_func2),
                   atomic_read(&out_count), per_cpu(percpu_in1, 0), per_cpu(percpu_in2, 0));
    return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static const struct proc_ops ktm_stats_fops = {
    .proc_read = ktm_stats_read,
};

static int __init ktm_init(void)
{
    struct proc_dir_entry *ent;

    init_waitqueue_head(&exception_wq);
    init_waitqueue_head(&zero_wq);

    func1_ptr = func1;
    func2_ptr = func2;

    func1_orig = (unsigned long)func1;
    func2_orig = (unsigned long)func2;

    func1_old = func1_orig;
    func2_old = func2_orig;

    func1_len = func1_end - func1_start;
    func2_len = func2_end - func2_start;

    pr_info("KTM: func1 at %lx, func2 at %lx\n", func1_orig, func2_orig);
    pr_info("KTM: func1 length %lu, func2 length %lu\n", func1_len, func2_len);

    ent = proc_create(PROC_NAME, 0222, NULL, &ktm_fops);
    if (!ent) {
        pr_err("KTM: failed to create %s\n", PROC_NAME);
        return -ENOMEM;
    }
    ent = proc_create(STATS_NAME, 0444, NULL, &ktm_stats_fops);
    if (!ent) {
        pr_warn("KTM: failed to create %s\n", STATS_NAME);
    }

    ktm_register_die_notifier();

    pr_info("KTM: module loaded (split)\n");
    return 0;
}

static void __exit ktm_exit(void)
{
    ktm_unregister_die_notifier();
    remove_proc_entry(PROC_NAME, NULL);
    remove_proc_entry(STATS_NAME, NULL);
    pr_info("KTM: module unloaded\n");
}

module_init(ktm_init);
module_exit(ktm_exit);
