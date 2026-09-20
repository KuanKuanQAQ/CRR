#include "ktm.h"

struct move_ctx {
	void *orig;      /* 原始函数地址 */
	void *dest;      /* 新分配地址 */
	size_t len;      /* 需拷贝的字节数 */
	unsigned long orig_page; /* 原页起始 */
	int orig_npages;
	unsigned long dest_page;
	int dest_npages;
	int error;
};

void do_exception_and_move(void)
{
    unsigned long total_in;

    pr_info("KTM: switching to EXCEPTION state\n");
    atomic_set(&state, KTM_EXCEPTION);

    /* 确保 state 的更新同步到所有 cpu */
    smp_mb();

    /* 等待控制流离开 */
    total_in = atomic_read(&in_count_func1) + atomic_read(&in_count_func2);
    while (total_in != 0) {
        pr_info("KTM: waiting for in-counts to reach zero (currently %lu)\n", total_in);
        wait_event_interruptible(zero_wq,
                                 (atomic_read(&in_count_func1) + atomic_read(&in_count_func2)) == 0);
        total_in = atomic_read(&in_count_func1) + atomic_read(&in_count_func2);
        if (signal_pending(current)) {
            pr_warn("KTM: interrupted while waiting\n");
            break;
        }
    }

    pr_info("KTM: all active flows left func1/func2.\n");
    
    void *tmp_new = NULL;
    move_function_and_udf(func1, func1_len, &tmp_new);
    func1_new = (unsigned long)tmp_new;
	pr_info("KTM: func1_new: %px", (void *)func1_new);
	pr_info("KTM: func1_old: %px", (void *)func1_old);

    /* 更新 trampoline 使用的函数指针 */
    if (func1_new != func1_old) {
        WRITE_ONCE(func1_ptr, (void (*)(void))func1_new);
		func1_old = func1_new;
    }

    if (func2_new != func2_old) {
        WRITE_ONCE(func2_ptr, (void (*)(void))func2_new);
		func2_old = func2_new;
    }

    /* Ensure changes visible and icache coherent */
    smp_mb();

    pr_info("KTM: switch back to NORMAL state\n");
    atomic_set(&state, KTM_NORMAL);
    smp_mb();
    wake_up_all(&exception_wq);
}

static int move_worker(void *arg)
{
	struct move_ctx *c = arg;
	void *orig = c->orig;
	void *dest = c->dest;
	size_t len = c->len;
	unsigned long ostart = c->orig_page;
	unsigned long dstart = c->dest_page;
	int ret;

	/* 1) 使原页面可写（如果不是） */
	ret = set_memory_rw(ostart, c->orig_npages);
	if (ret) {
		pr_err("move_func: set_memory_rw(orig) failed: %d\n", ret);
		c->error = ret;
		return ret;
	}

	/* 2) 如果新页不是可写也尝试设为可写（通常 PAGE_KERNEL_EXEC 可能已含 RW） */
	ret = set_memory_rw(dstart, c->dest_npages);
	if (ret) {
		pr_err("move_func: set_memory_rw(dest) failed: %d\n", ret);
		/* 尝试恢复原页权限再退出 */
		set_memory_rox(ostart, c->orig_npages);
		c->error = ret;
		return ret;
	}

	/* 3) 拷贝函数字节到新地址 */
	/* error: 需要重定位移动后的函数代码段 */
	memcpy(dest, orig, len);

	/* 4) 刷新新地址的指令缓存 */
	flush_icache_range((unsigned long)dest, (unsigned long)dest + len);

	/* 5) 在原地址填充 udf #0 (0x00000000) —— 填满函数长度 */
	memset(orig, 0x00, len);

	/* 刷新原地址的指令缓存以确保所有 CPU 可见 */
	flush_icache_range((unsigned long)orig, (unsigned long)orig + len);

	/* 6) 恢复页面权限：把原页设为只可执行（RX），把新页也设为 RX（W^X） */
	ret = set_memory_rox(ostart, c->orig_npages);
	if (ret) {
        pr_warn("move_func: set_memory_rox(orig) failed: %d\n", ret);
    }

	ret = set_memory_rox(dstart, c->dest_npages);
	if (ret) {
        pr_warn("move_func: set_memory_rox(dest) failed: %d\n", ret);
    }

	c->error = 0;
	return 0;
}

/*
 * 把函数 [func, func+len) 搬到新 vmalloc 可执行内存，并把原地填 udf。
 * 输入:
 *   func:   原函数地址
 *   len:    需移动的字节长度（必须保证覆盖整个函数体）
 * 输出:
 *   *newp:  新地址（如果不为 NULL）
 * 返回:
 *   0 成功，负错误码失败
 */
int move_function_and_udf(void *func, size_t len, void **newp)
{
	void *new_area;
	unsigned long alloc_size;
	struct move_ctx ctx;
	unsigned long orig_start_page;
	int orig_npages;
	unsigned long dest_start_page;
	int dest_npages;
	int ret;

	if (!func || len == 0)
		return -EINVAL;

	/* 分配页对齐大小的可执行 vmalloc 区 */
	alloc_size = PAGE_ALIGN(len);

	/* 使用 __vmalloc_node_range 以便传入 caller 信息 */
	new_area = __vmalloc_node_range(alloc_size, 1, VMALLOC_START, VMALLOC_END,
					GFP_KERNEL, PAGE_KERNEL_EXEC,
					VM_FLUSH_RESET_PERMS,
					NUMA_NO_NODE, __builtin_return_address(0));
	if (!new_area) {
		pr_err("move_func: vmalloc exec failed for %lu bytes\n", alloc_size);
		return -ENOMEM;
	}

	/* 构造上下文 */
	orig_start_page = (unsigned long)func & PAGE_MASK;
	orig_npages = (PAGE_ALIGN((unsigned long)func + len) - orig_start_page) >> PAGE_SHIFT;

	dest_start_page = (unsigned long)new_area & PAGE_MASK;
	dest_npages = alloc_size >> PAGE_SHIFT;

	memset(&ctx, 0, sizeof(ctx));
	ctx.orig = func;
	ctx.dest = new_area;
	ctx.len = len;
	ctx.orig_page = orig_start_page;
	ctx.orig_npages = orig_npages;
	ctx.dest_page = dest_start_page;
	ctx.dest_npages = dest_npages;
	ctx.error = -EFAULT;

	ret = move_worker(&ctx);
	if (ret) {
		pr_err("move_func: move_worker failed: %d\n", ret);
		vfree(new_area);
		return ret;
	}
	if (ctx.error) {
		pr_err("move_func: worker reported error: %d\n", ctx.error);
		vfree(new_area);
		return ctx.error;
	}

	if (newp)
		*newp = new_area;

	return 0;
}
