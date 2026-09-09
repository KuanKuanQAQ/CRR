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

#include "internal.h"
#include <asm/vmx.h>
#include <uapi/asm/vmx.h>

#ifndef MSR_IA32_VMX_BASIC
#define MSR_IA32_VMX_BASIC	0x00000480
#endif

/* 每 CPU 的 VMXON 区域（4KB，物理连续）。当前只用 boot CPU。*/
static void *vmxon_region;

/* S2.1b 基础设施（定义在下方，smoke 测试提前使用）。*/
static int ikaslr_ept_build(void);
static void ikaslr_ept_free(void);
static int ikaslr_vmcs_load(u32 rev);
static int ikaslr_vmwrite(unsigned long field, unsigned long val);
static unsigned long ikaslr_vmread(unsigned long field);
static int ikaslr_vmclear(u64 pa);
static int ikaslr_vmptrld(u64 pa);
static u64 ept_pointer;
static void *vmcs_region;
static void ikaslr_setup_vmcs(void);
static int ikaslr_vmlaunch_min(void);
static bool ikaslr_ept_has_xo(void);
static int ikaslr_ept_protect(unsigned long pa);
static void ikaslr_ept_protect_free(void);
static int ikaslr_vmlaunch_read_test(void *prot_va);
static void ikaslr_guest_workload(long iters);
static void ikaslr_guest_reader(long reads);
static volatile u8 *ikaslr_guest_read_target;
static int ikaslr_vmlaunch_loop(void (*guest_entry)(long), long arg);
static volatile u64 ikaslr_cpuid_exits, ikaslr_resume_count, ikaslr_ept_viol_count;
static volatile u32 ikaslr_unhandled_reason;
static volatile u64 ikaslr_reads_detected, ikaslr_rerand_requested;
static void ikaslr_ept_set_readable(bool readable);
static void ikaslr_set_mtf(bool on);
static u64 *ept_pdpt;
static unsigned long vmcs_fail_field;

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
 * S2.1b-2b + S2.1c：先跑最小 VMLAUNCH 回路，再把一页设为 execute-only 让 guest
 * 读它，用 exit reason 判定物理页级 XOM 是否生效。
 */
static void ikaslr_ept_xom_test(void)
{
	void *page;
	unsigned long pa;
	u32 reason;

	/* S2.1b-2b：最小回路（guest 只执行 vmcall）。*/
	if (ikaslr_vmlaunch_min()) {
		pr_err("S2.1b-2b: VMLAUNCH failed, VM_INSTRUCTION_ERROR=%lu\n",
		       ikaslr_vmread(VM_INSTRUCTION_ERROR));
		return;
	}
	reason = ikaslr_vmread(VM_EXIT_REASON) & 0xffff;
	pr_info("S2.1b-2b OK: VMLAUNCH entered self-guest; exit reason=%u (%s)\n",
		reason, reason == EXIT_REASON_VMCALL ? "VMCALL" : "?");

	/* S2.1c：把一页设为 execute-only，再让 guest 读它。*/
	if (!ikaslr_ept_has_xo()) {
		pr_warn("S2.1c: CPU lacks EPT execute-only support; skipping\n");
		return;
	}
	page = (void *)get_zeroed_page(GFP_KERNEL);
	if (!page)
		return;
	*(u8 *)page = 0xc3;		/* 放一条 ret，代表"代码页" */
	pa = virt_to_phys(page);

	if (ikaslr_ept_protect(pa)) {
		free_page((unsigned long)page);
		return;
	}

	/*
	 * 第一次 VMLAUNCH 之后 VMCS 处于 launched 状态，再次 VMLAUNCH 会得到
	 * VM_INSTRUCTION_ERROR=4（VMLAUNCH with non-clear VMCS）——实测踩到。
	 * VMCLEAR 把它写回并标为 clear（内容保留），VMPTRLD 重新设为当前 VMCS，
	 * 之后才能再次 VMLAUNCH。字段重填一遍以确保 guest/host state 完好。
	 */
	{
		u64 vpa = virt_to_phys(vmcs_region);

		if (ikaslr_vmclear(vpa) || ikaslr_vmptrld(vpa)) {
			pr_err("S2.1c: failed to re-arm VMCS\n");
			ikaslr_ept_protect_free();
			free_page((unsigned long)page);
			return;
		}
		ikaslr_setup_vmcs();
		if (vmcs_fail_field) {
			pr_err("S2.1c: VMCS re-setup failed at %lx\n", vmcs_fail_field);
			ikaslr_ept_protect_free();
			free_page((unsigned long)page);
			return;
		}
	}

	if (ikaslr_vmlaunch_read_test(page)) {
		pr_err("S2.1c: VMLAUNCH failed, VM_INSTRUCTION_ERROR=%lu\n",
		       ikaslr_vmread(VM_INSTRUCTION_ERROR));
	} else {
		reason = ikaslr_vmread(VM_EXIT_REASON) & 0xffff;
		if (reason == EXIT_REASON_EPT_VIOLATION) {
			pr_info("S2.1c OK: guest read of execute-only page trapped; "
				"EPT violation gpa=%lx qual=%lx\n",
				ikaslr_vmread(GUEST_PHYSICAL_ADDRESS),
				ikaslr_vmread(EXIT_QUALIFICATION));
			pr_info("S2.1c: physical-page XOM is effective\n");
		} else if (reason == EXIT_REASON_EPT_MISCONFIG) {
			pr_err("S2.1c: EPT misconfiguration (reason 49) - bad entry\n");
		} else {
			pr_err("S2.1c: read was NOT trapped (reason=%u); XOM ineffective\n",
			       reason);
		}
	}
	ikaslr_ept_protect_free();
	free_page((unsigned long)page);

	/* S2.1d：re-arm VMCS，跑受控 guest workload，验证 exit/VMRESUME 循环并测开销。*/
	{
		u64 vpa = virt_to_phys(vmcs_region);
		long iters = 100000;
		u64 c0, c1;
		int rc;

		if (ikaslr_vmclear(vpa) || ikaslr_vmptrld(vpa)) {
			pr_err("S2.1d: re-arm failed\n");
			return;
		}
		ikaslr_setup_vmcs();
		if (vmcs_fail_field) {
			pr_err("S2.1d: VMCS re-setup failed at %lx\n", vmcs_fail_field);
			return;
		}
		ikaslr_cpuid_exits = ikaslr_resume_count = 0;
		ikaslr_unhandled_reason = 0;

		c0 = rdtsc();
		rc = ikaslr_vmlaunch_loop(ikaslr_guest_workload, iters);
		c1 = rdtsc();

		if (rc == 0)
			pr_info("S2.1d OK: guest ran %ld CPUIDs across %llu VM exits, "
				"%llu cycles total (%llu cyc/exit); unhandled=%u\n",
				iters, ikaslr_cpuid_exits, c1 - c0,
				ikaslr_cpuid_exits ? (c1 - c0) / ikaslr_cpuid_exits : 0,
				ikaslr_unhandled_reason);
		else
			pr_err("S2.1d: loop failed rc=%d, VM_INSTRUCTION_ERROR=%lu, "
			       "unhandled_reason=%u\n", rc,
			       ikaslr_vmread(VM_INSTRUCTION_ERROR),
			       ikaslr_unhandled_reason);
	}

	/* S2.5：re-arm，保护一页，guest 反复读它，验证检测式 XOM 完整循环。*/
	{
		u64 vpa = virt_to_phys(vmcs_region);
		void *cpage = (void *)get_zeroed_page(GFP_KERNEL);
		long reads = 5;
		int rc;

		if (!cpage)
			return;
		*(u8 *)cpage = 0xc3;
		if (ikaslr_ept_has_xo() && !ikaslr_ept_protect(virt_to_phys(cpage)) &&
		    !ikaslr_vmclear(vpa) && !ikaslr_vmptrld(vpa)) {
			ikaslr_setup_vmcs();
			ikaslr_reads_detected = ikaslr_rerand_requested = 0;
			ikaslr_ept_viol_count = 0;
			ikaslr_unhandled_reason = 0;
			ikaslr_guest_read_target = cpage;

			rc = ikaslr_vmlaunch_loop(ikaslr_guest_reader, reads);

			if (rc == 0)
				pr_info("S2.5 OK: guest read protected page %ld times; "
					"reads detected=%llu, reprotect cycles=%llu, "
					"rerand-would-trigger=%llu; unhandled=%u\n",
					reads, ikaslr_reads_detected,
					ikaslr_ept_viol_count, ikaslr_rerand_requested,
					ikaslr_unhandled_reason);
			else
				pr_err("S2.5: loop rc=%d err=%lu unhandled=%u\n", rc,
				       ikaslr_vmread(VM_INSTRUCTION_ERROR),
				       ikaslr_unhandled_reason);
		}
		ikaslr_ept_protect_free();
		free_page((unsigned long)cpage);
	}
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

	pr_info("S2.1a OK: entered VMX root mode (rev=%u, VMXON %lld ns)\n",
		rev, ktime_to_ns(ktime_sub(ktime_get(), t0)));

	/* S2.1b：构造 EPT identity 页表，加载 VMCS，写入 EPTP 并读回校验。*/
	if (!ikaslr_ept_build() && !ikaslr_vmcs_load(rev)) {
		if (!ikaslr_vmwrite(EPT_POINTER, ept_pointer)) {
			u64 rb = ikaslr_vmread(EPT_POINTER);

			if (rb == ept_pointer) {
				pr_info("S2.1b-1 OK: VMCS loaded, EPTP verified (%llx)\n",
					rb);
				/* S2.1b-2a：填满所有 VMCS 字段并校验（不 VMLAUNCH）。*/
				ikaslr_setup_vmcs();
				if (vmcs_fail_field) {
					pr_err("S2.1b-2a: VMWRITE failed at field %lx\n",
					       vmcs_fail_field);
				} else {
					pr_info("S2.1b-2a OK: all VMCS fields valid\n");
					ikaslr_ept_xom_test();
				}
			} else {
				pr_err("S2.1b: EPTP readback mismatch %llx != %llx\n",
				       rb, ept_pointer);
			}
		} else {
			pr_err("S2.1b: VMWRITE(EPT_POINTER) failed\n");
		}
	}

	if (vmcs_region) {
		ikaslr_vmclear(virt_to_phys(vmcs_region));
		kfree(vmcs_region);
		vmcs_region = NULL;
	}
	ikaslr_ept_free();

	ikaslr_vmxoff();
	cr4_clear_bits(X86_CR4_VMXE);
	local_irq_restore(flags);

	pr_info("nested virtualization is available to the protected kernel\n");

	kfree(vmxon_region);
	vmxon_region = NULL;
	return 0;
}
late_initcall(ikaslr_ept_smoke);

/*
 * ===========================================================================
 * S2.1b：EPT identity 页表 + VMCS 基础设施
 * ===========================================================================
 *
 * EPT 页表：identity map（guest 物理 = host 物理），用 1GB 大页覆盖全部物理地址
 * 空间，只有要保护的 .rand.text 页在 S2.1c 里拆细并设为 execute-only。1GB 大页
 * 使 EPT 页表只需 PML4(1 项) + PDPT(512 项 1GB 页) 两级，几乎零开销。
 */

/* EPT 项的权限位（低 3 位）与内存类型（bits[5:3]）。*/
#define EPT_R		(1ull << 0)
#define EPT_W		(1ull << 1)
#define EPT_X		(1ull << 2)
#define EPT_RWX		(EPT_R | EPT_W | EPT_X)
#define EPT_MT_WB	(6ull << 3)	/* write-back */
#define EPT_PS		(1ull << 7)	/* 大页 */

/* identity map 覆盖的物理地址空间：512 个 1GB 页 = 512 GB。*/
#define EPT_NR_1GB	512

static u64 *ept_pml4;			/* 1 页 */
/* ept_pdpt 前置声明于文件上方 */
/* ept_pointer 前置声明于文件上方 */

static int ikaslr_ept_build(void)
{
	u64 pdpt_pa;
	int i;

	ept_pml4 = (u64 *)get_zeroed_page(GFP_KERNEL);
	ept_pdpt = (u64 *)get_zeroed_page(GFP_KERNEL);
	if (!ept_pml4 || !ept_pdpt)
		return -ENOMEM;

	/* PDPT：512 个 1GB identity 大页，全 RWX（保护在 S2.1c 施加）。*/
	for (i = 0; i < EPT_NR_1GB; i++)
		ept_pdpt[i] = ((u64)i << 30) | EPT_RWX | EPT_MT_WB | EPT_PS;

	pdpt_pa = virt_to_phys(ept_pdpt);
	ept_pml4[0] = pdpt_pa | EPT_RWX;

	/* EPTP：页表基址 | 4 级页遍历(值 3) | 内存类型 WB。*/
	ept_pointer = virt_to_phys(ept_pml4) |
		      (3ull << 3) |		/* page-walk length - 1 = 3 → 4 级 */
		      6ull;			/* EPT paging-structure MT = WB */

	pr_info("EPT identity map built: %d GB, EPTP=%llx\n",
		EPT_NR_1GB, ept_pointer);
	return 0;
}

static void ikaslr_ept_free(void)
{
	if (ept_pml4)
		free_page((unsigned long)ept_pml4);
	if (ept_pdpt)
		free_page((unsigned long)ept_pdpt);
	ept_pml4 = ept_pdpt = NULL;
}

/* ---- VMCS 基础设施（vmcs_region 前置声明于文件上方）---- */

static int ikaslr_vmclear(u64 pa)
{
	u8 err;

	asm volatile ("vmclear %[pa]; setna %[err]"
		      : [err] "=rm" (err) : [pa] "m" (pa) : "cc", "memory");
	return err ? -EIO : 0;
}

static int ikaslr_vmptrld(u64 pa)
{
	u8 err;

	asm volatile ("vmptrld %[pa]; setna %[err]"
		      : [err] "=rm" (err) : [pa] "m" (pa) : "cc", "memory");
	return err ? -EIO : 0;
}

static int ikaslr_vmwrite(unsigned long field, unsigned long val)
{
	u8 err;

	asm volatile ("vmwrite %[val], %[field]; setna %[err]"
		      : [err] "=rm" (err)
		      : [val] "rm" (val), [field] "r" (field) : "cc", "memory");
	return err ? -EIO : 0;
}

static unsigned long ikaslr_vmread(unsigned long field)
{
	unsigned long val;

	asm volatile ("vmread %[field], %[val]"
		      : [val] "=rm" (val) : [field] "r" (field) : "cc");
	return val;
}

/*
 * 分配并初始化 VMCS region，vmclear + vmptrld 使其成为当前 VMCS。
 * 调用时须已处于 VMX root 模式（VMXON 之后）。
 */
static int ikaslr_vmcs_load(u32 rev)
{
	u64 pa;

	vmcs_region = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!vmcs_region)
		return -ENOMEM;
	*(u32 *)vmcs_region = rev;
	pa = virt_to_phys(vmcs_region);

	if (ikaslr_vmclear(pa) || ikaslr_vmptrld(pa)) {
		kfree(vmcs_region);
		vmcs_region = NULL;
		return -EIO;
	}
	return 0;
}

/*
 * ===========================================================================
 * S2.1b-2a：填充 VMCS 的 host / guest / control 字段
 * ===========================================================================
 *
 * 本步只做 VMWRITE 并逐个校验成功，**不 VMLAUNCH**——因此不会三重故障，可安全
 * 验证所有字段编码与取值合法。VMLAUNCH 在 S2.1b-2b。
 *
 * guest state = 当前内核状态的延续（passthrough）：guest 就是这个内核自己，因此
 * guest 的 CR/段/描述符表/MSR 全部取当前值。host state 同样取当前值（VM exit 后
 * 回到内核）。controls 配成最小拦截：不拦中断/异常/CR/MSR，启用 EPT，只有 CPUID
 * 与 EPT violation 之类才 exit。
 */
#include <asm/desc.h>
#include <asm/segment.h>

/* 段 AR：VMX access-rights 字节，内核平坦段的标准值。*/
#define AR_CODE64	0xa09b	/* P,S,type=exec/read/acc,L=1,G=1 */
#define AR_DATA		0xc093	/* P,S,type=data/write/acc,D/B=1,G=1 */
#define AR_TSS64	0x008b	/* P,type=busy 64-bit TSS,S=0 */
#define AR_UNUSABLE	0x10000	/* bit16: unusable */

static u32 vmx_adjust_ctl(u32 want, u32 msr)
{
	u32 lo, hi;

	rdmsr(msr, lo, hi);	/* lo=allowed-0(必须为1), hi=allowed-1(可为1) */
	want |= lo;
	want &= hi;
	return want;
}

/* vmcs_fail_field 前置声明于文件上方 */

static int vmcs_w(unsigned long field, unsigned long val)
{
	if (ikaslr_vmwrite(field, val)) {
		if (!vmcs_fail_field)
			vmcs_fail_field = field ? field : 0xdead;
		return -EIO;
	}
	return 0;
}

static u16 read_cs(void) { u16 v; asm("mov %%cs,%0" : "=r"(v)); return v; }
static u16 read_ds(void) { u16 v; asm("mov %%ds,%0" : "=r"(v)); return v; }
static u16 read_es(void) { u16 v; asm("mov %%es,%0" : "=r"(v)); return v; }
static u16 read_ss(void) { u16 v; asm("mov %%ss,%0" : "=r"(v)); return v; }
static u16 read_fs(void) { u16 v; asm("mov %%fs,%0" : "=r"(v)); return v; }
static u16 read_gs(void) { u16 v; asm("mov %%gs,%0" : "=r"(v)); return v; }
static u16 read_tr(void) { u16 v; asm("str %0" : "=r"(v)); return v; }
static u16 read_ldtr(void) { u16 v; asm("sldt %0" : "=r"(v)); return v; }

static void ikaslr_setup_vmcs(void)
{
	struct desc_ptr gdt, idt;
	unsigned long cr0 = read_cr0(), cr3 = __read_cr3(), cr4 = __read_cr4();
	unsigned long tr_base;
	u16 tr = read_tr();

	native_store_gdt(&gdt);
	store_idt(&idt);

	/* TR base：从 GDT 中 TR 选择子对应的描述符取出。*/
	{
		struct ldttss_desc *d =
			(struct ldttss_desc *)(gdt.address + (tr & ~7));
		tr_base = ((unsigned long)d->base0) |
			  ((unsigned long)d->base1 << 16) |
			  ((unsigned long)d->base2 << 24) |
			  ((unsigned long)d->base3 << 32);
	}

	vmcs_fail_field = 0;

	/* ---- control 字段 ---- */
	vmcs_w(PIN_BASED_VM_EXEC_CONTROL,
	       vmx_adjust_ctl(0, MSR_IA32_VMX_TRUE_PINBASED_CTLS));
	vmcs_w(CPU_BASED_VM_EXEC_CONTROL,
	       vmx_adjust_ctl(CPU_BASED_ACTIVATE_SECONDARY_CONTROLS,
			      MSR_IA32_VMX_TRUE_PROCBASED_CTLS));
	vmcs_w(SECONDARY_VM_EXEC_CONTROL,
	       vmx_adjust_ctl(SECONDARY_EXEC_ENABLE_EPT,
			      MSR_IA32_VMX_PROCBASED_CTLS2));
	vmcs_w(VM_EXIT_CONTROLS,
	       vmx_adjust_ctl(VM_EXIT_HOST_ADDR_SPACE_SIZE |
			      VM_EXIT_LOAD_IA32_EFER | VM_EXIT_SAVE_IA32_EFER,
			      MSR_IA32_VMX_TRUE_EXIT_CTLS));
	vmcs_w(VM_ENTRY_CONTROLS,
	       vmx_adjust_ctl(VM_ENTRY_IA32E_MODE | VM_ENTRY_LOAD_IA32_EFER,
			      MSR_IA32_VMX_TRUE_ENTRY_CTLS));
	vmcs_w(EXCEPTION_BITMAP, 0);
	vmcs_w(EPT_POINTER, ept_pointer);
	vmcs_w(VMCS_LINK_POINTER, ~0ul);

	/* ---- host state ---- */
	vmcs_w(HOST_CR0, cr0);
	vmcs_w(HOST_CR3, cr3);
	vmcs_w(HOST_CR4, cr4);
	vmcs_w(HOST_CS_SELECTOR, read_cs() & ~7);
	vmcs_w(HOST_DS_SELECTOR, read_ds() & ~7);
	vmcs_w(HOST_ES_SELECTOR, read_es() & ~7);
	vmcs_w(HOST_SS_SELECTOR, read_ss() & ~7);
	vmcs_w(HOST_FS_SELECTOR, read_fs() & ~7);
	vmcs_w(HOST_GS_SELECTOR, read_gs() & ~7);
	vmcs_w(HOST_TR_SELECTOR, tr & ~7);
	vmcs_w(HOST_FS_BASE, __rdmsr(MSR_FS_BASE));
	vmcs_w(HOST_GS_BASE, __rdmsr(MSR_GS_BASE));
	vmcs_w(HOST_TR_BASE, tr_base);
	vmcs_w(HOST_GDTR_BASE, gdt.address);
	vmcs_w(HOST_IDTR_BASE, idt.address);
	vmcs_w(HOST_IA32_SYSENTER_CS, __rdmsr(MSR_IA32_SYSENTER_CS));
	vmcs_w(HOST_IA32_SYSENTER_ESP, __rdmsr(MSR_IA32_SYSENTER_ESP));
	vmcs_w(HOST_IA32_SYSENTER_EIP, __rdmsr(MSR_IA32_SYSENTER_EIP));
	vmcs_w(HOST_IA32_EFER, __rdmsr(MSR_EFER));

	/* ---- guest state（= 当前内核状态）---- */
	vmcs_w(GUEST_CR0, cr0);
	vmcs_w(GUEST_CR3, cr3);
	vmcs_w(GUEST_CR4, cr4);
	vmcs_w(GUEST_DR7, 0x400);
	vmcs_w(GUEST_RFLAGS, 0x2);	/* 保留位1，其余清（关中断） */
	vmcs_w(GUEST_IA32_EFER, __rdmsr(MSR_EFER));

#define SEG(pfx, selv, basev, arv)					\
	do {								\
		vmcs_w(GUEST_##pfx##_SELECTOR, (selv));			\
		vmcs_w(GUEST_##pfx##_BASE, (basev));			\
		vmcs_w(GUEST_##pfx##_LIMIT, 0xffffffff);		\
		vmcs_w(GUEST_##pfx##_AR_BYTES, (arv));			\
	} while (0)
	SEG(CS, read_cs(), 0, AR_CODE64);
	SEG(DS, read_ds(), 0, AR_DATA);
	SEG(ES, read_es(), 0, AR_DATA);
	SEG(SS, read_ss(), 0, AR_DATA);
	SEG(FS, read_fs(), __rdmsr(MSR_FS_BASE), AR_DATA);
	SEG(GS, read_gs(), __rdmsr(MSR_GS_BASE), AR_DATA);
	SEG(TR, tr, tr_base, AR_TSS64);
	SEG(LDTR, read_ldtr(), 0, AR_UNUSABLE);
#undef SEG
	vmcs_w(GUEST_TR_LIMIT, 0x67);
	vmcs_w(GUEST_LDTR_LIMIT, 0);

	vmcs_w(GUEST_GDTR_BASE, gdt.address);
	vmcs_w(GUEST_GDTR_LIMIT, gdt.size);
	vmcs_w(GUEST_IDTR_BASE, idt.address);
	vmcs_w(GUEST_IDTR_LIMIT, idt.size);
	vmcs_w(GUEST_SYSENTER_CS, __rdmsr(MSR_IA32_SYSENTER_CS));
	vmcs_w(GUEST_SYSENTER_ESP, __rdmsr(MSR_IA32_SYSENTER_ESP));
	vmcs_w(GUEST_SYSENTER_EIP, __rdmsr(MSR_IA32_SYSENTER_EIP));
	vmcs_w(GUEST_ACTIVITY_STATE, 0);
	vmcs_w(GUEST_INTERRUPTIBILITY_INFO, 0);
	vmcs_w(GUEST_PENDING_DBG_EXCEPTIONS, 0);
}

/*
 * ===========================================================================
 * S2.1b-2b：VMLAUNCH（最小验证）
 * ===========================================================================
 *
 * 把内核降为 self-guest，guest 立即执行一条 VMCALL 触发 VM exit，随后干净退出。
 * 这验证 VMCS state 正确、VMLAUNCH 可行、能进 guest 并捕获 exit——不追求让内核
 * 主线持续在 guest 里运行（那需要完整的 exit/VMRESUME 循环，是 S2.1d 的工作）。
 *
 * 经典最小回路：GUEST_RIP/RSP 指向 vmlaunch 之后，guest "继续"执行到 1: 处的
 * vmcall；HOST_RIP 指向 2:，VM exit 时 CPU 从那里以 host 状态继续。vmlaunch 失败
 * （未进 guest）则执行其下一条 setna，由 fail 标志报告，可读 VM_INSTRUCTION_ERROR
 * 诊断。
 */
static int ikaslr_vmlaunch_min(void)
{
	u8 fail = 0;

	asm volatile (
		"mov %%rsp, %%rax\n\t"
		"vmwrite %%rax, %[grsp]\n\t"	/* GUEST_RSP = 当前 rsp */
		"vmwrite %%rax, %[hrsp]\n\t"	/* HOST_RSP  = 同栈 */
		"lea 1f(%%rip), %%rax\n\t"
		"vmwrite %%rax, %[grip]\n\t"	/* GUEST_RIP = 1: */
		"lea 2f(%%rip), %%rax\n\t"
		"vmwrite %%rax, %[hrip]\n\t"	/* HOST_RIP  = 2: */
		"vmlaunch\n\t"
		"setna %[fail]\n\t"		/* 仅 vmlaunch 失败才执行到这 */
		"jmp 3f\n\t"
		"1:\n\t"			/* guest 从此继续（guest 模式） */
		"vmcall\n\t"			/* 无条件 VM exit */
		"2:\n\t"			/* VM exit 落此（host 模式，rsp 已恢复） */
		"3:\n\t"
		: [fail] "+r" (fail)
		: [grsp] "r" ((unsigned long)GUEST_RSP),
		  [hrsp] "r" ((unsigned long)HOST_RSP),
		  [grip] "r" ((unsigned long)GUEST_RIP),
		  [hrip] "r" ((unsigned long)HOST_RIP)
		: "rax", "cc", "memory");

	return fail ? -1 : 0;
}

/*
 * ===========================================================================
 * S2.1c：把代码页在 EPT 中设为 execute-only（物理页级 XOM）
 * ===========================================================================
 *
 * identity map 用的是 1GB 大页，要保护单个 4KB 页就必须逐级拆细：
 *   1GB 大页 -> PD（512 个 2MB 大页） -> PT（512 个 4KB 页）
 * 拆的时候必须把该区间的其余部分**照原样 identity 映射回去**，否则 guest 访问
 * 同一 1GB 内的其它内存就会失去映射。
 *
 * 目标页的 EPT 项设为 R=0, W=0, X=1：可执行、不可读。guest 读它即产生
 * EPT violation（exit reason 48），这正是第 4 章检测源一要捕获的信号。
 *
 * execute-only 需要 CPU 支持（IA32_VMX_EPT_VPID_CAP bit0）。不支持时 R=0,X=1
 * 是非法组合，会得到 EPT misconfiguration（reason 49）而不是 violation，
 * 因此先查能力位并如实报告。
 */
static u64 *ept_pd;		/* 覆盖被保护页所在的 1GB，512 个 2MB 项 */
static u64 *ept_pt;		/* 覆盖被保护页所在的 2MB，512 个 4KB 项 */
static unsigned long ept_prot_pa;	/* 被保护页的物理地址 */

static bool ikaslr_ept_has_xo(void)
{
	u64 cap;

	rdmsrl(MSR_IA32_VMX_EPT_VPID_CAP, cap);
	return !!(cap & VMX_EPT_EXECUTE_ONLY_BIT);
}

/* 把物理地址 pa 所在的 4KB 页设为 execute-only。*/
static int ikaslr_ept_protect(unsigned long pa)
{
	unsigned long g1 = pa >> 30;			/* 1GB 索引 */
	unsigned long base1g = g1 << 30;
	unsigned long i2m = (pa >> 21) & 511;		/* 该 1GB 内的 2MB 索引 */
	unsigned long base2m = pa & ~((1UL << 21) - 1);
	unsigned long i4k = (pa >> 12) & 511;
	int i;

	if (g1 >= EPT_NR_1GB)
		return -ERANGE;

	ept_pd = (u64 *)get_zeroed_page(GFP_KERNEL);
	ept_pt = (u64 *)get_zeroed_page(GFP_KERNEL);
	if (!ept_pd || !ept_pt)
		return -ENOMEM;

	/* 拆 1GB -> PD：该 1GB 内其余部分仍用 2MB 大页 identity 映射。*/
	for (i = 0; i < 512; i++)
		ept_pd[i] = (base1g + ((u64)i << 21)) | EPT_RWX | EPT_MT_WB | EPT_PS;

	/* 拆该 2MB -> PT：其余 4KB 页照常 identity 映射。*/
	for (i = 0; i < 512; i++)
		ept_pt[i] = (base2m + ((u64)i << 12)) | EPT_RWX | EPT_MT_WB;

	/* 目标页：execute-only（R=0, W=0, X=1）。*/
	ept_pt[i4k] = (pa & PAGE_MASK) | EPT_X | EPT_MT_WB;

	/* 挂回去：非叶项只有 RWX + 地址，没有内存类型/PS 位。*/
	ept_pd[i2m] = virt_to_phys(ept_pt) | EPT_RWX;
	ept_pdpt[g1] = virt_to_phys(ept_pd) | EPT_RWX;

	ept_prot_pa = pa;
	pr_info("EPT: page %lx now execute-only (R=0,W=0,X=1)\n", pa);
	return 0;
}

static void ikaslr_ept_protect_free(void)
{
	if (ept_pd)
		free_page((unsigned long)ept_pd);
	if (ept_pt)
		free_page((unsigned long)ept_pt);
	ept_pd = ept_pt = NULL;
}

/*
 * S2.1c 的 guest：先读被保护页，再 vmcall。
 * XOM 生效 -> 读触发 EPT violation（reason 48），永远到不了 vmcall；
 * XOM 失效 -> 读成功，落到 vmcall（reason 18）。exit reason 因此直接给出结论。
 */
static int ikaslr_vmlaunch_read_test(void *prot_va)
{
	u8 fail = 0;

	asm volatile (
		"mov %%rsp, %%rax\n\t"
		"vmwrite %%rax, %[grsp]\n\t"
		"vmwrite %%rax, %[hrsp]\n\t"
		"lea 1f(%%rip), %%rax\n\t"
		"vmwrite %%rax, %[grip]\n\t"
		"lea 2f(%%rip), %%rax\n\t"
		"vmwrite %%rax, %[hrip]\n\t"
		"vmlaunch\n\t"
		"setna %[fail]\n\t"
		"jmp 3f\n\t"
		"1:\n\t"			/* guest 开始执行 */
		"movzbl (%[prot]), %%eax\n\t"	/* 读被保护页 -> 期望 EPT violation */
		"vmcall\n\t"			/* 只有读成功才会到这 */
		"2:\n\t"			/* VM exit 落点 */
		"3:\n\t"
		: [fail] "+r" (fail)
		: [grsp] "r" ((unsigned long)GUEST_RSP),
		  [hrsp] "r" ((unsigned long)HOST_RSP),
		  [grip] "r" ((unsigned long)GUEST_RIP),
		  [hrip] "r" ((unsigned long)HOST_RIP),
		  [prot] "r" (prot_va)
		: "rax", "cc", "memory");

	return fail ? -1 : 0;
}

/*
 * ===========================================================================
 * S2.1d：exit / VMRESUME 循环 —— guest 跨多次 VM exit 持续运行
 * ===========================================================================
 *
 * 设计：HOST_RIP 指向 launch asm 块里的 exit 处理段，HOST_RSP 指向一个专用退出栈。
 * 每次 VM exit CPU 都回到该处（host 状态、退出栈）：把 guest GPR 压到退出栈上，
 * 调用 C 分发器处理，然后恢复 GPR 并 VMRESUME 回 guest。guest 有自己的栈，与退出
 * 栈分离，因此 exit 处理不扰动 guest 栈。
 *
 * 配置为最小拦截（pin-based/异常位图为 0），故中断与异常由 guest 内核自己的 IDT
 * 处理、不 exit。持续运行中会 exit 的只有无条件指令：CPUID、XSETBV，以及我们要的
 * EPT violation。分发器逐一处理并前进 GUEST_RIP，其余 reason 一律停机上报。
 *
 * 本步用一个受控 guest workload（若干次 CPUID + 读）把循环跑通并测开销；让内核
 * 主线永久运行在 guest 下是部署形态（S2.1e / 后续），风险更高，单列。
 */

/* guest 退出时保存的通用寄存器（单核、关中断，静态即可）。*/
struct ikaslr_gregs {
	u64 rax, rbx, rcx, rdx, rsi, rdi, rbp;
	u64 r8, r9, r10, r11, r12, r13, r14, r15;
};

/* 计数器前置声明于文件上方 */

static void ikaslr_advance_rip(void)
{
	unsigned long len = ikaslr_vmread(VM_EXIT_INSTRUCTION_LEN);
	unsigned long rip = ikaslr_vmread(GUEST_RIP);

	ikaslr_vmwrite(GUEST_RIP, rip + len);
}

/*
 * VM exit 分发器（由 launch asm 段调用，参数为退出栈上保存的 guest GPR）。
 * 返回 0 = VMRESUME 回 guest；1 = 停机退出循环。
 */
int ikaslr_vmexit_dispatch(struct ikaslr_gregs *r);
int ikaslr_vmexit_dispatch(struct ikaslr_gregs *r)
{
	u32 reason = ikaslr_vmread(VM_EXIT_REASON) & 0xffff;

	switch (reason) {
	case EXIT_REASON_CPUID: {
		u32 a = r->rax, c = r->rcx, eax, ebx, ecx, edx;

		asm volatile ("cpuid"
			      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
			      : "a"(a), "c"(c));
		r->rax = eax; r->rbx = ebx; r->rcx = ecx; r->rdx = edx;
		ikaslr_cpuid_exits++;
		ikaslr_advance_rip();
		ikaslr_resume_count++;
		return 0;
	}
	case EXIT_REASON_XSETBV: {
		u32 ecx = r->rcx, eax = r->rax, edx = r->rdx;

		asm volatile ("xsetbv" : : "a"(eax), "c"(ecx), "d"(edx));
		ikaslr_advance_rip();
		ikaslr_resume_count++;
		return 0;
	}
	case EXIT_REASON_EPT_VIOLATION:
		/*
		 * 捕获到对被保护代码页的读取（§4.4.3 检测源一）。
		 * 记录为"被探测"、请求一次随机化（经推迟路径，安全），然后临时开读 +
		 * 置 MTF，放行这一次读；MTF handler 再把页恢复为 execute-only。
		 */
		ikaslr_ept_viol_count++;
		ikaslr_reads_detected++;
		/*
		 * 注意：这里**不**内联调用随机化。整个 EPT 测试跑在 local_irq_save
		 * 窗口内，而随机化的 set_memory_* 会经 IPI 做跨核 TLB flush，在关中断
		 * 且持有 VMCS 的上下文里会破坏系统状态（实测导致随后 init 段错误）。
		 * 检测在此记录，触发随机化交由正常上下文（部署形态 S2.1e）。
		 */
		ikaslr_rerand_requested++;	/* 记录"本应触发一次" */
		ikaslr_ept_set_readable(true);
		ikaslr_set_mtf(true);
		ikaslr_resume_count++;
		return 0;			/* 重执行出错的读，随后 MTF 触发 */
	case EXIT_REASON_MONITOR_TRAP_FLAG:
		/* 那一次读已放行；关掉 MTF，把页恢复 execute-only。*/
		ikaslr_set_mtf(false);
		ikaslr_ept_set_readable(false);
		ikaslr_resume_count++;
		return 0;
	case EXIT_REASON_VMCALL:
		return 1;			/* guest 主动结束 */
	default:
		ikaslr_unhandled_reason = reason;
		return 1;
	}
}

/* guest 栈（受控 workload 使用；host 退出处理用本函数 C 栈，见 vmlaunch_loop）。*/
static u8 ikaslr_guest_stack[8192] __aligned(16);

/*
 * 受控 guest workload：做 iters 次 CPUID（每次都会 VM exit → 分发 → resume），
 * 借此把 exit/VMRESUME 循环跑满，然后 vmcall 结束。纯计算 + CPUID，不触发别的 exit。
 */
static noinline void ikaslr_guest_workload(long iters)
{
	long i;

	for (i = 0; i < iters; i++)
		asm volatile ("cpuid" : : "a"(0) : "rbx", "rcx", "rdx");
	asm volatile ("vmcall");
	/* 不返回 */
}

/* 供 S2.5 的 guest：读被保护页 arg 次（每次触发检测式 XOM 循环），再 vmcall。*/
static volatile u8 *ikaslr_guest_read_target;
static noinline void ikaslr_guest_reader(long reads)
{
	long i;
	u8 sink = 0;

	for (i = 0; i < reads; i++)
		sink += ikaslr_guest_read_target[0];
	asm volatile ("vmcall" : : "a"(sink));
	/* 不返回 */
}

/*
 * 启动 guest 并驱动 exit/VMRESUME 循环，直到分发器决定停机。
 * 返回 0 成功（正常停机），-1 = VMLAUNCH 失败，-2 = VMRESUME 失败。
 */
static int ikaslr_vmlaunch_loop(void (*guest_entry)(long), long arg)
{
	int rc = 0;

	/* RSP/RIP 字段先在 C 里用现成的 vmwrite 写好（字段须为寄存器操作数）。*/
	ikaslr_vmwrite(GUEST_RSP,
		       (unsigned long)&ikaslr_guest_stack[sizeof(ikaslr_guest_stack) - 16]);
	ikaslr_vmwrite(GUEST_RIP, (unsigned long)guest_entry);
	/*
	 * HOST_RSP 必须 = 本函数当前的 rsp（不能用独立退出栈）：VM exit 时 CPU 把
	 * rsp 设为 HOST_RSP，若指向别的栈，停机后返回本函数就带着错误的 rsp，
	 * 触发 stack-protector（实测踩到）。设为当前 rsp 后，每次 exit 落在本函数
	 * 栈帧之下，停机时把 15 个已压的 GPR 弹掉即回到正确 rsp。guest 有独立的
	 * guest_stack，故不与之冲突。
	 */

	asm volatile (
		/* HOST_RSP = 当前 rsp。*/
		"mov %[hrsp], %%rdx\n\t"
		"vmwrite %%rsp, %%rdx\n\t"
		/* HOST_RIP = 1f（exit 处理段）。*/
		"lea 1f(%%rip), %%rax\n\t"
		"mov %[hrip], %%rdx\n\t"
		"vmwrite %%rax, %%rdx\n\t"
		/* guest 从 GUEST_RIP 开始时 rdi = 此刻的 rdi，故把 arg 放进 rdi。*/
		"mov %[arg], %%rdi\n\t"
		"vmlaunch\n\t"
		"jmp 3f\n\t"			/* VMLAUNCH 失败 */

		"1:\n\t"			/* 每次 VM exit 回到这（host 状态，退出栈）*/
		"push %%r15\n\t push %%r14\n\t push %%r13\n\t push %%r12\n\t"
		"push %%r11\n\t push %%r10\n\t push %%r9\n\t push %%r8\n\t"
		"push %%rbp\n\t push %%rdi\n\t push %%rsi\n\t push %%rdx\n\t"
		"push %%rcx\n\t push %%rbx\n\t push %%rax\n\t"
		"mov %%rsp, %%rdi\n\t"		/* &gregs（rax 在最低地址）*/
		"call ikaslr_vmexit_dispatch\n\t"
		"test %%eax, %%eax\n\t"
		"jnz 2f\n\t"			/* 停机 */
		"pop %%rax\n\t pop %%rbx\n\t pop %%rcx\n\t pop %%rdx\n\t"
		"pop %%rsi\n\t pop %%rdi\n\t pop %%rbp\n\t pop %%r8\n\t"
		"pop %%r9\n\t pop %%r10\n\t pop %%r11\n\t pop %%r12\n\t"
		"pop %%r13\n\t pop %%r14\n\t pop %%r15\n\t"
		"vmresume\n\t"
		"movl $-2, %[rc]\n\t"		/* VMRESUME 失败 */
		"jmp 4f\n\t"

		"2:\n\t"			/* 正常停机：丢弃保存的 GPR，返回 0 */
		"add $120, %%rsp\n\t"		/* 15 * 8 = 120 */
		"jmp 4f\n\t"
		"3:\n\t"
		"movl $-1, %[rc]\n\t"
		"4:\n\t"
		: [rc] "+m" (rc)
		: [hrip] "i" ((unsigned long)HOST_RIP),
		  [hrsp] "i" ((unsigned long)HOST_RSP),
		  [arg] "m" (arg)
		: "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp",
		  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
		  "cc", "memory");
	return rc;
}

/*
 * ===========================================================================
 * S2.5：EPT violation → 检测 → 触发随机化（检测式 XOM 的完整循环）
 * ===========================================================================
 *
 * §4.4.3 检测源一的正确响应是"允许这一次读发生、但把它当信号"：
 *   1. 读被保护代码页 -> EPT violation；
 *   2. 记录被探测、请求随机化；临时把该页改为可读，并置 MTF（单步）；
 *   3. VMRESUME -> 出错的读指令重执行、成功；紧接着 MTF 触发新的 VM exit；
 *   4. MTF handler：清 MTF、把该页恢复为 execute-only。
 * 于是攻击者只读到一次（随即被随机化使其过期），而合法读（kprobes 等）也不会被
 * 阻断——正是"检测而非阻断"。
 *
 * 单核下 INVEPT 单上下文即可；多核要跨核 shootdown（见 04a-ept-single-core.md）。
 */

/* INVEPT 单上下文（type 1）。*/
static void ikaslr_invept(void)
{
	struct { u64 eptp, gpa; } desc = { ept_pointer, 0 };

	asm volatile ("invept %[desc], %[type]"
		      : : [desc] "m" (desc), [type] "r" (1ul) : "cc", "memory");
}

/* 把被保护页的 EPT 权限在 execute-only 与 RWX 之间切换，并刷新 EPT TLB。*/
static void ikaslr_ept_set_readable(bool readable)
{
	unsigned long i4k = (ept_prot_pa >> 12) & 511;

	if (readable)
		ept_pt[i4k] = (ept_prot_pa & PAGE_MASK) | EPT_RWX | EPT_MT_WB;
	else
		ept_pt[i4k] = (ept_prot_pa & PAGE_MASK) | EPT_X | EPT_MT_WB;
	ikaslr_invept();
}

/* MTF：在 primary proc-based controls 里置/清 monitor-trap-flag 位。*/
static void ikaslr_set_mtf(bool on)
{
	unsigned long v = ikaslr_vmread(CPU_BASED_VM_EXEC_CONTROL);

	if (on)
		v |= CPU_BASED_MONITOR_TRAP_FLAG;
	else
		v &= ~CPU_BASED_MONITOR_TRAP_FLAG;
	ikaslr_vmwrite(CPU_BASED_VM_EXEC_CONTROL, v);
}

/* ikaslr_reads_detected / ikaslr_rerand_requested 前置声明于文件上方 */
