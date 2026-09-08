# 第 3 章实现细节：持续随机化执行机制

> 对应论文第 3 章与 `PROGRESS.md` 的 Phase 1（S1.1–S1.10）。本文随实现推进增补。
> 设计依据见 `../设计文档/草稿/毕业论文_第3章_v2.md`。

## 代码落点

| 路径 | 内容 |
| --- | --- |
| `include/linux/ikaslr.h` | 公共接口：段注解宏、`struct ikaslr_func`、区域符号、API |
| `kernel/ikaslr/core.c` | 运行时核心：初始化、区域发现、（后续）线程追踪与重随机化 |
| `kernel/ikaslr/Kconfig` | `CONFIG_IKASLR` / `CONFIG_IKASLR_DEBUG` |
| `include/asm-generic/vmlinux.lds.h` | 段与指针表定义（`TRAMP_TEXT`/`RAND_TEXT`/`*_TABLE_DATA`） |
| `arch/x86/kernel/vmlinux.lds.S` | x86 上安置上述段（arm64 待 Phase 2/3 wiring） |

## S1.1 子系统骨架与区域发现（已完成）

### 段布局

复用既有链接基础设施（原为 `CONFIG_CKASLR`，现扩展为 `CKASLR || IKASLR` 同时触发）：

```
.tramp.text.<fn>       固定地址跳板，随机化时不迁移
.rand.text.<fn>        随机化函数体，可迁移
.data..tramp_ptr_tbl   每函数一项，&跳板，按链接顺序
.data..rand_ptr_tbl    每函数一项，&函数体，按链接顺序
```

边界符号：`__tramp_text_start/end`、`__rand_text_start/end`、
`__start/__end_{tramp,rand}_ptr_tbl`。

> **对齐约束（与第 4 章耦合，论文 §4.5.2）**：ARM 观察点掩码要求随机化区域 +
> 跳板区域连续且按 2 的幂自然对齐。当前 x86 布局尚未施加此对齐；arm64 布局在
> Phase 2 wiring 时统一满足。记为待办。

### 区域发现

`ikaslr_init()`（`late_initcall`）打印区域边界；若 `.rand.text` 为空（尚无注解
函数、或 LLVM pass 未接入）则直接返回。否则 `ikaslr_build_funcs()` 同步遍历两张
指针表，构建 `struct ikaslr_func[]`：

- 两表项数必须相等（每个随机化函数各一 tramp + 一 body），不等即报错——用于捕获
  缺失配对的注解；
- 函数体大小由相邻 body 指针之差得出，末项以 `__rand_text_end` 定界。

此步只做"发现"：数组描述了哪些函数可被迁移及其大小，尚未涉及移动或跳板改写。

### 验证

- `CONFIG_IKASLR=y` 全量构建并链接通过（x86-64，QEMU 目标）；区域符号、
  `ikaslr_init`/`ikaslr_nr_funcs`/`ikaslr_rerandomize` 均正确入链。
- 当前无注解函数，故 `.rand.text` 为空，`ikaslr_init` 打印空区域并返回——
  E2 扫描器（`scripts/ikaslr/scan_xregion.py`）在该映像上确认区域为空。
- `CONFIG_IKASLR=n` 由构造保证可编译：链接守卫退回到仅 `CKASLR`（即 `main` 的
  既有状态），`kernel/ikaslr/` 不参与构建。

## S1.2 跳板运行时与 target 槽（已完成）

### 一个随机化函数的源码形态

```c
IKASLR_RAND_FN(int, foo, int a)      /* 函数体 -> .rand.text.foo，可迁移 */
{ ... }

IKASLR_TRAMP_FN(int, foo, int a)     /* fixed_in 跳板 -> .tramp.text.foo，固定 */
{
        int ret;
        ikaslr_enter();                      /* 进入随机化区域 */
        ret = IKASLR_TARGET(foo)(a);         /* 经 target 槽间接转移 */
        ikaslr_leave();                      /* 离开 */
        return ret;
}
```

跳板**保留原函数名**，因此所有既有调用者一行都不用改；函数体改名为 `<fn>_body`。
`IKASLR_TRAMP_FN` 同时发出该函数的 target 槽（`.data..ikaslr_target`）与表项。

### 为什么这就是 O(1)（需求 D1）

调用者始终调用**固定地址**的跳板，跳板经 target 槽间接转移到函数体。因此函数体
搬到哪里，只影响**一个** target 槽，与该函数有多少调用点无关 —— 这正是论文
§3.4.2 所说"随机化时需要更新的，就只有这一个 target_addr"。

### 表的组织：为什么存指针而不是结构体

`.data..ikaslr_tramp_tbl` 中存放的是 `struct ikaslr_tramp *`（8 字节）而非结构体
本身。**这是踩过坑之后的选择**：x86-64 把 ≥32 字节的数据对象按 32 字节对齐，而
该结构 40 字节，直接排布会在表项之间留下 24 字节空洞——按 `sizeof` 索引就会落进
填充区读到垃圾（实测两项相距 64 字节而非 40）。改存 8 字节指针即天然紧密排列，
这也是内核既有表（`__start___tracepoints_ptrs` 等）的通行做法。
`core.c` 另有一处跨度自检，若表项不紧密即在初始化时报错而非静默出错。

### 初始化

`ikaslr_build_table()` 按函数体地址**排序**表项（表项的链接顺序未必等于代码的
链接顺序），用相邻项之差得出每个函数体大小（末项以 `__rand_text_end` 定界），
并把每个 target 槽初始化为函数体的链接期地址。

### 自测（`kernel/ikaslr/selftest.c`，仅 CONFIG_IKASLR_DEBUG）

两个自足的被随机化函数，使 S1.2 之后每一步都能端到端验证。QEMU 实测：

```
ikaslr: regions: .rand.text=[...,...) 33 B, .tramp.text=[...,...) 94 B
ikaslr:   [0] ikaslr_st_add   tramp=... body=... size=16 target=...
ikaslr:   [1] ikaslr_st_mul   tramp=... body=... size=17 target=...
ikaslr: registered 2 randomizable function(s)
ikaslr/selftest: add(40,2)=42 mul(6,7)=42 nr_funcs=2
ikaslr/selftest: PASS: calls reached the bodies through their trampolines
```

### 已知限制（影响 S1.4）

自测函数刻意写得**自足**（不调用外部函数、不引用全局变量）。原因是在 LLVM pass
就绪之前，`.rand.text` 中的函数体**并非位置无关代码**：它含有 PC 相对的外部调用与
全局变量引用，迁移后这些位移全部失效。早期原型 ktm 也卡在同一处
（`ktm_move.c` 中的 `/* error: 需要重定位移动后的函数代码段 */`）。
因此当前手工路径下可安全迁移的，只有自足函数这一子集；一般函数的迁移需要
S1.4 的重定位处理或 S1.10 的 LLVM pass。这是**当前实现与论文设计之间一处真实的
差距**，不应在论文中含糊带过。

## S1.3 精确线程追踪（已完成）

### 机制

活跃执行流集合以一个原子计数实现：经 `fixed_in` 进入时加一，跳板返回前减一。
**这个信息是免费的**——跳板本来就必须被经过（论文 §3.4.5）。计数为零即可确定
区域内既没有执行流在跑、也没有栈帧会返回到区域内，因此可以直接切换代码页，
不必像 Shuffler 那样保留旧副本（§3.2.2）。

### 竞态：为什么先加一再查标志

朴素写法"先查阻断标志、未阻断则加一"是**错的**：两步之间随机化线程可能置位
标志并读到计数为零，于是在本执行流已经进入的情况下开始搬移代码。

正确顺序是 **先加一 → 内存屏障 → 再查标志**；若发现已被阻断就减一退出、等待
放行后重试。这样随机化线程置位标志后读到的计数，必然已经把所有"先加一"的执行
流计入。对称地，随机化线程置位标志后也要有屏障再读计数。

```
ikaslr_enter():                    randomizer:
    atomic_inc(active)                 WRITE_ONCE(blocked, true)
    smp_mb()                           smp_mb()
    if (blocked) { dec; wait; retry }  wait until active == 0
```

### 非抢占上下文

`ikaslr_enter()` 可能在中断、持锁等非抢占上下文被调用，因此等待分两条路径：
可抢占时 `wait_event()` 睡眠，否则 `cpu_relax()` 自旋。自旋会把随机化耗时转成
中断延迟——这正是 §3.4.6 推迟随机化（S1.5）要解决的问题。

### 超时

`ikaslr_wait_region_empty()` 带超时。若某执行流长时间停留在区域内（例如阻塞在
I/O 上），随机化应当**放弃本次**而不是无限期挂住：放弃只损失一次随机化机会，
挂住会拖垮系统。

### 验证（QEMU 实测）

```
ikaslr/selftest: dispatch: add(40,2)=42 mul(6,7)=42 nr_funcs=2
ikaslr/selftest: tracking: active before=0 after=0
ikaslr/selftest: block: wait_empty ok; enters 4->5 backoffs=0 max_active=1
ikaslr/selftest: PASS: dispatch + thread tracking + block protocol
```

## S1.4 无副本随机化（已完成）

`kernel/ikaslr/randomize.c`。一次随机化的完整流程（论文 §3.4.7）：

| 步 | 动作 | 说明 |
| --- | --- | --- |
| 1 | 检查互斥标志 | `atomic_cmpxchg`，随机化不可嵌套（§3.4.6 策略一） |
| 2 | 生成新布局 | Fisher-Yates 打乱函数顺序 + 函数间随机间隙（≤256 B） |
| 3 | 分配新代码页并复制 | 此时**尚未阻断**，不影响正在执行的流；随后降为 RO+X |
| 4 | 阻断入口并等待清空 | `ikaslr_block_region()` + `ikaslr_wait_region_empty()` |
| 5 | **更新所有 target 槽** | **唯一的代码索引更新**，量 = 函数数，与调用点数量无关 |
| 6/7 | 交接并放行 | 切换 `ikaslr_cur_base`，`ikaslr_unblock_region()` |
| 8 | 使旧份不再可执行 | vmalloc 的直接 `vfree`；映像内的置 NX |

### 第 8 步：无副本性质的关键

**这一步容易被忽略而使"无副本"落空。** 旧份若在 vmalloc 中，`vfree` 即可
（`VM_FLUSH_RESET_PERMS` 会复位权限）；但**首次**迁移时旧份在内核映像的
`.rand.text` 里，无法回收——若不处理，映像中那份旧代码仍然可执行，攻击者
依旧能把它当作 gadget 来源，随机化形同虚设。

因此首次迁移后对映像内 `.rand.text` 置 NX。链接脚本已把该段按页对齐并独占整页
（前后各有 `ALIGN(PAGE_SIZE)`），故置 NX 不会波及其它代码。之后随机化区域内
**任一时刻只有一份可执行代码**，不存在 Shuffler 式的旧副本暴露窗口。

### 超时即放弃

若等待清空超时（默认 100 ms），本次随机化**放弃**并保持原状，而不是继续等待：
放弃只损失一次随机化机会，挂住会拖垮系统。

### 验证（QEMU 实测，两轮随机化）

```
ikaslr: retired in-image .rand.text (1 page(s), now NX)
ikaslr/selftest: rerand: ikaslr_st_add ffffffff81dab000 -> ffa0000000035170,
                         ikaslr_st_mul ffffffff81dab010 -> ffa0000000035100 (8732707 ns, round 1)
ikaslr/selftest: rerand: second round ok, now at ffa000000002d0b0 / ffa000000002d050
ikaslr/selftest: no-copy: all bodies left the image region
ikaslr/selftest: PASS: dispatch + tracking + block protocol + rerandomization
```

调用者与跳板**一行未改**，函数体两次搬到全新地址后调用结果依旧正确——这就是
需求 D1 的端到端体现。

> **关于耗时**：实测单轮 3.6–8.7 ms（仅 2 个极小函数）。这个数字**几乎全部来自
> `__vmalloc_node_range` 与 `set_memory_ro`**，而非索引更新（更新量只有 2 个槽）。
> 论文 §3.6.4 的时间分解应据此分列，不要让读者误以为代价来自索引更新。
> 真实规模下需要重测，并考虑预分配代码页池以摊掉分配开销。

### 当前限制

复制式迁移要求函数体**自足**（不含指向区域外的 PC 相对引用）。自测函数满足此
条件（`add` 无任何 PC 相对引用；`mul` 只有函数内部的相对跳转）。一般函数含有
对外部函数的调用与全局变量引用，搬移后会失效，需 S1.7 共享 GOT 与 S1.10 的
LLVM 函数级 PIC 消除。**这是当前实现与论文设计之间一处真实差距。**

## S1.5–S1.10（待实现）

见 `PROGRESS.md` Phase 1。
