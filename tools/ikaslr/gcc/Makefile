# SPDX-License-Identifier: GPL-2.0
# I-KASLR GCC 插件。
#
# 需要 gcc-<ver>-plugin-dev：
#     sudo apt install gcc-11-plugin-dev      # 版本须与所用 gcc 一致
#
#   make
#   IKASLR_FUNCS=funcs.txt gcc -O2 -fplugin=./ikaslr_gcc.so -c t.c -o t.o
# 或
#   gcc -fplugin=./ikaslr_gcc.so -fplugin-arg-ikaslr_gcc-funcs=funcs.txt ...

GCC        ?= gcc
PLUGIN_DIR := $(shell $(GCC) -print-file-name=plugin)
CXXFLAGS   := -I$(PLUGIN_DIR)/include -fPIC -fno-rtti -O2 -Wall

all: ikaslr_gcc.so

ikaslr_gcc.so: ikaslr_gcc.c
	@test -f $(PLUGIN_DIR)/include/gcc-plugin.h || \
	 { echo "ERROR: gcc plugin headers missing; install gcc-*-plugin-dev"; exit 1; }
	$(GCC) $(CXXFLAGS) -shared -x c++ $< -o $@

clean:
	rm -f ikaslr_gcc.so

.PHONY: all clean
