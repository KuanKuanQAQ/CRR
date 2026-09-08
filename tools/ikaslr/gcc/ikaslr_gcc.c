// SPDX-License-Identifier: GPL-2.0
/*
 * I-KASLR GCC 插件（论文 §3.5.1 的 GCC 版本，与 tools/ikaslr/llvm 功能对等）。
 *
 * ⚠ 本文件尚未在本机编译验证：构建它需要 gcc-<ver>-plugin-dev，而开发机上未安装
 *   （gcc -print-file-name=plugin 目录下没有 include/gcc-plugin.h）。见同目录
 *   Makefile 与 Documentation/crr/实现/07-compiler.md。
 *
 * 与 LLVM 版一致，对每个被选中的函数 F：
 *   1. 把 F 的汇编名改为 F_body，并置于 .rand.text.F；
 *   2. 以原名 F 发出 fixed_in 跳板（顶层内联汇编），置于 .tramp.text.F；
 *   3. 发出 target 槽与跳板表项；
 *   4. 把**同一编译单元内**的调用点改指向跳板。
 *
 * 为什么跳板用汇编而不在 GIMPLE 里构造函数：
 *   在 GCC 里新建一个函数并接入 cgraph 需要操作 GIMPLE 与调用图，代价高且难以
 *   验证；而跳板形式固定、极短（论文 §3.8 也强调"跳板函数体极短且形式单一"），
 *   逐架构写一段汇编反而更贴切，也便于逐条核对它没有引入多余的 gadget。
 *
 * 第 4 步的必要性：跨编译单元的调用按符号名解析，改名后自然落到跳板上；但同一
 * 单元内的调用绑定的是 DECL，若不改写就会直接调到 F_body 而绕过跳板。
 * （LLVM 版踩过同一个坑，见 07-compiler.md。）
 */
#include "gcc-plugin.h"
#include "plugin-version.h"
#include "tree.h"
#include "tree-pass.h"
#include "context.h"
#include "function.h"
#include "basic-block.h"
#include "gimple.h"
#include "gimple-iterator.h"
#include "cgraph.h"
#include "stringpool.h"
#include "attribs.h"
#include "output.h"
#include "varasm.h"
#include "gimple-ssa.h"
#include "tree-ssa-operands.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int plugin_is_GPL_compatible;

/* ---- 随机化函数名单 ---- */
#define MAX_FUNCS 4096
static char *ikaslr_names[MAX_FUNCS];
static int ikaslr_nnames;

static int ikaslr_selected(const char *name)
{
	int i;

	for (i = 0; i < ikaslr_nnames; i++)
		if (!strcmp(ikaslr_names[i], name))
			return 1;
	return 0;
}

static void ikaslr_load_list(const char *path)
{
	char line[256];
	FILE *f = fopen(path, "r");

	if (!f) {
		fprintf(stderr, "ikaslr: cannot open function list '%s'\n", path);
		return;
	}
	while (fgets(line, sizeof(line), f) && ikaslr_nnames < MAX_FUNCS) {
		char *p = line, *e;

		while (*p == ' ' || *p == '\t')
			p++;
		e = p + strlen(p);
		while (e > p && (e[-1] == '\n' || e[-1] == '\r' ||
				 e[-1] == ' ' || e[-1] == '\t'))
			*--e = 0;
		if (!*p || *p == '#')
			continue;
		ikaslr_names[ikaslr_nnames++] = xstrdup(p);
	}
	fclose(f);
}

/*
 * ---- 跳板的架构相关汇编 ----
 *
 * x86-64 调用约定下，调用 ikaslr_enter 会破坏保存参数的 caller-saved 寄存器，
 * 因此进入前把参数寄存器压栈、返回后恢复；同理在调用 ikaslr_leave 前后保护
 * 返回值（rax:rdx）。6 次压栈保持 16 字节栈对齐。
 */
static const char *ikaslr_tramp_asm_x86 =
"\t.pushsection .tramp.text.%s,\"ax\",@progbits\n"
"\t.align 16\n"
"\t.globl %s\n"
"\t.type %s,@function\n"
"%s:\n"
"\tendbr64\n"
"\tpushq %%rdi\n\tpushq %%rsi\n\tpushq %%rdx\n"
"\tpushq %%rcx\n\tpushq %%r8\n\tpushq %%r9\n"
"\tcall ikaslr_enter\n"
"\tpopq %%r9\n\tpopq %%r8\n\tpopq %%rcx\n"
"\tpopq %%rdx\n\tpopq %%rsi\n\tpopq %%rdi\n"
"\tcall *__ikaslr_target_%s(%%rip)\n"
"\tpushq %%rax\n\tpushq %%rdx\n"
"\tcall ikaslr_leave\n"
"\tpopq %%rdx\n\tpopq %%rax\n"
"\tret\n"
"\t.size %s,.-%s\n"
"\t.popsection\n";

/*
 * AArch64：参数在 x0-x7，返回值在 x0/x1。调用前后保护它们与 lr。
 * UDF/PAC 相关处理由第 5 章的 PA 插桩另行叠加。
 */
static const char *ikaslr_tramp_asm_arm64 =
"\t.pushsection .tramp.text.%s,\"ax\",@progbits\n"
"\t.align 4\n"
"\t.globl %s\n"
"\t.type %s,%%function\n"
"%s:\n"
"\tstp x29, x30, [sp, #-96]!\n"
"\tmov x29, sp\n"
"\tstp x0, x1, [sp, #16]\n\tstp x2, x3, [sp, #32]\n"
"\tstp x4, x5, [sp, #48]\n\tstp x6, x7, [sp, #64]\n"
"\tbl ikaslr_enter\n"
"\tldp x0, x1, [sp, #16]\n\tldp x2, x3, [sp, #32]\n"
"\tldp x4, x5, [sp, #48]\n\tldp x6, x7, [sp, #64]\n"
"\tadrp x16, __ikaslr_target_%s\n"
"\tldr x16, [x16, #:lo12:__ikaslr_target_%s]\n"
"\tblr x16\n"
"\tstp x0, x1, [sp, #80]\n"
"\tbl ikaslr_leave\n"
"\tldp x0, x1, [sp, #80]\n"
"\tldp x29, x30, [sp], #96\n"
"\tret\n"
"\t.size %s,.-%s\n"
"\t.popsection\n";

/* target 槽与跳板表项（表里存指针，理由同 LLVM 版：避免表项被过度对齐）。*/
static const char *ikaslr_data_asm =
"\t.pushsection .data..ikaslr_target,\"aw\",@progbits\n"
"\t.align 8\n"
"__ikaslr_target_%s:\n\t.quad %s_body\n"
"\t.popsection\n"
"\t.pushsection .rodata\n"
".Likaslr_name_%s:\n\t.asciz \"%s\"\n"
"\t.popsection\n"
"\t.pushsection .data\n"
"\t.align 8\n"
"__ikaslr_ent_%s:\n"
"\t.quad %s\n"              /* tramp  */
"\t.quad %s_body\n"         /* body   */
"\t.quad __ikaslr_target_%s\n"
"\t.quad .Likaslr_name_%s\n"
"\t.quad 0\n"               /* size：内核初始化时填 */
"\t.popsection\n"
"\t.pushsection .data..ikaslr_tramp_tbl,\"aw\",@progbits\n"
"\t.align 8\n"
"\t.quad __ikaslr_ent_%s\n"
"\t.popsection\n";

static void ikaslr_emit_asm(const char *name)
{
	char buf[4096];
	const char *tmpl;

#if defined(__x86_64__)
	tmpl = ikaslr_tramp_asm_x86;
	snprintf(buf, sizeof(buf), tmpl, name, name, name, name, name, name, name);
#elif defined(__aarch64__)
	tmpl = ikaslr_tramp_asm_arm64;
	snprintf(buf, sizeof(buf), tmpl, name, name, name, name,
		 name, name, name, name);
#else
	fprintf(stderr, "ikaslr: unsupported target architecture\n");
	return;
#endif
	symtab->finalize_toplevel_asm(build_string(strlen(buf), buf));

	snprintf(buf, sizeof(buf), ikaslr_data_asm,
		 name, name,			/* target 槽 */
		 name, name,			/* 名字串 */
		 name, name, name, name, name,	/* 表项 */
		 name);				/* 表内指针 */
	symtab->finalize_toplevel_asm(build_string(strlen(buf), buf));
}

/* ---- 改造 pass ---- */

static void ikaslr_transform_decl(tree decl, const char *name)
{
	char body[256];

	/* 禁止内联：内联出来的副本会绕过跳板与计数（LLVM 版实测踩过）。*/
	DECL_UNINLINABLE(decl) = 1;
	DECL_ATTRIBUTES(decl) = tree_cons(get_identifier("noinline"), NULL_TREE,
					  DECL_ATTRIBUTES(decl));

	/* 函数体落入随机化区域，并改名为 <fn>_body。*/
	{
		char sec[256];

		snprintf(sec, sizeof(sec), ".rand.text.%s", name);
		set_decl_section_name(decl, sec);
	}
	snprintf(body, sizeof(body), "%s_body", name);
	symtab->change_decl_assembler_name(decl, get_identifier(body));

	ikaslr_emit_asm(name);
}

/*
 * 把本编译单元内指向被随机化函数的调用改指向跳板。
 *
 * 跨单元调用按符号名解析，改名后自然落到跳板上；同单元调用绑定的是 DECL，
 * 不改写就会直接调到 F_body 而绕过跳板。
 */
static tree ikaslr_tramp_decl(tree orig, const char *name)
{
	tree d = build_decl(UNKNOWN_LOCATION, FUNCTION_DECL,
			    get_identifier(name), TREE_TYPE(orig));

	DECL_EXTERNAL(d) = 1;
	TREE_PUBLIC(d) = 1;
	TREE_USED(d) = 1;
	DECL_ARTIFICIAL(d) = 1;
	return d;
}

static unsigned int ikaslr_exec(void)
{
	basic_block bb;
	gimple_stmt_iterator gsi;

	FOR_EACH_BB_FN(bb, cfun) {
		for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
			gimple *stmt = gsi_stmt(gsi);
			tree fndecl;
			const char *cname;

			if (!is_gimple_call(stmt))
				continue;
			fndecl = gimple_call_fndecl(stmt);
			if (!fndecl)
				continue;
			cname = IDENTIFIER_POINTER(DECL_NAME(fndecl));
			if (!ikaslr_selected(cname))
				continue;
			gimple_call_set_fndecl(as_a<gcall *>(stmt),
					       ikaslr_tramp_decl(fndecl, cname));
			update_stmt(stmt);
		}
	}
	return 0;
}

static const pass_data ikaslr_pass_data = {
	GIMPLE_PASS, "ikaslr", OPTGROUP_NONE, TV_NONE,
	PROP_gimple_any, 0, 0, 0, 0,
};

namespace {
struct ikaslr_pass : gimple_opt_pass {
	ikaslr_pass(gcc::context *ctx) : gimple_opt_pass(ikaslr_pass_data, ctx) {}
	unsigned int execute(function *) override { return ikaslr_exec(); }
};
}

/* 在所有 IPA pass 之前改造被选中的函数本身。*/
static void ikaslr_start_unit(void *gcc_data, void *user_data)
{
	struct cgraph_node *node;

	FOR_EACH_FUNCTION(node) {
		tree decl = node->decl;
		const char *name;

		if (!decl || !DECL_NAME(decl))
			continue;
		name = IDENTIFIER_POINTER(DECL_NAME(decl));
		if (!ikaslr_selected(name) || !node->has_gimple_body_p())
			continue;
		ikaslr_transform_decl(decl, name);
	}
}

int plugin_init(struct plugin_name_args *info,
		struct plugin_gcc_version *version)
{
	struct register_pass_info pass_info;
	const char *list = getenv("IKASLR_FUNCS");
	int i;

	if (!plugin_default_version_check(version, &gcc_version)) {
		fprintf(stderr, "ikaslr: GCC version mismatch\n");
		return 1;
	}

	for (i = 0; i < info->argc; i++)
		if (!strcmp(info->argv[i].key, "funcs"))
			list = info->argv[i].value;
	if (!list) {
		fprintf(stderr, "ikaslr: no function list "
				"(-fplugin-arg-ikaslr_gcc-funcs=FILE or $IKASLR_FUNCS)\n");
		return 0;	/* 无名单时什么也不做 */
	}
	ikaslr_load_list(list);
	if (!ikaslr_nnames)
		return 0;

	register_callback(info->base_name, PLUGIN_ALL_IPA_PASSES_START,
			  ikaslr_start_unit, NULL);

	pass_info.pass = new ikaslr_pass(g);
	pass_info.reference_pass_name = "ssa";
	pass_info.ref_pass_instance_number = 1;
	pass_info.pos_op = PASS_POS_INSERT_AFTER;
	register_callback(info->base_name, PLUGIN_PASS_MANAGER_SETUP, NULL,
			  &pass_info);
	return 0;
}
