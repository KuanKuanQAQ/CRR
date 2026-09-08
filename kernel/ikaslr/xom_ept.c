// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR x86 EPT 只执行内存（论文 §4.5.1）——嵌套 hypervisor 路径。
 *
 * 部署模型（作者 2026-09-09 决策）：内核**自带**一层薄 hypervisor，把自身降为
 * guest，用 EPT 施加"内核自己关不掉的"页权限。QEMU+KVM 只提供裸机等价的
 * 嵌套虚拟化环境，不参与 XOM 逻辑，也没有 hypercall——真机上就是内核 late-launch
 * 成 hypervisor（类似 BitVisor/SecVisor）。
 *
 * ⚠ 这是一个学位论文级的子系统，分步实现，每步都能独立验证：
 *   S2.1a 进入/退出 VMX root 模式（VMXON/VMXOFF 往返）        <- 本文件当前范围
 *   S2.1b 构造 EPT 页表 + VMCS，VMLAUNCH 把内核降为 self-guest
 *   S2.1c EPT 把代码物理页设为不可读；读触发 EPT violation 被捕获
 *   S2.1d 内存池划分 + 分配器适配 + 加载期页表切换
 *   S2.5  EPT violation → 第 4 章控制流审计 → 触发随机化
 *
 * 本文件先做 S2.1a：确认 L1 内核确实能进入 VMX root 模式，即嵌套虚拟化对受保护
 * 内核可用。这是后续一切的地基；若这一步在目标环境里失败，整条 x86 路径无从谈起。
 */
#define pr_fmt(fmt) "ikaslr/ept: " fmt

#include <linux/ikaslr.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/ktime.h>

#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/cpufeature.h>
#include <asm/tlbflush.h>
#include <asm/special_insns.h>
#include <asm/io.h>

#ifndef MSR_IA32_VMX_BASIC
#define MSR_IA32_VMX_BASIC	0x00000480
#endif

/* 每 CPU 的 VMXON 区域（4KB，物理连续）。当前只用 boot CPU。*/
static void *vmxon_region;

/* 确保 VMX 在 IA32_FEAT_CTL 中已启用（未锁定则由我们锁定并启用）。*/
static int ikaslr_vmx_enable_feat_ctl(void)
{
	u64 feat;

	rdmsrl(MSR_IA32_FEAT_CTL, feat);
	if (feat & FEAT_CTL_LOCKED) {
		if (!(feat & FEAT_CTL_VMX_ENABLED_OUTSIDE_SMX)) {
			pr_err("VMX disabled by locked IA32_FEAT_CTL (BIOS/L0)\n");
			return -ENODEV;
		}
		return 0;
	}
	feat |= FEAT_CTL_LOCKED | FEAT_CTL_VMX_ENABLED_OUTSIDE_SMX;
	wrmsrl(MSR_IA32_FEAT_CTL, feat);
	return 0;
}

/* VMXON：返回 0 成功。失败时 CPU 会设 CF/ZF。*/
static int ikaslr_vmxon(u64 pa)
{
	u8 err;

	asm volatile ("vmxon %[pa]; setna %[err]"
		      : [err] "=rm" (err)
		      : [pa] "m" (pa)
		      : "cc", "memory");
	return err ? -EIO : 0;
}

static void ikaslr_vmxoff(void)
{
	asm volatile ("vmxoff" ::: "cc", "memory");
}

/*
 * S2.1a：VMXON/VMXOFF 往返。在 boot CPU 上、关中断的窗口内完成。
 */
static int __init ikaslr_ept_smoke(void)
{
	u64 vmx_basic, pa;
	unsigned long flags;
	u32 rev;
	ktime_t t0;
	int ret;

	if (!boot_cpu_has(X86_FEATURE_VMX)) {
		pr_info("CPU has no VMX; EPT XOM path unavailable\n");
		return 0;
	}
	ret = ikaslr_vmx_enable_feat_ctl();
	if (ret)
		return 0;	/* 环境不支持：不阻止内核启动 */

	vmxon_region = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!vmxon_region)
		return -ENOMEM;

	/* VMXON 区域头 4 字节须写 VMCS revision id（IA32_VMX_BASIC[30:0]）。*/
	rdmsrl(MSR_IA32_VMX_BASIC, vmx_basic);
	rev = (u32)vmx_basic & 0x7fffffff;
	*(u32 *)vmxon_region = rev;
	pa = virt_to_phys(vmxon_region);

	local_irq_save(flags);

	cr4_set_bits(X86_CR4_VMXE);
	t0 = ktime_get();
	ret = ikaslr_vmxon(pa);
	if (ret) {
		cr4_clear_bits(X86_CR4_VMXE);
		local_irq_restore(flags);
		pr_err("VMXON failed (nested virt not enabled in L0?)\n");
		kfree(vmxon_region);
		vmxon_region = NULL;
		return 0;
	}

	/* 已进入 VMX root 模式。S2.1b 将在此构造 VMCS/EPT 并 VMLAUNCH。*/
	ikaslr_vmxoff();
	cr4_clear_bits(X86_CR4_VMXE);
	local_irq_restore(flags);

	pr_info("S2.1a OK: entered and left VMX root mode (rev=%u, VMXON %lld ns)\n",
		rev, ktime_to_ns(ktime_sub(ktime_get(), t0)));
	pr_info("nested virtualization is available to the protected kernel\n");

	kfree(vmxon_region);
	vmxon_region = NULL;
	return 0;
}
late_initcall(ikaslr_ept_smoke);
