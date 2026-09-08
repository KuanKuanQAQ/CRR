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

## S1.2–S1.10（待实现）

见 `PROGRESS.md` Phase 1。核心接口已在 `ikaslr.h` 中以桩形式给出：
`ikaslr_enter/leave`（S1.3 线程追踪）、`ikaslr_rerandomize`（S1.4/S1.5 无副本 +
推迟随机化）。`struct ikaslr_func` 将随 S1.2 补入 `target`（唯一需更新的索引）、
随 S1.8 补入白名单关联字段。
