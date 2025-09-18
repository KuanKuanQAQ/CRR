#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/delay.h>

#include <linux/rerand.h>

struct func_entry {
    void *old_addr;
    size_t size;
    void *new_addr; /* filled at runtime */
};
struct func_entry *func_array;

static void build_func_entry(void)
{
	int i = 0;
	int j = 0;
	int k = 0;


	pr_info("build_func_entry(): %s\n", "");

	void *tramp_ptr_tbl = __start_tramp_ptr_tbl;
	pr_info("tramp_ptr_tbl: %px\n", tramp_ptr_tbl);

	void *rand_ptr_tbl = __start_rand_ptr_tbl;
	pr_info("rand_ptr_tbl: %px\n", rand_ptr_tbl);

	for (void **p = tramp_ptr_tbl; p < __end_tramp_ptr_tbl; p++, i++) {
		pr_info("tramp_ptr_tbl[%d]: %px\n", i, *p);
	}
	
	for (void **p = rand_ptr_tbl; p < __end_rand_ptr_tbl; p++, j++) {
		pr_info("rand_ptr_tbl[%d]: %px\n", j, *p);
	}
	if (i != j) {
		pr_err("build_func_entry: i != j\n");
		return;
	}
	func_array = kcalloc(i, sizeof(*func_array), GFP_KERNEL);

	for (void **p = rand_ptr_tbl; k < i - 1; p++, k++) {
		func_array[k].old_addr = *p;
		func_array[k].size = (unsigned long)(*(p+1)) - (unsigned long)(*p);
	}
	func_array[k].old_addr = (void **)rand_ptr_tbl + k;
	func_array[k].size = (unsigned long)__end_rand_ptr_tbl - (unsigned long)rand_ptr_tbl;


}

static int do_rerand(void)
{
    struct func_entry *fstart = __start_rand_ptr_tbl;
    struct func_entry *fend   = __end_rand_ptr_tbl;
    size_t n = fend - fstart;
    void *copy_base;
    size_t total_size = 0;
    size_t i;


	pr_info("do_rerand(): %s\n", "TODO");

	void *tramp_ptr_tbl = __start_tramp_ptr_tbl;
	pr_info("tramp_ptr_tbl: %px\n", tramp_ptr_tbl);

	void *rand_ptr_tbl = __start_rand_ptr_tbl;
	pr_info("rand_ptr_tbl: %px\n", rand_ptr_tbl);

	for (void **p = tramp_ptr_tbl; p < __end_tramp_ptr_tbl; p++, i++) {
		pr_info("tramp_ptr_tbl[%d]: %px\n", i, *p);
	}
	
	i = 0;
	for (void **p = rand_ptr_tbl; p < __end_rand_ptr_tbl; p++, i++) {
		pr_info("rand_ptr_tbl[%d]: %px\n", i, *p);
	}


    if (n == 0) {
        pr_info("do_rerand: no functions in table\n");
        return 0;
    }

    /* 计算总大小并为新镜像分配页（对齐到 PAGE） */
    for (i = 0; i < n; i++)
        total_size += ALIGN(fstart[i].size, 16); /* alignment */

    copy_base = vmalloc_exec(total_size);
    if (!copy_base) {
        pr_err("do_rerand: vmalloc_exec failed\n");
        return -ENOMEM;
    }

    /* 随机打乱函数顺序（Fisher-Yates） */
    for (i = n - 1; i > 0; i--) {
        u32 r = prandom_u32() % (i + 1);
        if (r != i) {
            struct func_entry tmp = fstart[i];
            fstart[i] = fstart[r];
            fstart[r] = tmp;
        }
    }

    /* 将函数按打乱后的顺序拷贝到新缓冲区，并记录映射 */
    {
        char *dst = copy_base;
        for (i = 0; i < n; i++) {
            void *src = fstart[i].old_addr;
            size_t sz = fstart[i].size;
            memcpy(dst, src, sz);
            /* 记录 new addr */
            fstart[i].new_addr = dst;
            /* 如果函数体中含有地址常量/PC-relative 数据，需额外处理（复杂） */
            dst += ALIGN(sz, 16);
        }
    }

    /* 在写 trampoline 之前，确保 instruction cache 可见 */
    flush_icache_range((unsigned long)copy_base,
                       (unsigned long)copy_base + total_size);

    /* 接下来需要 patch trampoline -> 我单独实现为函数 */
    /* 当中会根据 trampoline 的编码方式找到需要写入的 immediate 并替换 */
    /* 比如如果 trampoline 模板是: movabs rax, imm64; jmp rax
     * 我们只需修改 imm64（8 字节）为 new target.
     */
    /* patch_trampolines(fstart, n, copy_base, total_size);  */
    /* 我把实现放到下面 */

    pr_info("do_rerand: copied %zu functions to %p\n", n, copy_base);
    return 0;
}

/*

/* 简单的 trampoline patch 实现示例：
 * - 假设 trampoline 里用 movabs rax, imm64 (opcode 48 b8 imm64)
 * - 我们搜索 .trampoline.text 区间中所有出现的特征并把 imm64 替换为 new addr
 * - 真实场景下你应更精确地识别每个 trampoline 的位置/格式
 */
static void patch_trampolines(struct func_entry *funcs, size_t n)
{
    unsigned long tstart = (unsigned long)__trampoline_text_start;
    unsigned long tend   = (unsigned long)__trampoline_text_end;
    unsigned long p;

    for (p = tstart; p + 10 < tend; p++) {
        /* 搜索 movabs rax, imm64 : 0x48 0xB8 */
        if (*(u8 *)p == 0x48 && *(u8 *)(p+1) == 0xB8) {
            /* 立即数在 p+2，64-bit */
            unsigned long *imm_ptr = (unsigned long *)(p + 2);
            unsigned long old_target = *imm_ptr;
            size_t j;
            /* 根据 old_target 在 funcs 中查找对应的 new_addr */
            for (j = 0; j < n; j++) {
                if ((unsigned long)funcs[j].old_addr == old_target) {
                    unsigned long new_t = (unsigned long)funcs[j].new_addr;
                    /* 使用 text_poke to safely overwrite kernel text */
#ifdef CONFIG_TEXT_POKE
                    /* text_poke will handle making text writable on the fly */
                    text_poke_64((void *)imm_ptr, new_t);
#else
                    /* fallback: change page rw, memcpy, then restore & flush icache */
                    unsigned long page = p & PAGE_MASK;
                    set_memory_rw(page, 1);
                    *imm_ptr = new_t;
                    set_memory_ro(page, 1);
                    flush_icache_range(page, page + PAGE_SIZE);
#endif
                    pr_info("patched trampoline at %p: %px -> %px\n",
                            (void *)p, (void *)old_target, (void *)new_t);
                    break;
                }
            }
        }
    }
}


static int __init rerand_init(void)
{
	void *start, *end;

    start = __tramp_text_start;
	end   = __tramp_text_end;
    if (start == end) {
        pr_info("rerand_init: .trampoline.text is empty, nothing to do\n");
        return 0;
    }
	
    pr_info("rerand_init: section .trampoline.text start=%px, end=%px, size=%ld bytes\n",
		start, end, (long)(end - start));


	start = __rand_text_start;
	end   = __rand_text_end;
	if (start == end) {
        pr_info("rerand_init: .rand.text is empty, nothing to do\n");
        return 0;
    }

    pr_info("rerand_init: section .rand.text start=%px, end=%px, size=%ld bytes\n",
		start, end, (long)(end - start));

	build_func_entry();
    do_rerand();

	return 0;
}
late_initcall(rerand_init);



/*


static struct timer_list randmod_timer;
// variable used to indicate period of doing rerandomizing
static unsigned long rand_period = 5000000UL;
static struct kobject *rand_kobj;
static const int time_out_iter = 3;

static void list_del_init_all(struct list_head *head)
{
	struct list_head *pos, *next;

	list_for_each_safe(pos, next, head) {
		list_del_init(pos);
	}
}

static ssize_t rand_period_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", rand_period);
}

static ssize_t rand_period_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	if (sscanf(buf, "%lu", &rand_period) != 1) {
		return -EINVAL;
	}
	return count;
}

static struct kobj_attribute rand_period_attr = __ATTR(rand_period, 0664, rand_period_show, rand_period_store);

static ssize_t rand_modules_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	size_t i;
	ssize_t len = 0;

	for (i = 0; i < module_count; ++i) {
		len += sprintf(buf + len, "%s\n", rand_modules[i]);
	}
	return len;
}

static ssize_t rand_modules_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	size_t i, j;
	char op;
	char module_name[MODULE_NAME_LEN];

	memset(module_name, 0, sizeof(module_name));

	if (sscanf(buf, "%c%s", &op, module_name) == 2) {
		// check if module(module_name) is loaded
		struct module *mod = find_module(module_name);

		if (!mod) {
			pr_err("Module %s is not loaded.\n", module_name);
			return -ENOENT;
		}

		if (op == '+') {
			for (i = 0; i < module_count; ++i) {
				if (strcmp(rand_modules[i], module_name) == 0) {
					pr_err("randmod:  %s already add", module_name);
					return -EEXIST;
				}
			}
			// add module
			if (module_count >= MAX_RANDMOD_MODULES) {
				pr_err("Module list is full.\n");
				return -ENOSPC;
			}
			// add module name to array
			strcpy(rand_modules[module_count], module_name);
			module_count++;
			pr_info("Module %s is added.\n", module_name);
			return count;
		} else if (op == '-') {
			// delete module
			for (i = 0; i < module_count; ++i) {
				// find module & replace it by last element
				if (strcmp(rand_modules[i], module_name) == 0) {
					for (j = 0; j < time_out_iter && i == randomize_module_index; j++) {
						msleep(rand_period / 3);
					}

					spin_lock(&randmod_thread_lock);
					if (randomize_module_index == i) {
						spin_unlock(&randmod_thread_lock);
						pr_err("randmod: wait for %s to complete rerandomization timeout.", module_name);
						return -EBUSY;
					}

					memmove(rand_modules + i, rand_modules + module_count - 1, sizeof(rand_modules[0]));
					// clear list of module by reinit it
					list_del_init_all(&randmod_thread_list[i]);
					list_splice(&randmod_thread_list[module_count - 1], &randmod_thread_list[i]);

					spin_unlock(&randmod_thread_lock);
					module_count--;
					pr_info("Module %s is removed.\n", module_name);
					return count;
				}
			}
			pr_err("Module %s is not found.\n", module_name);
			return -ENOENT;
		}
	}
	return -EINVAL;
}

static struct kobj_attribute rand_modules_attr = __ATTR(rand_modules, 0664, rand_modules_show, rand_modules_store);

void rerandomize(char *module_name)
{
	pr_debug("randmod: Do rerandomize for %s.\n", module_name);
}

void randmod_timer_callback(struct timer_list *timer)
{
	size_t i;

	pr_debug("randmod: Start rerandomize!\n");
	for (i = 0; i < module_count; ++i) {
		// do rerandomizing
		spin_lock(&randmod_thread_lock);
		randomize_module_index = i;
		spin_unlock(&randmod_thread_lock);

		rerandomize(rand_modules[i]);

		spin_lock(&randmod_thread_lock);
		randomize_module_index = -1;
		spin_unlock(&randmod_thread_lock);
	}
	// modify timer for next time
	mod_timer(&randmod_timer, jiffies + usecs_to_jiffies(rand_period));
}

static int __init randmod_init(void)
{
	int err;
	size_t i;

	module_count = 0;
	randomize_module_index = -1;

	spin_lock_init(&randmod_thread_lock);
	for (i = 0; i < MAX_RANDMOD_MODULES; i++) {
		INIT_LIST_HEAD(&randmod_thread_list[i]);
	}

	// create randmod dir in sysfs
	rand_kobj = kobject_create_and_add("randmod", kernel_kobj);
	if (!rand_kobj) {
		pr_err("randmod: Failed to create sysfs directory\n");
		return -ENOMEM;
	}

	// create /sys/module/randmod/rand_period
	err = sysfs_create_file(rand_kobj, &rand_period_attr.attr);
	if (err) {
		pr_err("randmod: Failed to create sysfs file\n");
		kobject_put(rand_kobj);
		return err;
	}

	// create /sys/module/randmod/rand_modules
	err = sysfs_create_file(rand_kobj, &rand_modules_attr.attr);
	if (err) {
		pr_err("randmod: Failed to create sysfs file\n");
		kobject_put(rand_kobj);
		return err;
	}

	timer_setup(&randmod_timer, randmod_timer_callback, 0);
	mod_timer(&randmod_timer, jiffies + usecs_to_jiffies(rand_period));

	pr_info("randmod: Initializing OK.\n");

	return 0;
}

static void __exit randmod_exit(void)
{
	size_t i;
	// delete /sys/module/randmod/rand_period & /sys/module/randmod/rand_modules
	sysfs_remove_file(rand_kobj, &rand_period_attr.attr);
	sysfs_remove_file(rand_kobj, &rand_modules_attr.attr);
	kobject_put(rand_kobj);

	del_timer(&randmod_timer);

	for (i = 0; i < time_out_iter && randomize_module_index != -1; i++) {
		msleep(rand_period);
	}

	spin_lock(&randmod_thread_lock);
	if (randomize_module_index != -1) {
		pr_err("randmod: wait for all modules to complete rerandomization timeout.");
	}

	for (i = 0; i < module_count; i++) {
		list_del_init_all(&randmod_thread_list[i]);
	}

	module_count = 0;
	spin_unlock(&randmod_thread_lock);

	randomize_module_index = -1;

	pr_info("randmod: Exiting OK.\n");
}

module_init(randmod_init);
module_exit(randmod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zihao Chang");
MODULE_DESCRIPTION("Randmod Module with Sysfs Interface");
MODULE_VERSION("1.0");

*/
