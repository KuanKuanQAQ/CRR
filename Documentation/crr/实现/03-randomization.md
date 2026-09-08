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

## S1.3–S1.10（待实现）

见 `PROGRESS.md` Phase 1。`ikaslr_enter/leave` 目前为空桩（S1.3 补入活跃执行流
集合），`ikaslr_rerandomize` 为空桩（S1.4/S1.5）。
