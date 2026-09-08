// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 持续随机化执行机制——运行时核心（论文第 3 章）。
 *
 * 本文件 (S1.1) 建立子系统骨架与初始化期的"发现"逻辑：从两张平行指针表
 * 构建随机化函数描述符数组 ikaslr_funcs[]，供后续步骤使用。
 *
 * 后续步骤在其上补入：
 *   S1.2 跳板运行时与 target_addr 表
 *   S1.3 精确线程追踪（ikaslr_enter/leave 的真实实现）
 *   S1.4 无副本随机化（ikaslr_rerandomize 的真实实现）
 *   S1.5 非抢占上下文的推迟随机化
 *   S1.6 函数指针语义分离
 *   S1.7 共享 GOT
 *   S1.8 白名单
 *   S1.9 随机化线程 / 控制接口
 *
 * 本文件一般化自早期原型 kernel/rerand.c（发现逻辑）与
 * samples/kernel_trampoline_move（移动 + UDF 异常挂起 + 进出计数）。
 */
#define pr_fmt(fmt) "ikaslr: " fmt

#include <linux/ikaslr.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/printk.h>

static struct ikaslr_func *ikaslr_funcs;
static int ikaslr_nfuncs;

int ikaslr_nr_funcs(void)
{
	return ikaslr_nfuncs;
}

/* ---- S1.3 线程追踪：此处为桩，真实实现在后续步骤 ---- */
void ikaslr_enter(unsigned int idx) { }
void ikaslr_leave(unsigned int idx) { }

/* ---- S1.4/S1.5 重随机化：此处为桩 ---- */
int ikaslr_rerandomize(void)
{
	if (!ikaslr_nfuncs)
		return 0;
	pr_warn_once("rerandomize: not implemented yet (S1.4)\n");
	return 0;
}

/*
 * 从两张指针表构建函数描述符数组。
 * 两表项数必须相等（每个随机化函数各一项 tramp + 一项 body）；不等即说明
 * 某个注解缺了配对，如实报错而不臆断。函数体大小由相邻 body 指针之差得出，
 * 最后一项以随机化区域末尾定界。
 */
static int __init ikaslr_build_funcs(void)
{
	void **tp = __start_tramp_ptr_tbl;
	void **rp = __start_rand_ptr_tbl;
	int nt = __end_tramp_ptr_tbl - __start_tramp_ptr_tbl;
	int nr = __end_rand_ptr_tbl - __start_rand_ptr_tbl;
	int i;

	if (nt != nr) {
		pr_err("pointer tables mismatch: tramp=%d rand=%d (missing annotation pair)\n",
		       nt, nr);
		return -EINVAL;
	}
	if (!nt)
		return 0;

	ikaslr_funcs = kcalloc(nt, sizeof(*ikaslr_funcs), GFP_KERNEL);
	if (!ikaslr_funcs)
		return -ENOMEM;

	for (i = 0; i < nt; i++) {
		ikaslr_funcs[i].tramp = tp[i];
		ikaslr_funcs[i].body  = rp[i];
		if (i < nt - 1)
			ikaslr_funcs[i].size =
				(unsigned long)rp[i + 1] - (unsigned long)rp[i];
		else
			ikaslr_funcs[i].size =
				(unsigned long)__rand_text_end - (unsigned long)rp[i];
	}
	ikaslr_nfuncs = nt;

	if (IS_ENABLED(CONFIG_IKASLR_DEBUG)) {
		for (i = 0; i < nt; i++)
			pr_info("func[%d]: tramp=%px body=%px size=%zu\n",
				i, ikaslr_funcs[i].tramp, ikaslr_funcs[i].body,
				ikaslr_funcs[i].size);
	}
	return 0;
}

static int __init ikaslr_init(void)
{
	unsigned long rand_sz = __rand_text_end - __rand_text_start;
	unsigned long tramp_sz = __tramp_text_end - __tramp_text_start;
	int ret;

	pr_info("regions: .rand.text=[%px,%px) %lu B, .tramp.text=[%px,%px) %lu B\n",
		__rand_text_start, __rand_text_end, rand_sz,
		__tramp_text_start, __tramp_text_end, tramp_sz);

	if (!rand_sz) {
		pr_info("randomization region empty; nothing to do "
			"(no annotated functions / LLVM pass not wired)\n");
		return 0;
	}

	ret = ikaslr_build_funcs();
	if (ret) {
		pr_err("init failed: %d\n", ret);
		return ret;
	}
	pr_info("registered %d randomizable function(s)\n", ikaslr_nfuncs);
	return 0;
}
late_initcall(ikaslr_init);
