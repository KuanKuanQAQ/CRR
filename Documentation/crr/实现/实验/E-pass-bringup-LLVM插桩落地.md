# 编译器 Pass 落地：从"能编"到"能跑"——以及暴露的一个根本缺口

| | |
| --- | --- |
| **清单编号** | E3-8 的延伸；解锁 E3／E4~E7／E9／E10／E12 的前置 |
| **对应正文** | §3.5.1（Pass 实现）、§3.4.3（函数级位置无关） |
| **状态** | ✅ **打通**：35 个真实内核函数被随机化，内核可编、可启动、自测通过、8/8 稳定 |
| **环境** | Linux 6.8，x86-64，clang 14 + GNU binutils，QEMU+KVM |

## 1. 目标

让 `CRR_TRAMPOLINE=y` 真正生效：用 `scripts/ikaslr/funcs/` 的名单编出一个
**真正随机化了内核函数**的内核，从而解锁所有端到端性能实验。

## 2. 做成的部分

### 2.1 Pass 本身可用

`tools/ikaslr/llvm/IKaslrPass.cpp` 在独立用例与真实内核代码上都验证通过：

- 名单里的函数 `F` → 改名 `F_body` 落 `.rand.text.F`；
- 新建同名跳板 `F` 落 `.tramp.text.F`；未列入的函数不受影响；
- 发出 `__ikaslr_target_F` 槽与 `.data..ikaslr_tramp_tbl` 表项。

生成的跳板正是设计的协议：

```
call ikaslr_enter
mov  <target 槽>(%rip),%rax
call *%rax
call ikaslr_leave
ret
```

> **注意**：`-mllvm -ikaslr-funcs-file=` 不生效（`-mllvm` 选项在插件注册之前被解析）。
> **必须用环境变量 `IKASLR_FUNCS`**，内核构建也走这条路。

### 2.2 构建系统改对了

原 `Makefile` 指向的是一条遗留路径（llvm-tutor 的 `libInjectTrampoline.so`），
与本项目的 pass 无关。已改为指向 `tools/ikaslr/llvm/libIKaslrPass.so`，并加了三道检查：
pass 存在、编译器是 clang、`IKASLR_FUNCS` 已设且文件存在。

```sh
make O=<b> CC=clang CRR_TRAMPOLINE=y \
     IKASLR_FUNCS=$PWD/scripts/ikaslr/funcs/s1-vfs.txt -j$(nproc) bzImage
```

> 本机无 `ld.lld`，`LLVM=1` 会失败；用 **`CC=clang` + GNU binutils** 即可。

### 2.3 修掉两个阻塞构建的问题

| 问题 | 根因 | 处理 |
| --- | --- | --- |
| 链接期 `unplaced orphan section '.eh_frame'` | 跳板是 pass 凭空造的函数，**不继承翻译单元的代码生成选项**；内核以 `-fno-asynchronous-unwind-tables` 编译并丢弃 `.eh_frame`，而新建函数默认带 `uwtable` | 在 pass 里对跳板 `removeFnAttr(UWTable)` + `addFnAttr(NoUnwind)` |
| 改了名单或 pass 后不重编 | `make` 不把 `.so` 与 `IKASLR_FUNCS` 当依赖 | 暂需手动删除含 `.tramp.text` 的目标文件；**应把二者加进依赖** |

### 2.4 内核能编、能启动

以 S1（32 个函数）编译成功：`.rand.text` 3 365 B、`.tramp.text` 1 997 B，
启动时正确注册 **35 个可随机化函数**（32 + 3 个内建示例），变体池就绪，
`/proc/ikaslr` 可用。以 2 个函数的最小名单编译时，**内核内自测全部通过**
（dispatch/tracking/block/rerand/defer/fixed_out/stale-return）。

## 3. 曾暴露的根本缺口：函数体不是位置无关的（**已解决**）

内核启动后，只要**真正调用到被随机化的函数**就会崩溃：

```
BUG: unable to handle page fault for address: ffa0000000011100
#PF: supervisor instruction fetch in kernel mode
```

### 定位过程

1. 目标槽是**对的**——在 `ikaslr_enter()` 里 dump，`seq_puts` 槽 = `ffa00000000350f0`，
   正落在 live 变体 `[ffa0000000035000, …)` 内；每一轮随机化后校验也全部正确。
2. 跳板汇编是**对的**——`call ikaslr_enter` 在前、`mov 槽,%rax` 在后（`%rax` 不会被
   `ikaslr_enter` 破坏）、再 `call *%rax`。
3. 不读 `/proc` 就**能正常启动**；一旦走到 seq_file 路径就崩。
4. 反汇编函数体，真相大白：

```
ffffffff81e79020 <seq_puts_body>:
  …
  e8 f9 bf fd ff    call ffffffff81e55030 <strlen>     ← PC 相对 call rel32
```

函数体被复制到 `0xffa00000000350f0` 后，该 `call` 位于 `…5102`，
目标 = `…5107 + (−0x24007)` = **`0xffa0000000011100`**——与故障地址**逐位吻合**。

### 结论

**Pass 只实现了 §3.4.2 的"跳板 + target 槽"，没有实现 §3.4.3 的"函数级位置无关"。**
函数体仍是 `-mcmodel=kernel` 的产物，内含指向区域外的 PC 相对引用，一搬就错。

这与 `../14-code-model-and-unmovable.md` 的分析完全一致：函数要能整体搬移，
**必须先消掉函数体内所有指向区域外的 PC 相对引用**：

| 引用类型 | x86-64 应改成 | 现状 |
| --- | --- | --- |
| 外部函数调用 | 经 `fixed_out` 跳板（§3.4.3 优化一） | ❌ 仍是 `call rel32` |
| 全局变量 | 64 位绝对立即数 `movabs` | ❌ 仍是 `mov off(%rip)` |

现有的 3 个内建示例函数之所以能跑，正是因为 `randfuncs.c` 用
`-mcmodel=large -fno-pic` 单独编译、`selftest.c` 里的函数是刻意写成自足的。

## 4. 位置无关改造的实现（§3.4.3 落地）

### 4.1 x86-64 上怎么做到

IR 层面表达不出"绝对寻址"：`inttoptr(ptrtoint @f)` 会被后端折回 `call rel32`，
LLVM 14 也**不支持每函数的 `code-model` 属性**（两者都实测无效）。
可行的办法是**内联汇编 + `"s"`（符号）约束**把符号地址物化成 64 位绝对立即数：

```
movabsq $sym, %reg      →  R_X86_64_64 重定位
call    *%reg
```

于是 pass 对函数体做统一改写：**凡操作数里（递归地）含全局符号，就重建为
"绝对地址 + 指令"形式**。直接调用因此自然变成间接调用，全局变量访问变成
先取绝对地址再解引用。

### 4.2 逐个啃掉的五类残留（每一类都是实测发现的）

| # | 残留 | 为什么 | 处理 |
| --- | --- | --- | --- |
| 1 | `call rel32` 到外部函数 | 最初只做了跳板，没做体改造 | 绝对地址 + 间接调用 |
| 2 | `kmalloc_caches+0x28`、`rename_lock` 仍是 `(%rip)` | 全局藏在 **ConstantExpr**（GEP／bitcast）里，只匹配裸 `GlobalVariable` 会漏 | 递归重建 ConstantExpr |
| 3 | `invalid operand for inline asm constraint 'i'` | 把 `WARN_ON` 的 `"i"(__FILE__)` 换成了运行期值 | **内联汇编整条跳过**（这类函数本就被 `gen_funcs.py` 排除） |
| 4 | `call __x86_indirect_thunk_r11` | **retpoline**：间接调用被编成到固定 thunk 的 PC 相对直接调用 | 关闭 retpoline（四档需一致） |
| 5 | `call __stack_chk_fail` / `call __memcpy` | 后端生成的直接调用，IR 里没有对应指令 | 对函数体去掉栈保护属性；把 `llvm.memcpy/memset/memmove` 换成绝对地址间接调用 |

**验证判据**：反汇编整个 `.rand.text`，`(%rip)` 与 `call <绝对地址>` 应为 **0** 处。
实测 35 个函数体全部通过。

> **一条值得写进正文的约束**：**retpoline 与可搬移代码在 x86 上不相容**——
> 间接调用会被编译成到固定 thunk 的 PC 相对调用。随机化区域内必须关闭 retpoline，
> 或让 thunk 调用也走绝对寻址。

### 4.3 结果

以 S1 编译：**35 个可随机化函数**（32 个 VFS 核心 + 3 个内建示例），
`.rand.text` 4 277 B。内核启动、**内核内自测全部通过**、`/proc` 可读
（读 `/proc/ikaslr/stats` 本身就要走随机化后的 `seq_puts`/`seq_printf`），
**连续 8 次启动全部通过、无一崩溃**。

实测运行数据（35 个函数）：

| 量 | 值 |
| --- | ---: |
| 关键路径 `critical_path_ns` | 481～1 913 ns |
| 其中 `cp_update_ns`（35 个槽） | 116～387 ns |
| 其中 `cp_wait_ns` | 41～223 ns |
| 改映射 `cp_remap_ns` | 17.9～23.0 µs |
| 变体准备 `prepare_ns` | 114 µs |
| 陈旧返回被兜底修正 | 3 次（`stale_fixup_fails` = 0） |

> 连续 200 次触发中 `rounds_missed` = 199：变体池被抽干，补充工作队列跟不上，
> 属预期行为（无就绪变体时返回 `-EAGAIN`），不是错误。**这正是 E9 频率敏感性
> 要量化的那条上限。**

## 5. 仍待处理

1. 把 `libIKaslrPass.so` 与 `IKASLR_FUNCS` 加进构建依赖——现在改名单/改 pass
   **不会触发重编**，必须手动删掉含 `.tramp.text` 的目标文件，已踩过两次。
2. **ORC**：被搬移函数的 ORC 表项未搬移，栈回溯失效（崩溃时调用栈全是 `?`）。
3. arm64 侧的等价改造（`adrp` → 共享 GOT，`bl` → 间接）。

## 6. 复现

```sh
make -C tools/ikaslr/llvm                      # 编 pass
make O=<b> CC=clang CRR_TRAMPOLINE=n x86_64_defconfig
scripts/config --file <b>/.config -e IKASLR -e IKASLR_DEBUG
make O=<b> CC=clang CRR_TRAMPOLINE=n olddefconfig
# 改名单后必须先清掉旧对象：
find <b> -name '*.o' | while read o; do
  readelf -SW "$o" 2>/dev/null | grep -q '\.tramp\.text' && rm -f "$o"; done
rm -f <b>/vmlinux.o <b>/vmlinux
make O=<b> CC=clang CRR_TRAMPOLINE=y \
     IKASLR_FUNCS=$PWD/scripts/ikaslr/funcs/s1-vfs.txt -j$(nproc) bzImage
```
