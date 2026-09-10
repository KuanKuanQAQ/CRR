# 函数体位置无关改造：怎么做到的（x86-64 与 AArch64）

> 记录把「被随机化的函数体真正可搬移」这件事做通的完整过程：每一处障碍、根因、
> 处理办法与验证判据。这是 §3.4.3 的工程落地，也是所有端到端实验的前置。
> 代码：`tools/ikaslr/llvm/IKaslrPass.cpp`；实验记录：`实验/E-pass-bringup-LLVM插桩落地.md`。

## 0. 结果

| | x86-64 | AArch64 |
| --- | --- | --- |
| 随机化函数数 | **35**（32 VFS 核心 + 3 内建示例） | **34** |
| `.rand.text` | 4 277 B | 5 644 B |
| 函数体内残留的 PC 相对外部引用 | **0** | **0** |
| 内核自测 | ✅ 全通过 | ✅ 全通过 |
| 读 `/proc/ikaslr/stats`（走随机化后的 `seq_puts`/`seq_printf`） | ✅ | ✅ |
| 连续启动 | **10/10** 无崩溃无告警 | **3/3**（TCG） |

---

## 1. 问题：为什么函数体默认搬不动

内核以 `-mcmodel=kernel`（x86）／PIE（arm64）编译，函数体内**指向区域外**的引用
都是 PC 相对的。函数被复制到别的地址后，这些相对偏移全部失效。

实测最直接的证据：`seq_puts_body` 里有

```
e8 f9 bf fd ff    call ffffffff81e55030 <strlen>     ← call rel32
```

复制到 `0xffa00000000350f0` 后，该指令位于 `…5102`，目标变成
`…5107 + (−0x24007)` = **`0xffa0000000011100`**——与实际故障地址**逐位吻合**
（`#PF: supervisor instruction fetch`）。

**函数内部**的相对跳转随函数整体搬移自动保持正确，不必处理。要消除的只有
两类对外引用：**外部函数调用**与**全局变量访问**。

## 2. x86-64：64 位绝对寻址

### 2.1 只有内联汇编能表达

IR 层面表达不出「绝对寻址」，两条看似可行的路都实测无效：

| 尝试 | 结果 |
| --- | --- |
| `inttoptr(ptrtoint @f to i64)` | 被后端**折回** `call rel32` |
| 每函数 `"code-model"="large"` 属性 | LLVM 14 **忽略**，仍是 `call rel32` |

可行的是**内联汇编 + `"s"`（符号）约束**：

```
movabsq $sym, %reg      →  R_X86_64_64
call    *%reg
```

> `"i"`（立即数）约束在目标文件生成阶段会报
> `invalid operand for inline asm constraint 'i'`，**必须用 `"s"`**。

### 2.2 统一的改写规则

pass 对函数体做一件事：**凡指令操作数里（递归地）含全局符号，就重建为
「绝对地址 + 指令」形式**。直接调用因此自然变成间接调用，全局访问变成
先取绝对地址再解引用。

### 2.3 逐个啃掉的五类残留

每一类都是**反汇编整个 `.rand.text` 找 `(%rip)` 和 `call <绝对地址>`** 发现的：

| # | 残留 | 根因 | 处理 |
| --- | --- | --- | --- |
| 1 | `call rel32` 到外部函数 | 最初只做了跳板、没做体改造 | 绝对地址 + 间接调用 |
| 2 | `kmalloc_caches+0x28`、`rename_lock` 仍是 `(%rip)` | 全局藏在 **ConstantExpr**（GEP／bitcast）里，只匹配裸 `GlobalVariable` 会漏 | **递归**重建 ConstantExpr |
| 3 | `invalid operand for inline asm constraint 'i'` | 把 `WARN_ON` 的 `"i"(__FILE__)` 换成了运行期值，破坏了约束 | **内联汇编整条跳过**（这类函数本就被 `gen_funcs.py` 排除） |
| 4 | `call __x86_indirect_thunk_r11` | **retpoline**：间接调用被编成到固定 thunk 的 PC 相对直接调用 | 关闭 retpoline，见 §4 |
| 5 | `call __stack_chk_fail`、`call __memcpy` | **后端生成**的直接调用，IR 里没有对应指令可改 | 对函数体去掉栈保护属性；把 `llvm.memcpy/memset/memmove` 降级成绝对地址间接调用 |

## 3. AArch64：随函数搬移的字面量池

### 3.1 绝对立即数在 arm64 上**不可用**

arm64 内核镜像以 **PIE** 链接（`CONFIG_RELOCATABLE`），链接器直接拒绝绝对重定位：

```
relocation R_AARCH64_MOVW_UABS_G0_NC against `names_cachep'
        can not be used when making a shared object; recompile with -fPIC
```

所以 x86 那套 `movz/movk` 四段拼绝对地址在 arm64 上**编不过**。

### 3.2 正确做法：字面量池

```
ldr $0, =sym        →  汇编器在**本函数所在节**内放一条字面量
blr $0
```

反汇编印证：

```
0000000000000000 <f>:
   4:  ldr  x8, 18 <f+0x18>      ← PC 相对，偏移落在同一节内
   8:  blr  x8
  18:  R_AARCH64_ABS64  ext      ← 字面量，绝对地址
```

两边都成立：**节内相对偏移**随函数整体搬移保持不变；**字面量本身**是
`R_AARCH64_ABS64`，由内核启动时的重定位填成最终地址，复制时already是最终值。

> 这正是论文 §3.4.3 所说「arm64 上共享偏移表／字面量池是**刚需**而非优化」——
> 本次实测给出了它的确切理由：**PIE 镜像不接受绝对重定位**。

## 4. 两条架构级不相容（值得写进正文）

| 架构 | 与可搬移代码不相容的设施 | 为什么 | 处理 |
| --- | --- | --- | --- |
| **x86-64** | **retpoline** | 间接调用被编成到固定 thunk 的 **PC 相对**直接调用（`call __x86_indirect_thunk_r11`），一搬就错 | 随机化范围内必须关闭；或让 thunk 调用也走绝对寻址 |
| **AArch64** | **BTI**（分支目标识别） | 本方案把**直接调用改成了间接调用**，而 BTI 要求间接分支落点必须有 `BTI` 指令；只被直接 `bl` 调用的函数没有落点标记 → `Oops - BTI` | 关闭 `CONFIG_ARM64_BTI_KERNEL`；或让编译器给所有可能成为间接目标的函数加落点 |

两者是同一件事的两面：**把直接调用变成间接调用，就会与"约束间接分支"的硬件
CFI 设施冲突**。x86 上没撞到 IBT，是因为内核开 IBT 时**所有**函数都带 `endbr64`；
arm64 的 BTI 落点是按需发射的，因此撞上了。

> 注意四档配置必须**一致**地关闭这些项，否则测的是缓解措施的开销而非随机化开销。

## 5. 栈回溯：ORC 的问题与处理

### 5.1 问题

ORC（Oops Rewind Capability）是 x86 的栈回溯格式，objtool 在**链接期**生成
`.orc_unwind_ip` 表，把**代码地址**映射到该处的栈帧布局。
函数被搬到变体后，ORC 表项仍指向**原地址**，回溯到搬移后的 PC 就查不到表项。

实测（从一个被随机化的函数里 `dump_stack()`）：

```
Call Trace:
 dump_stack_lvl+0x69/0xa0
 ? ikaslr_control_init+0xa0/0xa0     ← 这些 '?' 是栈扫描猜出来的，且是错的
 ? ikaslr_rf_general+0x24/0x40
 ? test_general_function+0x99/0x160
```

`?` 表示不可靠。之前每次崩溃调用栈全是 `?`，根因就在这里。
影响 oops 回溯、`perf` 调用链、livepatch 一致性检查。

### 5.2 处理：改用帧指针 unwinder

`CONFIG_UNWINDER_FRAME_POINTER=y`（关 `UNWINDER_ORC`）。帧指针回溯在运行时沿
`%rbp` 链走，**不依赖任何以地址为键的表**，因此对搬移后的代码天然成立。

配套还需一处 pass 改动：**跳板必须继承原函数的 `frame-pointer` 属性**。
跳板是 pass 凭空造的函数，默认不建帧指针，回溯走到跳板就断链
（实测回溯里仍满是 `?`）。现在一并继承 `frame-pointer`／`target-cpu`／
`target-features` 等代码生成属性。

**代价**：帧指针会占用一个寄存器并增加序言/尾声，对性能有影响。
由于**四档配置一律相同**，A/B 对比仍然有效；但绝对数字包含了这部分开销，
正文报告时要写明。

### 5.3 更"正确"的做法（未做，列为后续）

内核对模块有 `unwind_module_init()` 这类机制来注册动态代码的 ORC。
完备方案应当在 `prepare` 时**把被搬移函数的 ORC 表项按新地址重建并注册**，
并在 `orc_find()` 中查询 I-KASLR 的表。这样可以保留 ORC。
本文选择帧指针方案是工程取舍，不是原理限制。

## 6. 顺带发现并修复的一个真实内核 bug

只有当**真实内核函数**（而非自足的示例函数）被随机化后才会暴露：

```
WARNING: ... rcu_note_context_switch+0x214/0x520
Voluntary context switch within RCU read-side critical section!
```

**根因**：`ikaslr_wait_unblocked()` 用 `preemptible()` 判断能否睡眠：

```c
if (preemptible())
        wait_event(...);       /* 睡 */
else
        cpu_relax();
```

但在 `CONFIG_PREEMPT_RCU` 下 **`rcu_read_lock()` 并不关抢占**，只是给
`rcu_read_lock_nesting` 加一，所以 RCU 读端临界区里 `preemptible()` **仍为真**。
而 VFS 的 **rcu-walk 路径查找**会在 RCU 读端临界区里调用 `dput`／`inode_permission`
这类函数——一旦它们被随机化，`fixed_in` 就会在 RCU 读端临界区里睡眠。

实测：6 次启动中 2 次触发。

**修复**：判据改为 `preemptible() && !rcu_preempt_depth()`。
阻断窗口只有微秒量级（关键路径实测 0.5–2 µs），不可睡上下文里自旋完全可接受。
`ikaslr_wait_region_empty()` 有同样的判据，一并修正。

修复后 **10/10 启动干净、无告警**。

> **对正文的意义**：这是 D3「覆盖非抢占上下文」的一个具体侧面——
> 不能睡的上下文不只有"关抢占/关中断"，**RCU 读端临界区**同样不能睡，
> 而它恰恰是 VFS 热路径的常态。建议在 §3.4.6 明确列出。

## 7. 构建系统：让改名单能触发重编

Kbuild 的 `if_changed` 只比较**编译命令行**，而插件 `.so` 与 `IKASLR_FUNCS`
都不在命令行里——改了名单或重编了 pass **不会触发重编**，于是镜像里混着
两份名单编出来的对象（本次踩过两次，一次表现为链接期 `.eh_frame` 报错、
一次表现为"改成 2 函数名单后仍显示 35 个函数"）。

处理：把两者的哈希折进命令行。

```make
IKASLR_STAMP := $(shell cat $(IKASLR_PASS) $(IKASLR_FUNCS) 2>/dev/null | md5sum | cut -c1-16)
KBUILD_CFLAGS += -fpass-plugin=$(IKASLR_PASS) -DIKASLR_STAMP=0x$(IKASLR_STAMP)
```

验证：名单不变 → **0** 次重编；换名单 → **2 793** 次重编。

## 8. 验证判据（每次改动后都应跑）

1. **静态**：反汇编整个 `.rand.text`，
   x86 上 `(%rip)` 与 `call <绝对地址>` 应为 **0**；
   arm64 上 `adrp` 与指向区域外的 `bl/b` 应为 **0**。
2. **动态**：内核自测全通过；读 `/proc/ikaslr/stats`（本身要走随机化后的
   `seq_puts`/`seq_printf`）成功；连续启动无 oops、**无 WARNING**。

> 第 2 条里「无 WARNING」不能省——RCU 那个 bug 只体现为 WARNING，
> 只 grep oops 会漏掉。

## 9. 复现

```sh
make -C tools/ikaslr/llvm                          # 编 pass

# x86-64
make O=<b> CC=clang CRR_TRAMPOLINE=n x86_64_defconfig
scripts/config --file <b>/.config -e IKASLR -e IKASLR_DEBUG \
    -d RETPOLINE -d RETHUNK -d UNWINDER_ORC -e UNWINDER_FRAME_POINTER
make O=<b> CC=clang CRR_TRAMPOLINE=n olddefconfig
make O=<b> CC=clang CRR_TRAMPOLINE=y \
     IKASLR_FUNCS=$PWD/scripts/ikaslr/funcs/s1-vfs.txt -j$(nproc) bzImage

# AArch64
make O=<a> ARCH=arm64 CC=clang CROSS_COMPILE=aarch64-linux-gnu- \
     CRR_TRAMPOLINE=n defconfig
scripts/config --file <a>/.config -e IKASLR -e IKASLR_DEBUG -d ARM64_BTI_KERNEL -d ARM64_BTI
make O=<a> ARCH=arm64 CC=clang CROSS_COMPILE=aarch64-linux-gnu- \
     CRR_TRAMPOLINE=n olddefconfig
make O=<a> ARCH=arm64 CC=clang CROSS_COMPILE=aarch64-linux-gnu- \
     CRR_TRAMPOLINE=y IKASLR_FUNCS=$PWD/scripts/ikaslr/funcs/s1-vfs.txt -j$(nproc) Image
```

## 10. 仍未做的

| 项 | 说明 |
| --- | --- |
| ORC 表项搬移 | 当前用帧指针 unwinder 回避；完备方案见 §5.3 |
| 全局引用的 ConstantExpr 覆盖面 | 已覆盖 GEP/bitcast 等常见形式；更奇特的常量表达式尚未穷举，判据是 §8 的静态扫描 |
| arm64 共享 GOT | 当前每函数一个字面量池条目；论文 §3.4.3 的**共享**单表尚未实现（省体积，不影响正确性） |
| 更大范围 | 目前验证到 S1（32 个函数）。S2/S3 需重跑 §8 的判据 |
