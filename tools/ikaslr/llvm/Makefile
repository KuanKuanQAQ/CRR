# SPDX-License-Identifier: GPL-2.0
# I-KASLR LLVM pass 插件。不依赖 cmake。
#
#   make                       构建 libIKaslrPass.so
#   make LLVM_CONFIG=/path/to/llvm-config
#
# 用法（编译内核或任意 C 文件时）：
#   clang -fpass-plugin=/path/libIKaslrPass.so \
#         -mllvm -ikaslr-funcs-file=funcs.txt ...

LLVM_CONFIG ?= $(shell command -v llvm-config || echo /usr/lib/llvm-14/bin/llvm-config)
CXX         ?= clang++

CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags) -fPIC -fno-rtti -O2 -Wall
LDFLAGS  := -shared

all: libIKaslrPass.so

libIKaslrPass.so: IKaslrPass.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f libIKaslrPass.so

.PHONY: all clean
