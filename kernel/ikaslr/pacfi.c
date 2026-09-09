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



/*
 * ===========================================================================
 * S3.2：高扇入函数动态识别（论文 §5.4.2）
 * ===========================================================================
 *
 * 目标：在运行时识别被多个调用点调用的函数（高扇入），无需全局静态分析。
 *
 * 与论文 §5.4.2 三阶段状态机的关系（一处必须说清的偏差）：
 *   §5.4.2 描述"认证成功后以调用点地址重新签名并写回；另一调用点认证失败后再以
 *   类型签名重试、成功即判高扇入"。**这一描述内部不自洽**：一旦 CS1 把指针重签为
 *   CS1 上下文并写回，CS2 既过不了自己的上下文、也过不了类型上下文（指针此刻是
 *   CS1 签的，不是类型签的），因此 CS2 无法凭认证区分"CS1 已签（高扇入）"与
 *   "被篡改"。
 *
 * 本实现改为一个**自洽且安全**的模型：
 *   - 存储中的指针始终以**类型上下文**签名（合法性的真值来源，等价于 kCFI 基线）；
 *   - 另用一个"归属"字段跟踪哪个调用点首先认领了它：
 *       首个调用点认证 -> 认领（owner = 该 CS）；
 *       同一 CS 再认证 -> 正常；
 *       另一个 CS 认证 -> **高扇入**（被两个调用点触及）。
 *   - 类型上下文认证失败 -> 指针被篡改 -> CFI violation。
 *   识别出的高扇入函数登记待迁入随机化区域（S3.5），此后不再参与 PA 验证。
 *
 * 精度（等价类 1）来自 S3.3：识别稳定后，把单一归属的函数重签为其 owner 的调用点
 * 上下文（此时安全从类型级升到调用点唯一级）；高扇入的则迁走。这样既自洽又达到
 * 论文的精度目标。
 */

#define IKASLR_CS_NONE		(-1)
#define IKASLR_MAX_FPTR		64

struct ikaslr_fptr {
	u64	slot;		/* 类型上下文签名后的指针 */
	u64	func;		/* 原始（未签名）函数地址 */
	u32	type_hash;
	int	owner_cs;	/* 首个认领的调用点 id；IKASLR_CS_NONE=未认领 */
	bool	high_fanin;
	bool	finalized;	/* S3.3：已重签为 owner 上下文 */
};

static atomic_long_t pacfi_violations;
static atomic_long_t pacfi_highfanin;

void ikaslr_pacfi_cfi_stats(unsigned long *viol, unsigned long *hfi)
{
	*viol = atomic_long_read(&pacfi_violations);
	*hfi = atomic_long_read(&pacfi_highfanin);
}

/* 产生一个受管理的函数指针：以类型上下文签名后存入 slot。*/
void ikaslr_pacfi_create(struct ikaslr_fptr *f, void *func, u32 type_hash)
{
	f->func = (u64)func;
	f->type_hash = type_hash;
	f->slot = ikaslr_pac_sign(f->func, ikaslr_ctx_type(type_hash));
	f->owner_cs = IKASLR_CS_NONE;
	f->high_fanin = false;
	f->finalized = false;
}

/*
 * 在调用点 cs_id 处验证并取用函数指针。返回可调用的函数地址；篡改则返回 0。
 * 副作用：认领 / 识别高扇入（见上方模型说明）。
 */
void *ikaslr_pacfi_verify(struct ikaslr_fptr *f, int cs_id)
{
	u64 authed;

	if (f->high_fanin)
		return (void *)f->func;	/* 已迁入随机化区域，免 PA */

	if (f->finalized) {
		/* S3.3：已重签为 owner 上下文，只有 owner 能通过。*/
		authed = ikaslr_pac_auth(f->slot, ikaslr_ctx_callsite(f->owner_cs));
		if (authed != f->func || cs_id != f->owner_cs) {
			/* 非 owner 触及一个已定稿的单归属函数 => 也是高扇入 */
			if (authed == f->func) {
				f->high_fanin = true;
				atomic_long_inc(&pacfi_highfanin);
				return (void *)f->func;
			}
			atomic_long_inc(&pacfi_violations);
			return NULL;
		}
		return (void *)f->func;
	}

	/* 未定稿：以类型上下文验证合法性。*/
	authed = ikaslr_pac_auth(f->slot, ikaslr_ctx_type(f->type_hash));
	if (authed != f->func) {
		atomic_long_inc(&pacfi_violations);	/* 篡改 */
		return NULL;
	}

	if (f->owner_cs == IKASLR_CS_NONE) {
		f->owner_cs = cs_id;			/* 首个认领 */
	} else if (f->owner_cs != cs_id) {
		f->high_fanin = true;			/* 第二个调用点 => 高扇入 */
		atomic_long_inc(&pacfi_highfanin);
	}
	return (void *)f->func;
}

/* S3.3：定稿——把单归属函数重签为 owner 的调用点上下文（安全升到唯一级）。*/
void ikaslr_pacfi_finalize(struct ikaslr_fptr *f)
{
	if (f->high_fanin || f->owner_cs == IKASLR_CS_NONE)
		return;
	f->slot = ikaslr_pac_sign(f->func, ikaslr_ctx_callsite(f->owner_cs));
	f->finalized = true;
}

#ifdef CONFIG_IKASLR_DEBUG
static void ikaslr_pacfi_dummy(void) { }

/* S3.2/S3.3：高扇入识别与定稿。CS 用小整数 id 代表不同调用点。*/
static void __init ikaslr_pacfi_ident_test(void)
{
	struct ikaslr_fptr f;
	unsigned long v0, h0, v1, h1;
	void *r;

	ikaslr_pacfi_cfi_stats(&v0, &h0);

	/* (1) 单调用点：CS=11 认领，多次调用不判高扇入。*/
	ikaslr_pacfi_create(&f, (void *)ikaslr_pacfi_dummy, 0x1234);
	r = ikaslr_pacfi_verify(&f, 11);
	if (r != (void *)ikaslr_pacfi_dummy || f.high_fanin) {
		pr_err("ident: FAIL first claim\n");
		return;
	}
	r = ikaslr_pacfi_verify(&f, 11);
	if (f.high_fanin) {
		pr_err("ident: FAIL same-cs marked high-fanin\n");
		return;
	}

	/* (2) 第二个调用点 CS=22 => 高扇入。*/
	r = ikaslr_pacfi_verify(&f, 22);
	if (!f.high_fanin) {
		pr_err("ident: FAIL second cs not detected as high-fanin\n");
		return;
	}

	/* (3) 篡改：破坏 slot 的签名 => CFI violation（返回 NULL）。*/
	{
		struct ikaslr_fptr g;

		ikaslr_pacfi_create(&g, (void *)ikaslr_pacfi_dummy, 0x1234);
		g.slot ^= 0x4UL << 56;		/* 破坏 PAC */
		r = ikaslr_pacfi_verify(&g, 11);
		if (r != NULL) {
			pr_err("ident: FAIL tampered pointer not rejected\n");
			return;
		}
	}

	/* (4) 定稿：单归属函数重签为 owner 上下文；非 owner 触及 => 高扇入。*/
	{
		struct ikaslr_fptr k;

		ikaslr_pacfi_create(&k, (void *)ikaslr_pacfi_dummy, 0x1234);
		ikaslr_pacfi_verify(&k, 33);	/* owner = 33 */
		ikaslr_pacfi_finalize(&k);
		if (!k.finalized) {
			pr_err("ident: FAIL finalize\n");
			return;
		}
		r = ikaslr_pacfi_verify(&k, 33);	/* owner ok */
		if (r != (void *)ikaslr_pacfi_dummy || k.high_fanin) {
			pr_err("ident: FAIL owner verify after finalize\n");
			return;
		}
		r = ikaslr_pacfi_verify(&k, 44);	/* 非 owner => 高扇入 */
		if (!k.high_fanin) {
			pr_err("ident: FAIL post-finalize non-owner not high-fanin\n");
			return;
		}
	}

	ikaslr_pacfi_cfi_stats(&v1, &h1);
	pr_info("ident: PASS - claim, high-fanin x2 (%lu->%lu), CFI violation (%lu->%lu), "
		"finalize+unique-context\n", h0, h1, v0, v1);
}

/* S3.1 探针：验证 PAC 在本环境（含 QEMU TCG）确实工作。*/
static void __init ikaslr_pacfi_ident_test(void);
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

	ikaslr_pacfi_ident_test();
	return 0;
}
late_initcall(ikaslr_pacfi_probe);
#endif
