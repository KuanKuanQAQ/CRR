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
#include <asm/vmx.h>

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
static u64 ept_pointer;
static void *vmcs_region;
static void ikaslr_setup_vmcs(void);
static int ikaslr_vmlaunch_min(void);
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
					/* S2.1b-2b：VMLAUNCH 最小回路。*/
					if (!ikaslr_vmlaunch_min()) {
						u32 reason = ikaslr_vmread(VM_EXIT_REASON) & 0xffff;

						pr_info("S2.1b-2b OK: VMLAUNCH entered self-guest; "
							"caught VM exit reason=%u (18=VMCALL)\n",
							reason);
					} else {
						pr_err("S2.1b-2b: VMLAUNCH failed, "
						       "VM_INSTRUCTION_ERROR=%lu\n",
						       ikaslr_vmread(VM_INSTRUCTION_ERROR));
					}
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
static u64 *ept_pdpt;			/* 1 页，512 个 1GB 项 */
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
