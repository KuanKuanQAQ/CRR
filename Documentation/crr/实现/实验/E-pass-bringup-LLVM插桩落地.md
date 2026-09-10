# 编译器 Pass 落地：从"能编"到"能跑"——以及暴露的一个根本缺口

| | |
| --- | --- |
| **清单编号** | E3-8 的延伸；解锁 E3／E4~E7／E9／E10／E12 的前置 |
| **对应正文** | §3.5.1（Pass 实现）、§3.4.3（函数级位置无关） |
| **状态** | 🟡 Pass 可用、内核可编可启动；**但被随机化的函数体尚不可搬移** |
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

## 3. 暴露的根本缺口：**函数体不是位置无关的**

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

## 4. 下一步（明确的工作项）

1. **在 pass 里实现函数体的位置无关改造**（这是解锁所有端到端实验的真正前置）：
   - 把体内对外部函数的 `call` 改为经 `fixed_out`；
   - 把对全局变量的 RIP 相对访问改为绝对寻址（x86）／共享 GOT（arm64）。
2. 把 `libIKaslrPass.so` 与 `IKASLR_FUNCS` 加进构建依赖，避免用陈旧对象。
3. **ORC**：被搬移函数的 ORC 表项必须一并搬移，否则栈回溯失效——
   本次崩溃的调用栈全是 `?`，正是这个问题的直接表现。

## 5. 复现

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
