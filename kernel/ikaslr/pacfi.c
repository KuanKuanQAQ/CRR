// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR 随机化辅助的 ARM PA 精准 CFI（论文第 5 章）。
 *
 * 核心观察（§5.1）：随机化区域内的控制流因目标地址不可预测而天然不可劫持，
 * 因此进入随机化区域的间接调用**不需要 PA 验证**；把造成上下文传播的高扇入
 * 函数移入随机化区域，即可从传播链中"剪除"它，两侧调用点得以各自建立唯一上下文。
 *
 * 本文件分步实现：
 *   S3.1 PA 原语（签名/认证 + 上下文计算）           <- 本步
 *   S3.2 高扇入函数动态识别（签名-认证-重签名 三阶段）
 *   S3.3 渐进式上下文精准化
 *   S3.4 跳板处分区验证（fixed_in 免验 / fixed_out 验证）
 *   S3.5 高扇入函数迁入随机化区域
 *
 * 用内核已安装的 APIA 密钥（CONFIG_ARM64_PTR_AUTH_KERNEL），配合我们自己的
 * 上下文（类型哈希 / 调用点哈希）。PAC 指令由 FEAT_PAuth 提供。
 */
#define pr_fmt(fmt) "ikaslr/pacfi: " fmt

#include <linux/ikaslr.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/init.h>
#include <linux/hash.h>

#include "internal.h"

/*
 * PA 原语。用 key A（指令密钥），modifier = 上下文。
 *   sign(p, ctx)  = PACIA p, ctx
 *   auth(p, ctx)  = AUTIA p, ctx   —— 成功则还原原指针，失败则得到损坏的指针
 * 验证是否成功：auth(sign(p,ctx), ctx) == p。用错误的 ctx 认证得到 != p。
 */
static __always_inline u64 ikaslr_pac_sign(u64 ptr, u64 ctx)
{
	asm volatile (".arch_extension pauth\n\tpacia %0, %1"
		      : "+r" (ptr) : "r" (ctx));
	return ptr;
}

static __always_inline u64 ikaslr_pac_auth(u64 ptr, u64 ctx)
{
	asm volatile (".arch_extension pauth\n\tautia %0, %1"
		      : "+r" (ptr) : "r" (ctx));
	return ptr;
}

/* 认证是否通过：还原值等于期望的原指针。*/
static __always_inline bool ikaslr_pac_ok(u64 signed_ptr, u64 ctx, u64 expected)
{
	return ikaslr_pac_auth(signed_ptr, ctx) == expected;
}

/* 上下文计算（§5.5.1）。类型上下文取函数原型哈希；调用点上下文取调用点地址哈希。*/
static u64 ikaslr_ctx_type(u32 type_hash)
{
	return hash_64((u64)type_hash | 0x1UL << 32, 64) | 1;	/* 非零 */
}

static u64 ikaslr_ctx_callsite(unsigned long cs_addr)
{
	return hash_64(cs_addr, 64) | 1;
}

/* 供 S3.2+ 使用的内部接口。*/
u64 ikaslr_pacfi_sign(u64 ptr, u64 ctx) { return ikaslr_pac_sign(ptr, ctx); }
u64 ikaslr_pacfi_auth(u64 ptr, u64 ctx) { return ikaslr_pac_auth(ptr, ctx); }
u64 ikaslr_pacfi_ctx_type(u32 h) { return ikaslr_ctx_type(h); }
u64 ikaslr_pacfi_ctx_cs(unsigned long a) { return ikaslr_ctx_callsite(a); }

#ifdef CONFIG_IKASLR_DEBUG
/* S3.1 探针：验证 PAC 在本环境（含 QEMU TCG）确实工作。*/
static int __init ikaslr_pacfi_probe(void)
{
	u64 p = (u64)&ikaslr_pacfi_probe;	/* 一个真实的内核代码地址 */
	u64 c1 = ikaslr_ctx_type(0xdeadbeef);
	u64 c2 = ikaslr_ctx_callsite((unsigned long)&ikaslr_pacfi_sign);
	u64 s1, s2;

	s1 = ikaslr_pac_sign(p, c1);
	s2 = ikaslr_pac_sign(p, c2);

	pr_info("probe: ptr=%llx sign(type)=%llx sign(cs)=%llx\n", p, s1, s2);

	if (s1 == p) {
		pr_err("probe: signing produced no PAC (FEAT_PAuth off?)\n");
		return 0;
	}
	if (!ikaslr_pac_ok(s1, c1, p)) {
		pr_err("probe: FAIL roundtrip auth(sign(p,c),c) != p\n");
		return 0;
	}
	if (ikaslr_pac_ok(s1, c2, p)) {
		pr_err("probe: FAIL wrong-context auth succeeded\n");
		return 0;
	}
	if (s1 == s2) {
		pr_err("probe: FAIL different contexts gave same signature\n");
		return 0;
	}
	pr_info("probe: PASS - PAC works; roundtrip ok, wrong-context rejected, "
		"contexts distinct\n");
	return 0;
}
late_initcall(ikaslr_pacfi_probe);
#endif
