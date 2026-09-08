// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 评估用含漏洞接口（论文 §4.6.1 的"攻击者能力载体"）。
 *
 * ⚠⚠⚠ 这是一个**故意开的内核后门**，仅用于在受控 QEMU 环境中评估检测与随机化
 * 的响应。它把整个内核内存暴露给普通用户态。绝不能进入任何可部署构建。
 * 由独立的 CONFIG_IKASLR_EVAL_VULN 控制，默认 N，加载即污染内核。
 *
 * 为什么需要它（§4.6.1）：威胁模型假设攻击者已具备任意内核地址读与写。真实漏洞
 * 的可利用性受堆布局、并发时序等影响，成功率不稳定，会把这些噪声混进检测覆盖率
 * 与响应延迟的测量。用一个能力**恰好**为"任意读 + 任意写"的合成载体，使攻击者的
 * 起点在每次实验中完全一致，从而可以对探测策略、频率、目标做参数扫描。真实 CVE
 * 的完整攻击链验证另放在 §6.8。
 *
 * 满足的三条约束（§4.6.1）：
 *   1. 能力不多不少：只提供任意读与任意写，不泄露布局、不提供代码执行、
 *      不绕过任何本文机制；
 *   2. 位于非随机化区域：它代表攻击者已占据的立足点，不受本文机制保护；
 *   3. 不参与性能测量：涉及开销的实验在不编译本模块的配置下进行。
 *
 * 接口：/proc/ikaslr/attack （0600，仅 root；在评估用 initramfs 中即 root）
 *   写 "r <hex_addr> <len>"   —— 读 len 字节，随后可从同一 fd 读回
 *   写 "w <hex_addr> <hex_bytes>" —— 把 hex_bytes 写到该地址
 *   写 "probe <hex_addr> <len>"   —— 顺序读取一段代码，模拟 JIT-ROP 式扫描
 */
#define pr_fmt(fmt) "ikaslr/evalvuln: " fmt

#include <linux/ikaslr.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/init.h>

#include "internal.h"

#define EV_BUF_MAX	4096

/* 上一次读操作的结果，供从同一 fd 读回。per-open 存储。*/
struct ev_state {
	u8	buf[EV_BUF_MAX];
	size_t	len;
};

/*
 * 任意读：直接从给定内核虚拟地址拷贝。
 * 用 copy_from_kernel_nofault 以免非法地址导致 oops——攻击者的越界读可能命中
 * 未映射区，我们要如实返回失败而不是崩溃（也更贴近真实漏洞的行为）。
 */
static int ev_read(unsigned long addr, size_t len, u8 *out)
{
	if (len > EV_BUF_MAX)
		return -EINVAL;
	if (copy_from_kernel_nofault(out, (void *)addr, len))
		return -EFAULT;
	return 0;
}

static int ev_write(unsigned long addr, const u8 *in, size_t len)
{
	if (copy_to_kernel_nofault((void *)addr, in, len))
		return -EFAULT;
	return 0;
}

static int ev_hexbytes(const char *s, u8 *out, size_t max)
{
	size_t n = 0;

	while (*s && n < max) {
		unsigned int b;
		int consumed;

		if (sscanf(s, "%2x%n", &b, &consumed) != 1)
			break;
		out[n++] = b;
		s += consumed;
	}
	return n;
}

static ssize_t ev_write_proc(struct file *f, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct ev_state *st = f->private_data;
	char *kbuf, cmd[16];
	unsigned long addr;
	size_t len;
	int ret = count;

	if (count >= EV_BUF_MAX)
		return -EINVAL;
	kbuf = kzalloc(count + 1, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	if (copy_from_user(kbuf, ubuf, count)) {
		kfree(kbuf);
		return -EFAULT;
	}

	if (sscanf(kbuf, "%15s %lx %zu", cmd, &addr, &len) >= 2) {
		if (!strcmp(cmd, "r") || !strcmp(cmd, "probe")) {
			if (ev_read(addr, len, st->buf))
				ret = -EFAULT;
			else
				st->len = len;
		} else if (!strcmp(cmd, "w")) {
			char *hex = strchr(kbuf, ' ');

			hex = hex ? strchr(hex + 1, ' ') : NULL;
			if (hex) {
				u8 tmp[256];
				int n = ev_hexbytes(hex + 1, tmp, sizeof(tmp));

				if (n <= 0 || ev_write(addr, tmp, n))
					ret = -EFAULT;
			}
		}
	}
	kfree(kbuf);
	return ret;
}

static ssize_t ev_read_proc(struct file *f, char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct ev_state *st = f->private_data;

	return simple_read_from_buffer(ubuf, count, ppos, st->buf, st->len);
}

static int ev_open(struct inode *i, struct file *f)
{
	f->private_data = kzalloc(sizeof(struct ev_state), GFP_KERNEL);
	return f->private_data ? 0 : -ENOMEM;
}

static int ev_release(struct inode *i, struct file *f)
{
	kfree(f->private_data);
	return 0;
}

static const struct proc_ops ev_ops = {
	.proc_open	= ev_open,
	.proc_read	= ev_read_proc,
	.proc_write	= ev_write_proc,
	.proc_release	= ev_release,
};

int __init ikaslr_evalvuln_init(struct proc_dir_entry *dir)
{
	/* 故意的后门：污染内核，并在日志里大声说明。*/
	add_taint(TAINT_CRAP, LOCKDEP_STILL_OK);
	pr_warn("EVALUATION-ONLY arbitrary kernel R/W backdoor is ACTIVE.\n");
	pr_warn("This must never appear in a deployable build.\n");
	if (!proc_create_data("attack", 0600, dir, &ev_ops, NULL))
		return -ENOMEM;
	return 0;
}
