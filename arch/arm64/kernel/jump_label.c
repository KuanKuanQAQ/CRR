// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Huawei Ltd.
 * Author: Jiang Liu <liuj97@gmail.com>
 *
 * Based on arch/arm/kernel/jump_label.c
 */
#include <linux/kernel.h>
#include <linux/jump_label.h>
#include <linux/ikaslr.h>
#include <asm/insn.h>
#include <asm/patching.h>

void arch_jump_label_transform(struct jump_entry *entry,
			       enum jump_label_type type)
{
	void *addr = (void *)jump_entry_code(entry);
	u32 insn;

	if (type == JUMP_LABEL_JMP) {
		insn = aarch64_insn_gen_branch_imm(jump_entry_code(entry),
						   jump_entry_target(entry),
						   AARCH64_INSN_BRANCH_NOLINK);
	} else {
		insn = aarch64_insn_gen_nop();
	}

	/*
	 * I-KASLR：随机化区域内的静态键，代码已不在链接期地址上。把同一条指令
	 * 打到映像母本与 READY 变体（母本是新变体的复制源，不打就会丢），再把
	 * 改写落到当前变体上。指令编码与位置无关：B 的偏移是"目标标号 − 本指令"，
	 * 两者同属一个函数、随函数整体搬移。
	 */
	/*
	 * I-KASLR：随机化区域内的静态键，代码已不在链接期地址上。母本、READY 与
	 * LIVE 三份都要打，理由与做法见 kernel/ikaslr/randomize.c 的
	 * ikaslr_patch_code()。指令编码与位置无关：B 的偏移是"目标标号 − 本指令"，
	 * 两者同属一个函数、随函数整体搬移。
	 */
	if (IS_ENABLED(CONFIG_IKASLR) &&
	    unlikely(ikaslr_image_to_live((unsigned long)addr))) {
		ikaslr_patch_code((unsigned long)addr, &insn, sizeof(insn));
		return;
	}

	aarch64_insn_patch_text_nosync(addr, insn);
}
