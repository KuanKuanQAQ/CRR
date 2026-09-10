> ⚠️ **部分过时(2026-09-10)**。下文"还未实现"一节中"函数级 PIC 与共享 GOT ——
> 当前生成的函数体仍非位置无关"**已不成立**:位置无关化在两个架构上均已实现并通过
> `verify_movable.py` 校验。编译期的当前事实以 [`17-implementation-current.md`](17-implementation-current.md) §1 为准。
> "`fixed_out` 与白名单条目的自动生成"这一条**也已不成立**(2026-09-10 完成,
> 见 17 §1.4/§3.6)。本文"还未实现"一节现已整节作废。

# 编译器支持实现细节（LLVM / GCC）

> 对应论文 §3.5.1（编译器扩展）与 §6.4（编译期静态验证）。
> 作者要求 LLVM 与 GCC 各实现一份。

## 编译器要做的事

对每个被选中的函数 F：

1. F 改名为 `F_body`，放入 `.rand.text.F` —— 函数体，可迁移；
2. 以**原名** F 新建跳板，放入 `.tramp.text.F` —— 地址固定；
3. 发出 F 的 target 槽（`.data..ikaslr_target`）与跳板表项（`.data..ikaslr_tramp_tbl`）；
4. 把所有既有调用点指向跳板。

跳板体等价于：

```c
ikaslr_enter();
r = (*__ikaslr_target_F)(args...);   /* 经 target 间接转移 */
ikaslr_leave();
return r;
```

**选择哪些函数**：名单文件（每行一个函数名），由 `-ikaslr-funcs-file=` 或环境变量
`IKASLR_FUNCS` 指定。用名单而非源码注解，是为了满足论文的设计目标之二——不修改
被随机化子系统的源码。

## LLVM 版（`tools/ikaslr/llvm/`，已实现并验证）

```sh
cd tools/ikaslr/llvm && make          # 产出 libIKaslrPass.so
IKASLR_FUNCS=funcs.txt clang -O2 -c t.c -o t.o \
    -fpass-plugin=$PWD/libIKaslrPass.so
```

> **注意：`-mllvm -ikaslr-funcs-file=` 在 `-fpass-plugin` 下不可用。** 插件的
> `cl::opt` 在命令行解析之后才注册，clang 会报 "Unknown command line argument"。
> 因此内核构建应使用环境变量 `IKASLR_FUNCS`。

### 两处踩过的坑（都会静默破坏不变式）

**其一：必须在内联器之前打 `noinline`。** 否则 `-O2` 会把被随机化函数整个内联进
调用者（实测直接常量折叠成 `lea (%rdi,%rdi,2)`）。内联出来的副本既不经过跳板
（随机化对它无效），也不经过 enter/leave 计数（活跃集合不再准确）。
因此插件注册了两个 pass：`PipelineStart` 处的标记 pass 打 `noinline`，
`OptimizerLast` 处的改造 pass 做实际变换。

**其二：只改名不足以重定向调用者。** 调用指令引用的是函数**对象**而非名字；把原
函数改名为 `F_body` 之后，调用点会跟着指向 `F_body`，等于绕过跳板（实测
`caller_fn` 直接 `jmp target_fn_body`）。正确做法是先建跳板、
`replaceAllUsesWith(跳板)`，再改名落段；target 槽也必须在 RAUW **之后**创建，
否则这条引用会被一并替换。

### 验证

生成的跳板（x86-64，-O2）：

```
target_fn:
    call ikaslr_enter
    mov  __ikaslr_target_target_fn(%rip),%rax
    call *%rax                 # 经 target 间接转移
    call ikaslr_leave
    ret
target_fn_body:                # .rand.text.target_fn，位置无关
    imul %esi,%edi ; lea 0x1(%rdi),%eax ; ret
```

链接后实际运行：

```
target_fn(6,7)=43 caller_fn(5)=16 enters=2 leaves=2
PASS: both calls went through the trampoline
```

### 还未实现

- `fixed_out`（离开随机化区域的跳板）与白名单条目的自动生成；
- 函数级 PIC 与共享 GOT（S1.6/S1.7）—— 当前生成的函数体仍非位置无关，
  故只有自足函数可迁移；
- 第 5 章的 PA 插桩。

## GCC 版（`tools/ikaslr/gcc/`，已编译并运行验证）

```sh
sudo apt install gcc-11-plugin-dev          # 版本须与所用 gcc 一致
cd tools/ikaslr/gcc && make                 # 产出 ikaslr_gcc.so
IKASLR_FUNCS=funcs.txt gcc -O2 -fplugin=./ikaslr_gcc.so -c t.c -o t.o
```

### 编译时补的头文件与 API

- `change_decl_assembler_name` 是 `symtab` 的成员：写成 `symtab->change_decl_assembler_name(...)`；
- `update_stmt` 在 `tree-ssa-operands.h`；改写 GIMPLE 调用需 include `gimple-ssa.h`。

### 验证（用与 LLVM 版**完全相同**的测试文件）

生成的跳板（x86-64，-O2）：压参数寄存器 → `call ikaslr_enter` → 恢复 →
`call *__ikaslr_target(%rip)` 间接转移 → 保护返回值 → `call ikaslr_leave` → `ret`；
`caller_fn` 落到 `target_fn`（跳板）。链接运行：

```
target_fn(6,7)=43 caller_fn(5)=16 enters=2 leaves=2
PASS: both calls went through the trampoline
```

与 LLVM 版结果一致。**两个编译器插件现在都真实可编译、可运行。**

安装依赖后：

```sh
sudo apt install gcc-11-plugin-dev
cd tools/ikaslr/gcc && make
IKASLR_FUNCS=funcs.txt gcc -O2 -fplugin=./ikaslr_gcc.so -c t.c -o t.o
```

设计与 LLVM 版一致，但跳板以**顶层内联汇编**发出而非在 GIMPLE 中构造函数——
后者在 GCC 里要操作 cgraph 与 GIMPLE，代价高且难以验证；前者形式固定、逐架构
各一段，反而更贴近论文对跳板"函数体极短且形式单一"的描述。x86-64 跳板压/弹
参数寄存器（rdi..r9）以保护它们越过 `ikaslr_enter`，压/弹 rax:rdx 以保护返回值
越过 `ikaslr_leave`，6 次压栈维持 16 字节栈对齐。
