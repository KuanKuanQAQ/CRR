// SPDX-License-Identifier: GPL-2.0
/*
 * jump label x86 support
 *
 * Copyright (C) 2009 Jason Baron <jbaron@redhat.com>
 *
 */
#include <linux/jump_label.h>
#include <linux/ikaslr.h>
#include <linux/memory.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/jhash.h>
#include <linux/cpu.h>
#include <asm/kprobes.h>
#include <asm/alternative.h>
#include <asm/text-patching.h>
#include <asm/insn.h>

int arch_jump_entry_size(struct jump_entry *entry)
{
	struct insn insn = {};

	insn_decode_kernel(&insn, (void *)jump_entry_code(entry));
	BUG_ON(insn.length != 2 && insn.length != 5);

	return insn.length;
}

struct jump_label_patch {
	const void *code;
	int size;
};

static struct jump_label_patch
__jump_label_patch(struct jump_entry *entry, enum jump_label_type type)
{
	const void *expect, *code, *nop;
	const void *addr, *dest;
	int size;

	addr = (void *)jump_entry_code(entry);
	dest = (void *)jump_entry_target(entry);

	size = arch_jump_entry_size(entry);
	switch (size) {
	case JMP8_INSN_SIZE:
		code = text_gen_insn(JMP8_INSN_OPCODE, addr, dest);
		nop = x86_nops[size];
		break;

	case JMP32_INSN_SIZE:
		code = text_gen_insn(JMP32_INSN_OPCODE, addr, dest);
		nop = x86_nops[size];
		break;

	default: BUG();
	}

	if (type == JUMP_LABEL_JMP)
		expect = nop;
	else
		expect = code;

	if (memcmp(addr, expect, size)) {
		/*
		 * The location is not an op that we were expecting.
		 * Something went wrong. Crash the box, as something could be
		 * corrupting the kernel.
		 */
		pr_crit("jump_label: Fatal kernel bug, unexpected op at %pS [%p] (%5ph != %5ph)) size:%d type:%d\n",
				addr, addr, addr, expect, size, type);
		BUG();
	}

	if (type == JUMP_LABEL_NOP)
		code = nop;

	return (struct jump_label_patch){.code = code, .size = size};
}


/*
 * I-KASLR：随机化区域内的静态键，代码已不在链接期地址上。返回 true 表示这条
 * 已经由本函数处理完，调用方不要再走原来的改写。
 *
 * 三件事：
 *   1. 把同一段字节打到**映像母本**与所有 READY 变体上。母本是 ikaslr_prepare()
 *      复制的唯一源头；不打母本，下一轮随机化就会把这次开关悄悄退回旧状态，
 *      而且不会有任何报错。
 *   2. 把改写落到当前变体上——那一份正在执行，用 text_poke_bp 的原子序列。
 *   3. 整个过程与随机化切换互斥。否则"译出变体地址"与"真正 text_poke"之间
 *      若插进一轮随机化，补丁会打进已退役变体的陷阱页里。
 *
 * jlp.code 对每一份副本都相同：JMP 的偏移是"目标标号 − 本指令"，两者同属一个
 * 函数、随函数整体搬移，差值不变。正因为如此静态键与本方案相容；而静态调用点
 * （.static_call_sites）写的是指向区域外的 call rel32，偏移随函数位置而变，
 * 与"函数体内不得有指向区域外的 PC 相对引用"直接冲突——那类函数不进随机化范围
 * （见 scripts/ikaslr/gen_funcs.py 的硬约束）。
 */
static bool ikaslr_jump_label_patch(struct jump_entry *entry,
				   const struct jump_label_patch *jlp)
{
	unsigned long addr = jump_entry_code(entry);

	if (!IS_ENABLED(CONFIG_IKASLR) || likely(!ikaslr_image_to_live(addr)))
		return false;
	ikaslr_patch_code(addr, jlp->code, jlp->size);
	return true;
}

static __always_inline void
__jump_label_transform(struct jump_entry *entry,
		       enum jump_label_type type,
		       int init)
{
	const struct jump_label_patch jlp = __jump_label_patch(entry, type);

	/*
	 * As long as only a single processor is running and the code is still
	 * not marked as RO, text_poke_early() can be used; Checking that
	 * system_state is SYSTEM_BOOTING guarantees it. It will be set to
	 * SYSTEM_SCHEDULING before other cores are awaken and before the
	 * code is write-protected.
	 *
	 * At the time the change is being done, just ignore whether we
	 * are doing nop -> jump or jump -> nop transition, and assume
	 * always nop being the 'currently valid' instruction
	 */
	if (init || system_state == SYSTEM_BOOTING) {
		if (ikaslr_jump_label_patch(entry, &jlp))
			return;
		text_poke_early((void *)jump_entry_code(entry), jlp.code, jlp.size);
		return;
	}

	if (ikaslr_jump_label_patch(entry, &jlp))
		return;
	text_poke_bp((void *)jump_entry_code(entry), jlp.code, jlp.size, NULL);
}

static void __ref jump_label_transform(struct jump_entry *entry,
				       enum jump_label_type type,
				       int init)
{
	mutex_lock(&text_mutex);
	__jump_label_transform(entry, type, init);
	mutex_unlock(&text_mutex);
}

void arch_jump_label_transform(struct jump_entry *entry,
			       enum jump_label_type type)
{
	jump_label_transform(entry, type, 0);
}

bool arch_jump_label_transform_queue(struct jump_entry *entry,
				     enum jump_label_type type)
{
	struct jump_label_patch jlp;

	if (system_state == SYSTEM_BOOTING) {
		/*
		 * Fallback to the non-batching mode.
		 */
		arch_jump_label_transform(entry, type);
		return true;
	}

	mutex_lock(&text_mutex);
	jlp = __jump_label_patch(entry, type);
	/*
	 * 随机化区域内的条目**不进批处理队列**：排队与 text_poke_finish() 之间
	 * 隔着一段不可控的时间，期间可能发生若干轮随机化，队列里记的变体地址
	 * 早就失效了。改走逐条路径，译地址与改写之间由 ikaslr_patch_begin/end
	 * 互斥，没有可插入的窗口。这类条目数量很少，不批处理不影响整体开销。
	 */
	if (!ikaslr_jump_label_patch(entry, &jlp))
		text_poke_queue((void *)jump_entry_code(entry), jlp.code,
				jlp.size, NULL);
	mutex_unlock(&text_mutex);
	return true;
}

void arch_jump_label_transform_apply(void)
{
	mutex_lock(&text_mutex);
	text_poke_finish();
	mutex_unlock(&text_mutex);
}
