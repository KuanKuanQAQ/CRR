# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_IKASLR) += core.o randomize.o whitelist.o fixup.o control.o detect.o
obj-$(CONFIG_IKASLR_XOM_EPT) += xom_ept.o
obj-$(CONFIG_IKASLR_DEBUG) += selftest.o bench.o
obj-$(CONFIG_IKASLR_EVAL_VULN) += evalvuln.o

# 可迁移的一般函数必须避免任何指向区域外的 PC 相对引用（见 randfuncs.c 开头）。
# x86-64: -mcmodel=large -fno-pic 把外部引用编成 64 位绝对地址。
# arm64 : 字面量池随函数搬移可解决全局引用，但 bl 仍是 PC 相对，
#         需由编译器插件改为间接调用（S1.10a/b），此处暂不启用。
ifdef CONFIG_X86_64
CFLAGS_randfuncs.o += -mcmodel=large -fno-pic
obj-$(CONFIG_IKASLR_DEBUG) += randfuncs.o
endif
