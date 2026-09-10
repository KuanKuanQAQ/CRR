# 17 当前实现全貌 —— 以代码为准

> 校准时间:2026-09-10。分支 `topic/ikaslr`。
>
> **这份文档的定位**:此前的实现文档是分阶段写的,写完之后代码又改了,已经出现
> 与代码不符的地方(见 §0.2)。这一份是**对着代码逐条核对后重写**的,每个论断都给
> 出 `文件:行号`,你可以直接翻代码验证。凡是我没有实测过的,一律写在 §9 的
> "未实现 / 未验证"里,不混进正文当成已完成。

---

## 0. 先说结论与旧文档的关系

### 0.1 一句话

被选中的内核函数在**编译期**被拆成"固定地址的跳板 + 可搬移的函数体",所有既有
调用者不动地落到跳板上;**运行时**准备好新的代码副本,阻断入口、等区域内执行流
清空、改写每函数一个 `target` 槽、放行,再把旧副本整体换成陷阱页。
索引更新量 = 函数个数,与调用点数量无关。

### 0.2 旧文档哪几条已经不对了

| 文档 | 过时的论断 | 现在的事实 |
|---|---|---|
| `07-compiler.md` "还未实现" | "函数级 PIC 与共享 GOT —— **当前生成的函数体仍非位置无关,故只有自足函数可迁移**" | 已实现。x86 用 `movabs` 绝对物化,arm64 用随函数搬移的字面量池。`verify_movable.py` 在两个架构上都扫出 0 条区域外 PC 相对引用。见 §3.2 / `16-position-independence.md` |
| `07-compiler.md` "还未实现" | "`fixed_out` 与白名单条目的自动生成" | **仍然没有实现**,这条是对的。见 §9.1 —— 这是当前最大的缺口 |
| `03-randomization.md` | 全文 0 处提到陷阱影像 / 改映射 / `trap_base` | §3.5.5 的陷阱影像+改映射已经落地在 `randomize.c:170/190/417`。见 §5.4 |
| `03-randomization.md` 章节表 | 停在 "S1.6/S1.7 一般函数的可迁移性(已完成)" | 之后还做了 trap image、三阶段计时、RCU 睡眠修复等 |

以下正文以代码为准。

### 0.3 源码清单

```
kernel/ikaslr/core.c        395   进入/离开协议、阻断、跳板表构建
kernel/ikaslr/randomize.c   808   变体池、排布、切换、陷阱影像与改映射
kernel/ikaslr/fixup.c       146   陈旧返回地址的陷阱捕获与 PC 重定向
kernel/ikaslr/whitelist.c    99   跨区域目标白名单(哈希表)
kernel/ikaslr/control.c     157   /proc/ikaslr/{stats,trigger,layout}
kernel/ikaslr/selftest.c    637   内核内自检(检测机制只靠它验证)
kernel/ikaslr/randfuncs.c    75   手写范式的示例随机化函数
kernel/ikaslr/bench.c       177   微基准
kernel/ikaslr/detect.c      152   第 4 章 泄漏检测
kernel/ikaslr/xom_ept.c    1030   第 4 章 x86 EPT 只执行内存
kernel/ikaslr/pacfi.c       414   第 5 章 ARM PA CFI
include/linux/ikaslr.h      187   对外接口与手写范式宏
tools/ikaslr/llvm/IKaslrPass.cpp  516   编译期改写
```

---

## 1. 编译期:LLVM pass 做了什么

`tools/ikaslr/llvm/IKaslrPass.cpp`,New Pass Manager 插件,通过
`-fpass-plugin=.../libIKaslrPass.so` 挂进 clang。

**函数名单**来自环境变量 `IKASLR_FUNCS` 指向的文本文件(每行一个函数名)。
> 注意:`-mllvm -ikaslr-funcs-file=` 这条路走不通 —— `-mllvm` 选项在插件注册**之前**
> 就被解析,选项还不存在。所以只能用环境变量。这是踩过的坑,别再改回去。

### 1.1 三步改写(`IKaslrPass.cpp:341` `transform`)

对名单里的每个函数 `F`:

1. **改名** `F` → `F_body`,section 设为 `.rand.text.F`;
2. **新建**一个同名同签名的 `F`,section 设为 `.tramp.text.F`,体是
   ```
   ikaslr_enter();
   r = (*__ikaslr_target_F)(args...);   // 经 target 槽间接转移
   ikaslr_leave();
   return r;
   ```
3. **发出** `__ikaslr_target_F`(初值 = `F_body` 的链接期地址)和一个
   `struct ikaslr_tramp` 表项指针(`IKaslrPass.cpp:125` `emitTableEntry`)。

**为什么用改名而不是遍历调用点重写**:改名之后,所有原有的 `call F` 自然就落到了
跳板上,一个调用点都不用碰。这正是"调用者不必修改"的做法,也是索引更新量能做到
O(函数数) 而非 O(调用点数) 的直接原因。

跳板还继承了原函数的 `frame-pointer` / `target-cpu` / `target-features` 属性,
并且被显式去掉 `uwtable`、加上 `nounwind`
—— 否则 pass 合成的跳板会带出 `.eh_frame`,链接期报孤儿段错误。

### 1.2 位置无关化(`IKaslrPass.cpp:253` `makeBodyPositionIndependent`)

函数体要能搬,就不能有任何**指向区域外的 PC 相对引用**。pass 把函数体里所有对
全局对象/函数的引用改成**绝对地址物化**(`IKaslrPass.cpp:179` `materializeAbs`):

| 架构 | 生成 | 重定位 | 为什么是这个 |
|---|---|---|---|
| x86-64 | `movabsq $sym, %reg` | `R_X86_64_64` | 64 位立即数,与 PC 无关 |
| AArch64 | `ldr $0, =sym` | `R_AARCH64_ABS64` | 字面量池**随函数一起搬移**,偏移不变 |

> arm64 为什么不用 `MOVZ/MOVK` 立即数序列:内核映像是 PIE,链接器直接拒绝
> `R_AARCH64_MOVW_UABS_*`。字面量池是唯一可行解。

嵌套的 `ConstantExpr`(如 `getelementptr`/`bitcast` 套全局)由 `rebuildAbs`
(`:228`)递归重建;`llvm.memcpy/memset/memmove` 被降级成对绝对地址的间接调用。

三个必须知道的边界情况:
- **PHI 节点**:物化指令不能插在 PHI 前面(IR 非法)。改成在对应的**前驱基本块**
  末尾物化。踩过 —— 不这么做 clang 在 `fs/filesystems.c` 上直接 SIGSEGV。
- **内联汇编**:整个跳过,不改。踩过 —— 改写 `WARN_ON` 的 `"i"(__FILE__)` 会报
  `invalid operand for inline asm constraint 'i'`。
- **栈保护属性**:在函数体上丢弃,否则会引入对 `__stack_chk_guard` 的 PC 相对访问。

### 1.3 函数体里的调用去了哪(实测,S3 范围 488 函数)

| 类别 | 数量 | 目标地址会动吗 | pass 的处理 |
|---|---:|---|---|
| ① 调用**被随机化**的函数 | 351 | 会动 | 改名的自然结果 → 落到 `fixed_in` 跳板;跳板地址固定,物化之 |
| ② 同编译单元内 `static` 函数 | 128 | 不动 | 物化绝对地址 |
| ③ 其他编译单元的全局函数 | 1204 | 不动 | 物化绝对地址 |
| **直接指向会移动的函数体** | **0** | — | **及格线,通过** |

第四行是这套机制正确性的**充要检查**:只要函数体里没有任何直接指向"会移动的
代码"的地址,搬移就不会错。目前是 0。

注意 ①:**间接调用的目的地址,经编译器改写后已经是 `fixed_in` 跳板了**,它们在
随机化时天然不需要修改。这是设计的核心,也是 O(1) 索引更新的来源。

### 1.4 fixed_out:离开随机化区域的跳板

②③ 两类出站调用被改写成(`IKaslrPass.cpp` `wrapOutboundCalls`):

```
ikaslr_out_enter(target);   // 记录离开 + 白名单检查(第 5 章再叠加 PA 验证)
r = target(...);
ikaslr_out_leave();         // 重新进入
```

同时为每个目标发出一条 `.data..ikaslr_whitelist` 条目。生成的代码(x86,实测):

```
movabs $0xffffffff81283920,%rdi   ; 参数 = 目标地址
movabs $0xffffffff810b8be0,%rax   ; ikaslr_out_enter
call   *%rax
movabs $0xffffffff81283920,%rax   ; 目标
call   *%rax                      ; 真正的调用
movabs $0xffffffff810b8c70,%rax   ; ikaslr_out_leave
call   *%rax
```

**哪些调用不包**:
- ① 调用被随机化的函数 —— RAUW 之后落到 `fixed_in` 跳板,跳板自己 enter/leave,
  再包一层只是徒增两次原子操作;
- `ikaslr_out_enter`/`out_leave` 自身(无限递归);
- 内联汇编、intrinsic、`musttail`(其后不允许插指令);
- **间接调用**:目标是运行期值,静态白名单覆盖不到,包了只会让每次调用都走
  "未登记目标"的告警路径。间接调用的合法性检查属于第 5 章 PA CFI 的范畴。

**计数不平衡的安全方向**:`out_enter` 减、`out_leave` 加。若被调用方不返回
(`noreturn`),计数偏**小** —— 区域显得更空,随机化更容易进行,不会误判为
"仍有执行流"。反向(计数偏大)才会让随机化永远等不到空,因此必须保证每个
`out_enter` 后面紧跟唯一的 `out_leave`:`CallInst` 不是终结指令,其
`getNextNode()` 必然存在,插入点唯一,配对精确。

`IKASLR_NO_OUTWRAP=1` 可关闭本改写,用于 A/B 测量 fixed_out 自身的开销。

### 1.5 pass 拒绝改造的函数

| 拒绝原因 | 为什么 |
|---|---|
| **变参函数** | 跳板要把收到的变参原样转给函数体,IR 层做不到——只能转发**具名**参数,变参连同 x86-64 的 `%al`(向量寄存器计数)一起丢失。唯一能原样转发的是 `musttail call`,但 musttail 要求其后紧跟 `ret`,而跳板在调用之后**必须**执行 `ikaslr_leave()`,两者不相容 |
| **`naked` 函数** | 没有编译器生成的序言/尾声,函数体就是一段裸汇编 |

> 这条是 2026-09-10 补上的,补之前是一个**静默的数据损坏**:`seq_printf` 被随机化后,
> 跳板只转发了 `%rdi`/`%rsi`,`seq_printf_body` 的 `va_start` 读到的全是垃圾。
> 表现为 `/proc/self/status` 读出乱码、`vsnprintf` 报 "field width too large"、
> 以及读野指针的缺页。启动看起来是"成功"的,所以先前的 10/10 干净启动没有发现它。
> 现在 pass 会打印 `ikaslr: skipping <fn>: variadic ...`,`02-build-kernels.sh`
> 把这些名字收进 `$O/ikaslr-skipped.txt`。

pass 还有两件**不做**的事:
- **不动内联汇编**,因此内联汇编里的 per-CPU 访问、`lock bts` 之类的全局内存操作数
  保持原样 —— 这类函数**不可搬移**,必须从名单里剔除(见 §6.3)。
- **不处理 ORC 展开表**(见 §7.2)。

### 1.6 参数与返回值的 ABI 属性

跳板必须继承函数体的参数/返回值属性(`sret`、`byval`、`zeroext`/`signext`),
并把它们原样加到跳板对函数体的调用上。少了它们,调用者按一种约定传参、跳板按
另一种收,是静默的寄存器/栈错位。

### 1.7 构建接线(`Makefile:562-600`)

```make
CRR_TRAMPOLINE ?= n
IKASLR_PASS ?= $(srctree)/tools/ikaslr/llvm/libIKaslrPass.so
export IKASLR_FUNCS
ifeq ($(CRR_TRAMPOLINE),y)
  # 检查:插件存在、CC 确实是 clang、IKASLR_FUNCS 已设且可读
  IKASLR_STAMP := $(shell cat $(IKASLR_PASS) $(IKASLR_FUNCS) | md5sum | cut -c1-16)
  KBUILD_CFLAGS += -fpass-plugin=$(IKASLR_PASS) -DIKASLR_STAMP=0x$(IKASLR_STAMP)
endif
```

`IKASLR_STAMP` 不是装饰:kbuild **不跟踪** `.so` 和函数名单的变化,换了名单或
重编了插件,旧 `.o` 会被当成最新的复用。这个坑踩过两次,现在把两者的哈希塞进
`KBUILD_CFLAGS`,内容一变命令行就变,kbuild 自然全量重编。

---

## 2. 链接期:段与表

```
.tramp.text.<fn>          固定地址跳板,永不迁移    [__tramp_text_start, __tramp_text_end)
.rand.text.<fn>           随机化函数体,可迁移      [__rand_text_start,  __rand_text_end)
.data..ikaslr_tramp_tbl   每函数一项 struct ikaslr_tramp *
.data..ikaslr_target      target 槽 —— 随机化时**唯一**被改写的数据
.data..ikaslr_whitelist   合法跨区域目标
```

`struct ikaslr_tramp`(`include/linux/ikaslr.h:44`)= `{tramp, body, target, name, size}`。

> 表里存的是**指针**而不是结构体本身。原因:x86-64 把 ≥32 字节的数据对象按 32 字节
> 对齐,而本结构 40 字节,直接排布会在表项之间留 24 字节空洞,按 `sizeof` 索引就
> 落进填充区。存 8 字节指针天然紧密,这也是内核既有表(`__start___tracepoints_ptrs`)
> 的通行做法。

`size` 字段在初始化时由**相邻函数体地址之差**算出(`core.c:304` `ikaslr_build_table`
先按 body 地址排序)。

---

## 3. 运行时

### 3.1 变体池

4 个预分配变体,状态机 `FREE → READY → LIVE → RETIRED`
(`randomize.c` 顶部 `enum ikaslr_var_state`)。

```c
struct ikaslr_variant {
    void *base;  unsigned long cap, used, *off;
    enum ikaslr_var_state state;
    bool poisoned;        /* 退役后陷阱是否已就位 */
    unsigned long round;
    void *trap_base;      /* 预建的陷阱影像 */
    struct page **code_pg, **trap_pg;
    bool trapped;         /* base 当前是否已重指到 trap_pg */
};
```

预分配是为了**关键路径零分配**:切换那一段不能睡、不能等内存。补充由
`ikaslr_refill_work`(`randomize.c:482`)在后台工作队列里做。

### 3.2 进入 / 离开协议(`core.c`)

- `ikaslr_enter()` (`core.c:102`):若 `blocked` 则等待放行,然后 `active++`。
- `ikaslr_leave()` (`core.c:135`):`active--`,归零时唤醒随机化线程。
- `ikaslr_out_enter(target)` (`core.c:179`):**离开**区域时 `active--`(方案 B)。
- `ikaslr_out_leave()` (`core.c:216`):从外部函数返回时 `active++`,**不等待阻断**。

**方案 B 为什么这么选**:离开区域时减计数,随机化就不必等到"调用外部函数的执行流
返回"。真实内核路径上,随机化函数阻塞在 I/O 里是常态;若不减计数,区域会长期非空,
随机化根本无法进行。代价是本执行流的栈帧仍在栈上、返回地址指向旧变体 —— 这个由
陷阱 + fixup 兜底(§3.5)。

`ikaslr_out_leave` **不能**等待阻断标志:那次返回是回到调用者的栈帧,不是新的进入;
若在此阻塞,会与"等待计数归零"的随机化线程互相死等。

**一个被 PREEMPT_RCU 咬过的坑**(`core.c:70`):
```c
if (preemptible() && !rcu_preempt_depth())
        wait_event(ikaslr_unblock_wq, !READ_ONCE(ikaslr_blocked));
else
        cpu_relax();
```
`rcu_read_lock()` 在 PREEMPT_RCU 下**不关抢占**,所以 RCU 读侧临界区里
`preemptible()` 为真。VFS 的 rcu-walk 会在这种上下文里调用随机化过的
`dput`/`inode_permission`,直接 `wait_event` 就是 RCU 读侧睡眠,内核报 WARNING。
必须再查 `rcu_preempt_depth()`。

### 3.3 一次随机化的时序(`randomize.c:513` `ikaslr_rerandomize`)

```
t0
├─ 阶段一 等待   ikaslr_block_region()            禁止新执行流进入
│               ikaslr_wait_region_empty(TIMEOUT) 等 active 归零
│               超时 → unblock + 放弃本轮(不冒险)
├─ 阶段二 更新   for each fn: ikaslr_update_target(t, 新地址)   ← O(函数数)
│               next->state = LIVE;  old->state = RETIRED;  ikaslr_live = next
│               ikaslr_unblock_region()            ← 放行,可见窗口到此结束
├─ 记录 ikaslr_last_ns
└─ 阶段三 退役   ikaslr_retire_trap(old)          把旧变体换成陷阱页
                 schedule_work(&ikaslr_refill_work)
```

三段分别计时:`cp_wait_ns` / `cp_update_ns` / `cp_remap_ns`,经
`ikaslr_rand_phases()` 导出。**阻断窗口只覆盖阶段一+二**;阶段三在放行之后做,
不计入停顿。

新变体的排布(`ikaslr_plan` `:232`)与就位(`ikaslr_prepare` `:296`)都在**上一轮之后
的后台**完成,切换时刻变体已经是 `READY`(已排布、已置 RO+X)。

### 3.4 陷阱影像 + 改映射(§3.5.5,`randomize.c:170/190/417`)

退役变体不能留着旧代码 —— 那是现成的 gadget 来源,也让陈旧返回悄悄执行到旧代码。
处理方式:

1. `ikaslr_build_trap_image()` (`:190`) 在**后台**预先建好一份同样大小的
   全 `int3`(x86) / `BRK #IMM`(arm64) 影像页 `trap_base`;
2. 退役时 `ikaslr_remap()` (`:170`) 走 `apply_to_page_range`,把 `base` 这段虚拟地址
   的 PTE **重指**到 `trap_pg`。

**为什么是改映射而不是 memset**:memset 要写满整个变体,时间随代码量线性增长且
要先把页改可写;改映射只改 PTE,代价与代码量无关。这是把退役开销从
O(代码字节数) 降到 O(页数) 的关键。

### 3.5 陈旧返回地址的修正(`fixup.c`)

陷阱触发后:
- **x86-64**:`int3` → die notifier 的 `DIE_INT3`,可干净恢复;
- **arm64**:`BRK #IKASLR_BRK_IMM` → **内核 break hook**(`register_kernel_break_hook`)。

> arm64 为什么不用 UDF + die notifier:未定义指令走 `die()`,即使 notifier 返回
> `NOTIFY_STOP` 能恢复,也会**每次打印 oops** 污染内核日志。BRK + break hook 才是
> x86 `int3` 的真正对应物,干净、无 oops。

`ikaslr_fixup_addr()` (`randomize.c:441`) 把陷阱地址换算成"哪个退役变体的第几个
函数的第几字节",再映射到当前 LIVE 变体的同一函数同一偏移,改写 PC,执行流继续。

### 3.6 白名单(`whitelist.c`)

条目有两个来源:手写的 `IKASLR_WHITELIST(fn)`,以及 **pass 为每个被包成 fixed_out
的目标自动发出的条目**。链接器汇总到 `.data..ikaslr_whitelist`,初始化时装入哈希表
(常数时间查询)并跨模块去重,随后把该段置只读。`ikaslr_out_enter` 在放行前查询。

实测(S1 = 31 个 VFS 函数):88 个出站调用点、47 个不同目标,白名单段 54 条(含跨模块
重复),去重后 47 条 —— **覆盖率 47/47**。压力测试中 `whitelist_rejects` 恒为 0。

**当前只告警不阻断**(`core.c:190` 附近有注释)。改成阻断需要先确认间接调用路径
的处理(那属于第 5 章 PA CFI),现在贸然阻断会误杀。

### 3.7 以代码地址为键的旁表

内核里有若干旁表在**链接期**把"某条指令的地址"编码进表项,而表项本身位于固定地址
的只读数据段。函数体一搬走,键就对不上了。处理方式分两类:

#### (a) 只查表的:换算查表用的地址,表一个字节都不动

| 表 | 钩在哪 | 后果(不处理的话) |
|---|---|---|
| `__ex_table` | `kernel/extable.c` `search_exception_tables()` 入口 | 本该被修正的异常变成 oops —— **正确性错误** |
| `__bug_table` | `lib/bug.c` `report_bug()` | `BUG()`/`WARN()` 变成"不明陷阱",真 oops |

```
查表前  ikaslr_live_to_image()   变体内地址 -> 映像链接期地址
查到后  ikaslr_image_to_live()   表给出的地址 -> 当前变体内地址
```

第二步只有 `__ex_table` 需要(fixup 地址要写回 `regs->ip`),做在各架构的
`fixup_exception()` 里。

> **为什么不去改表**:`__ex_table` 是**按地址排序、用二分查找**的
> (`search_kernel_exception_table` → `bsearch`)。函数搬走之后排序就乱了,每轮
> 随机化都重排一遍几千条表项,代价完全无法接受。换算地址则是 O(函数数) 的线性
> 扫描,而且只在"地址确实落在当前变体里"时才发生 —— 普通异常只多两条比较。
>
> **为什么换算下沉到 `search_exception_tables()` 而不是只放在 `fixup_exception()`**:
> 还有若干处只判"有没有表项"的调用方 —— x86/arm64 缺页路径里"内核代码访问用户
> 内存"的检查、kprobes 的可探测性判断。放在入口一处覆盖全部。

#### (b) 要写回代码的:静态键

`__jump_table` 的开关是**运行期改写代码**。做法分两步
(`kernel/ikaslr/randomize.c` `ikaslr_patch_code()`):

**第一步:打映像母本 + 所有 READY 变体。** 这两类副本都没有在执行,直接改写即安全。
母本非打不可 —— `ikaslr_prepare()` **始终从母本复制**(以 live 为源会与退役填陷阱
的工作项竞态),不打母本的话,下一轮随机化新变体从母本复制,这次开关就**悄无声息
地退回旧状态,而且不报任何错**。

这一步要与随机化切换互斥(复用 `ikaslr_in_progress`),否则某个 READY 变体可能正好
在被改写的同时被提升为 LIVE,那就变成了对正在执行的代码做非原子改写。

**第二步:当前 LIVE 那一份不去改它,而是换掉它** —— 触发一次随机化,切到一个
第一步已经打过补丁的 READY 变体上。

为什么不直接改 LIVE,两条路都走不通:

1. **x86 的 `text_poke_bp()` 根本无法寻址变体**。它把待改写地址存成**相对 `_stext`
   的 s32 偏移**(`struct text_poke_loc.rel_addr`),而变体分配在 vmalloc 区、与
   `_stext` 相距几十 TB,偏移直接溢出。实测 `text_poke_bp_batch()` 在
   `movslq (%r12,%rbx),%rax` 之后拿到一个截断的地址并缺页。
2. **退而求其次的"阻断入口 + 等区域变空再直接改写"在大范围下也不行**。实测 S3 的
   483 个函数覆盖 VFS/mm/net,等不到空;而等待期间整个区域被阻断,系统反而卡住
   (启动直接停在这一步)。

换一份则同时绕开这两点:新变体是**从未被执行过的新内存**,既没有交叉改写代码的
一致性问题(不需要广播 `sync_core`),也不需要第二套静默期实现 —— 复用的就是已经
充分验证过的切换路径。切换失败时如实告警并说明"下一次随机化会带上这次改动",
不假装成功。

随机化区域内的条目也**不进 `text_poke_queue` 批处理队列** —— 排队与
`text_poke_finish()` 之间隔着不可控的时间,期间可能发生若干轮随机化,队列里记的
地址早已失效。

**静态键为什么与本方案相容**:JMP 的偏移是"目标标号 − 本指令",两者同属一个函数、
随函数整体搬移,差值不变。

#### (c) 启动时一次性施加的:不用管

`.altinstructions`、`.smp_locks` 在启动时打在映像母本上,而变体**始终从母本复制**,
因此变体天然带着已打好的补丁。

#### (d) 本质不相容的:必须排除

`.static_call_sites` 写回代码的是指向**区域外**的 `call rel32`,偏移随函数位置而变
—— 与"函数体内不得有指向区域外的 PC 相对引用"这条不变式直接冲突,**无解**。
含这类条目的函数不能进入随机化范围(`gen_funcs.py` 的硬约束)。

#### (e) 能编出来、但功能会静默丢失的:`__mcount_loc`(ftrace)与 kprobes

> **这一条起初判错了**,写成了"ftrace 与位置无关性本质冲突"。实测更正:

`CONFIG_DYNAMIC_FTRACE` 在**构建期**就把 `call __fentry__` 换成了 5 字节 NOP
(实测 `dput_body` 入口是 `endbr64` + `0f 1f 44 00 00`),因此静态映像里没有指向
区域外的 PC 相对调用,**函数体照样可搬移** —— 开着 `FUNCTION_TRACER` 编 S3 范围,
`verify_movable.py` 依然报"全部可搬移"。

冲突发生在**运行期启用 tracer 的那一刻**:ftrace 按 `__mcount_loc` 记录的**映像
地址**打补丁(S3 下有 477 条指向随机化区域),而执行发生在变体里,于是补丁打在
一份首次随机化后就被置 NX、再也不会执行的代码上 —— **静默失效**。

kprobes 是同一失效模式,而且已经实测(`selftest.c` `test_kprobes_coexist`):

| 探什么 | 注册 | 随机化前命中 | 随机化后命中 | 结论 |
|---|---|---:|---:|---|
| 跳板 `ikaslr_st_add` | 成功 | 1 | 1 | ✅ 正常,跳板地址固定 |
| 函数体 `ikaslr_st_add_body` | **成功** | **0** | **0** | ❌ **静默失效** |

**静默失效比报错更糟**:使用者以为探针生效了。详见 `实验/E3-C-与既有机制共存.md`。

#### 对可随机化范围的影响(全树实测)

| 口径 | 排除函数数 | 占比 | 构成 |
|---|---:|---:|---|
| 不处理旁表 | 10083 / 60554 | 16.7% | per-CPU 5806、`__bug_table` 1702、`.smp_locks` 1609、`__jump_table` 641、`.altinstructions` 193、`__ex_table` 132 |
| 处理旁表后 | 5814 / 60554 | **9.6%** | **per-CPU 5814,别无其他** |

也就是说:处理完旁表之后,**一个内核函数不能被随机化的理由只剩 per-CPU 访问这一条**
(`%gs:` / `tpidr` 形式要求偏移是常量,不可回避)。

---

## 4. 强制构建配置

这几项**四档配置一律相同**,它们是"代码可搬移"的前提,不是可选项
(`scripts/ikaslr/perf/env.sh` `ikaslr_mandatory_config`):

| 架构 | 配置 | 为什么必须 |
|---|---|---|
| x86-64 | `RETPOLINE=n` `RETHUNK=n` `CPU_UNRET_ENTRY=n` `CALL_DEPTH_TRACKING=n` | retpoline 把间接调用换成对 **PC 相对 thunk** 的调用,函数体一搬就跳错 |
| x86-64 | `UNWINDER_ORC=n` `UNWINDER_FRAME_POINTER=y` | ORC 表**按地址索引**,代码搬走后查不到条目(见 §9.2) |
| arm64 | `ARM64_BTI=n` `ARM64_BTI_KERNEL=n` | 间接分支的落点需要 BTI landing pad;物化绝对地址的间接调用没有,直接 `Oops - BTI` |

`CONFIG_IKASLR` / `IKASLR_STATS` / `IKASLR_DEBUG` / `IKASLR_XOM_EPT`(x86,第 4 章) /
`IKASLR_PACFI`(arm64,第 5 章)见 `kernel/ikaslr/Kconfig`。

---

## 5. 对外接口

```
/proc/ikaslr/stats    0444  统计:轮次、三阶段耗时、活跃计数峰值、白名单拒绝数…
/proc/ikaslr/trigger  0200  写入触发一次随机化
/proc/ikaslr/layout   0400  当前变体的函数排布
```

**安全说明**:此前用于评测的漏洞模块(`evalvuln.c`,经 `/proc/ikaslr/attack` 提供
任意内核读写)已按作者要求**永久删除**(commit `aa5624f080f5`)。检测机制现在**只靠
内核内自检**(`selftest.c`)验证,不存在用户态攻击程序,也不再做真实 CVE 攻击链实验。

---

## 6. 已经验证到什么程度

### 6.1 启动与自检

| 配置 | 范围 | 结果 |
|---|---|---|
| x86-64,QEMU+KVM | S1 = 31 函数 | 10/10 干净启动(0 oops/warn) |
| x86-64,QEMU+KVM,并发压力 | S1 | 8/8 通过:每轮 250–390 次随机化在 4 个并发 VFS worker 之下完成 |
| x86-64,QEMU+KVM,DEBUG 全自测 | S1 + randfuncs | 5/5 通过,含 `sidetab/ex`、`sidetab/jump` |
| arm64,QEMU TCG,DEBUG 全自测 | S1 = 19 函数 | 通过:白名单 73 条 / 0 拒绝,`sidetab` 换算往返 PASS,SMOKE PASS |

**并发压力测试**(`tools/ikaslr/init_stress.c`)是这一轮新加的,判据全部取自内核统计:

| 指标 | 要求 | 实测 |
|---|---|---|
| `whitelist_rejects` | 必须 0 | 0 |
| `stale_fixup_fails` | 必须 0 | 0 |
| `stale_fixups` | >0 才说明兜底路径真被走到 | 4–16 |
| `enter_backoffs` | >0 才说明争用是真的 | 1400–1550 |
| `rounds` 增量 | 显著 | +250 ~ +390 |

> 为什么必须并发:单线程冒烟测试里,触发随机化时区域本来就是空的,fixed_out 的
> 记账与陈旧返回地址的陷阱/修正路径**根本走不到**。只有在"一批执行流正在随机化
> 过的 VFS 函数里、并且有些已经跳到区域外"的时候切换代码,才真的在考验方案 B。
>
> 更正一条我先前说错的话:我曾报告"约 30% 的启动会失败"。那是引用了交接过来的
> **陈旧 `.output` 文件**,没有实测。实测后树是干净的。这条已作废。

### 6.1b 阻断协议的嵌套自我阻塞(已修复,过程值得记)

同一套并发压力测试,把范围从 S1(31 函数)换到 S3(483 函数)时,随机化成功率从
~70% 掉到 **0–3%**。我第一次给出的解释是"范围一大、负载一重就等不到空",
**那个解释是错的**。作者反问了一句:"随机化开始时 `fixed_in` 会阻止执行流进入,
区域只出不进,活跃集合怎么不会及时变空?" —— 这一问点破了。

加诊断后数据立刻否掉了"负载太重":

| 指标 | 值 | 说明 |
|---|---:|---|
| `wait_residual`(超时那刻的残余计数) | **3** | 区域确实从 16 排到了 3,然后卡住不动 |
| `outleave_blocked` | 712 | `out_leave` 绕过阻断加回计数只有几百次,不是洪水 |
| `enter_backoffs` | **8 439 132** | 但有 840 万次回退重试 |

"只剩 3 个不走"+"840 万次重试"放在一起只有一个解释:**嵌套调用的自我阻塞**。

一个执行流在被随机化的函数 A 里(持有 1 个 `active` 计数),A 又调用被随机化的
函数 B → B 的 `fixed_in` 看到阻断标志,把它挡住。于是:

- 它被挡在 B 的跳板里,而**仍然持有 A 的那一个计数**;
- 随机化方要等 `active == 0` 才放行,这个计数永远不减;
- 两边互相等待,必然走到超时。840 万次回退就是这几个执行流在重试循环里打转。

S3 有 **351 处"随机化函数调用随机化函数"**,嵌套是常态;S1 只有 31 个函数、
嵌套稀少 —— 这正好解释了 70% vs 0–3% 的差距,而与负载轻重无关。

**修法**:按任务记一个 `current->ikaslr_depth`(与 `active` 严格同步增减),
`ikaslr_enter()` 只在 `depth == 1`(本任务**新**进入区域)时才受阻断约束。
深度大于一说明本任务已经在区域内、随机化方无论如何都得等它出来,再挡一次
只有坏处没有好处。安全性不变:切换仍然只在 `active == 0` 时发生。

`task_struct` 是 fork 时整份拷贝的,因此必须在 `copy_process()` 里清零 ——
否则子任务一出生就带着非零深度,它的 `fixed_in` 将永不受阻断约束,而
`ikaslr_leave` 又会把深度减成负数。

**效果**:

| | 修复前 | 修复后 |
|---|---:|---:|
| S3 成功轮数 | 3 / 200(1%) | **200 / 200(100%)** |
| `wait_residual` | 3 | **0** |
| `nested_admitted` | — | 61 280(旧代码会自我阻塞的次数) |

`/proc/ikaslr/stats` 新增 `nested_admitted` / `outleave_blocked` /
`wait_residual` / `enter_forced` 四项,就是为了让这类问题下次能被直接看见
而不是靠猜。

> 教训记在这里:我当时把一个可以被"为什么"问倒的解释当成了结论。
> "负载太重"是个听起来合理、但没有任何一个数支持的说法;真正的证据
> (残余计数只有 3)一测就出来了。

### 6.2 旁表的针对性自测(`selftest.c` `test_sidetables`)

压力测试只能碰运气命中,旁表必须有针对性的验证。`randfuncs.c` 里为此写了两个
随机化函数,各自在**函数体内部**留下一条旁表条目:

| 子测试 | 做什么 | 通过意味着 |
|---|---|---|
| A 换算往返 | 每个函数体中点做 `image→live→image` | 换算双向一致;区域外地址两边都返回 0(不误伤普通异常) |
| B `__ex_table` | `ikaslr_rf_nofault()` 取一个 vmalloc 保护页,重复 3 轮随机化 | 搬移后异常仍被修正成 `-EFAULT` 而不是 oops |
| C `__jump_table` | 开启静态键 → 随机化 3 轮 → 每轮都必须仍然是"开" | 开关不仅当轮生效,**还跨轮保持**(即母本被打上了) |

C 的第 3 步是要害:新变体从**映像母本**复制,若运行期的静态键改写只打到当前变体,
下一轮就会把开关悄悄退回旧状态,而且不报任何错。

> 写这个测试时踩到的坑,值得记一笔:B 一开始用地址 0 作为"必然出错的地址",结果
> 直接 oops。原因是 0 是**用户地址**——开了 SMAP 的机器上,内核态访问用户地址且
> AC=0 时,`do_user_addr_fault()` 会直接 `page_fault_oops()`,**根本不查异常表**
> (`arch/x86/mm/fault.c` 里那段的注释就写着 "get_kernel_nofault() will not get
> here")。改用 vmalloc 分配之后的保护页才是可移植的"合法内核地址但一定不可读"。

### 6.3 可搬移性

`scripts/ikaslr/verify_movable.py` 反汇编 `[__rand_text_start, __rand_text_end)`,
判据是"函数体内不得有指向区域外的 PC 相对引用":

- x86-64:任何 `(%rip)`;任何目标落在区域外的 `call/jmp <绝对地址>`
- arm64:任何 `adrp`;任何目标落在区域外的 `bl/b <绝对地址>`
  (`ldr xN, <区域内地址>` 是随函数搬移的字面量池,合法)

**两个架构上都是 0 条。**

### 6.4 名单是"编译—扫描—剔除—重编"迭代出来的

`gen_funcs.py` 按重定位做静态启发式初筛,但**必然有漏网**(per-CPU、内联汇编里的
全局内存操作数、后端自生成的调用)。正确做法是以 `verify_movable.py` 为准迭代:

```
编一次 → 扫出不合格函数 → 从名单剔除 → 重编 → 直到为 0
```

`02-build-kernels.sh` 已把这一步接进构建流程,并另外收集 pass 主动跳过的函数
(变参/naked)到 `$O/ikaslr-skipped.txt`。

**不可随机化的代码占 9.6%,且全部来自 per-CPU 访问**(全树实测,见 §3.7 末尾的表)。
这不是缺陷 —— 方法本身就允许不可随机化的函数存在,它们的存在恰好论证了对非随机
部分施加额外防护的必要性。

---

## 7. 已实现 vs 设计要求但未实现

这一节是本文档最该被认真读的部分。

### 7.1 ✅ `fixed_out` 与白名单(2026-09-10 完成)

pass 现在把 ②③ 两类出站调用改写成 `out_enter → 调用 → out_leave`,并为每个目标
自动发出白名单条目。见 §1.4、§3.6。实测覆盖率 47/47,`whitelist_rejects` 恒为 0。

**仍然不覆盖间接调用** —— 静态白名单对运行期目标无能为力,那属于第 5 章 PA CFI。

### 7.2 ✅ 地址键旁表(2026-09-10 完成)

`__ex_table`、`__bug_table`、`__jump_table` 已纳入处理,`.altinstructions` /
`.smp_locks` 由"变体从母本复制"天然覆盖。见 §3.7。作用是把不可随机化的比例从
16.7% 降到 9.6%,且残余全是 per-CPU。

### 7.3 ⏸️ ORC 表重定位(明确不做)

ORC 展开表按地址索引,代码搬走后查不到条目。**当前用 frame-pointer unwinder 绕过**,
四档配置一律相同,因此不影响性能对比的公平性,也**不影响运行时正确性**——只影响
栈回溯的可读性。作者 2026-09-10 决定不做。

### 7.4 ⏸️ arm64 共享 GOT(明确不做)

当前是**每函数一个字面量池**,同一符号在多个函数体里各存一份。这只是空间开销
(见 `12-got-codemodel-results.md`),不影响正确性。作者 2026-09-10 决定不做。

### 7.5 ❌ 本质不相容、只能排除的

`.static_call_sites` 与 `__mcount_loc`(ftrace):写回代码的是指向区域外的
`call rel32`,与位置无关性直接冲突。见 §3.7(d)。

### 7.6 ⚠️ 白名单只告警不阻断

见 §3.6。

### 7.7 顺手修掉的两个上游问题

- **modpost 段错误**:`.rand.text` / `.tramp.text` 不在 modpost 的授权文本段列表里,
  于是函数体内的 `__ex_table` 条目被判成"异常表指向非文本段";而报这条错误的
  `default_mismatch_handler()` 在 `__ex_table` 里找不到 `fromsym` 时会**解引用空指针**,
  modpost 直接段错误、什么都不打印。已把两个段加进 `OTHER_TEXT_SECTIONS`,并把
  `struct sectioncheck` 的三个数组从 20 扩到 24 —— 不扩的话 gcc 只报
  "excess elements in array initializer" 警告,随后 `good_tosec` 少了结尾的 NULL,
  `match()` 越界读野指针。
- **`text_poke_bp()` 无法寻址 vmalloc 区**:见 §3.7(b)。

### 7.8 ✅ 阻断协议的嵌套自我阻塞(2026-09-10 修复)

见 §6.1b。S3 成功率 1% → 100%。**此前这一节写的"这是方法在大范围下的固有活性
代价、需要作者在三条路之间取舍"是错的** —— 那是一个实现缺陷,不是方法的性质。

### 7.9 未定的实验前置

- E3-10 / R-67(ftrace/kprobes 共存):§3.7(d) 已回答一半 —— ftrace 插桩点与被
  随机化的函数体不能共存。kprobes 部分待定。
- 待做实验:E3 四档端到端、E3-1 实测验证、E3-4 无拷贝、E3-6 延迟触发。
- ARM 真机实验保留,待上机。

---

## 8. 自己复现验证

```bash
# 1. 编插件
make -C tools/ikaslr/llvm

# 2. 编内核(必须 clang;CRR_TRAMPOLINE=y 才真的随机化)
cd scripts/ikaslr/perf
IKASLR_FUNCS=$PWD/../funcs/s3-full.txt ./02-build-kernels.sh R

# 3. 验可搬移性(应输出 "全部可搬移")
python3 scripts/ikaslr/verify_movable.py <build>/vmlinux --arch x86_64

# 4. 看调用分类学(§1.3 那张表)
objdump -d --start-address=$(nm vmlinux|awk '/__rand_text_start/{print "0x"$1}') \
           --stop-address=$(nm vmlinux|awk '/__rand_text_end/{print "0x"$1}') vmlinux

# 5. 启动后
cat /proc/ikaslr/stats
echo 1 > /proc/ikaslr/trigger
```

若 `02-build-kernels.sh` 警告"被随机化函数 ≤3",说明 `CRR_TRAMPOLINE` 没生效或
名单没读到 —— 这时四档等价,测出来的"开销"没有意义。

---

## 9. 相关文档

| 文档 | 内容 | 状态 |
|---|---|---|
| `16-position-independence.md` | 位置无关化的完整推导与两架构方案 | 最新 |
| `15-scope-selection.md` | S1/S2/S3 范围怎么选、为什么 | 最新 |
| `14-code-model-and-unmovable.md` | mcmodel 设计与不可随机化代码统计 | 最新 |
| `12-got-codemodel-results.md` | GOT 定容实验(R-31) | 最新 |
| `11-related-comparison.md` | 与 Adelie / dbox 等的对比 | 最新 |
| `实验/` | 一实验一文档 | 见 `实验/README.md` |
| `07-compiler.md` | 编译期 | **部分过时,以本文 §1 为准** |
| `03-randomization.md` | 运行时 | **缺 §3.5.5,以本文 §3 为准** |
