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

    for (i = 0; i < j; i++) {
        pr_info("func_array[%d]: old_addr=%p, size=%zu\n",
                i, func_array[i].old_addr, func_array[i].size);
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

	return 0;
}
late_initcall(rerand_init);
