# I-KASLR 实现总览

> 本文是实现细节文档的入口。设计依据见 `../设计文档/草稿/` 第 1–6 章；
> 进度见 `PROGRESS.md`。本系列文档描述**代码怎么实现的**，与论文的**为什么这么设计**互补。

## 系统组成（对应论文图 1-2）

I-KASLR = 编译期支持（LLVM 扩展）+ 三个运行时模块，经**跳板函数**耦合：

| 模块 | 论文章节 | 代码落点 | 平台 |
| --- | --- | --- | --- |
| 持续随机化执行（跳板、PIC、线程追踪、无副本随机化） | 第3章 | `kernel/ikaslr/` | 跨平台 |
| 信息泄露检测与按需触发（XOM、多源检测、控制流审计） | 第4章 | `kernel/ikaslr/detect/`、`arch/*/` | x86=EPT，ARM=观察点 |
| PA 精准 CFI（高扇入识别、分区验证） | 第5章 | `kernel/ikaslr/pacfi/`、`arch/arm64/` | 仅 ARM |
| LLVM pass（函数级PIC、跳板生成、重定向、PA插桩、静态验证） | 3.5/5.5 | `tools/ikaslr/llvm/` | 编译期 |
| 实验脚本 | 各章 6.x | `scripts/ikaslr/` | — |

## 与既有原型的关系

本实现在以下既有原型上一般化，而非从零开始：

| 既有原型 | 提供了什么 | 如何并入 |
| --- | --- | --- |
| `.rand.text`/`.tramp.text` 段 + `kernel/rerand.c` | 段布局、指针表发现 | 演化为 `kernel/ikaslr/` 的段与注册机制 |
| `samples/kernel_trampoline_move` (ktm) | 函数移动 + UDF 异常挂起 + 进出计数 | 抽出为随机化引擎的 arch 后端与线程追踪 |
| `samples/hw_breakpoint` (topic/hw-breakpoint) | ARM 硬件观察点驱动 | 第4章 ARM XOM 路径的基础 |
| `samples/log_time` | 计时插桩 | 实验计时工具 |

## 术语对照（论文 ↔ 代码）

| 论文术语 | 代码标识 |
| --- | --- |
| 随机化区域 / 非随机化区域 | `.rand.text` / 其余 `.text` |
| 跳板 fixed_in / fixed_out | `ikaslr_tramp_in_*` / `ikaslr_tramp_out_*` |
| target_addr（唯一需更新的索引） | `struct ikaslr_tramp.target` |
| 活跃执行流集合 | `ikaslr_active_set` |
| 推迟标志（非抢占上下文） | per-cpu `ikaslr_deferred` |
| 白名单 | `ikaslr_whitelist` |
| 被探测函数 | `struct ikaslr_probed_fn` |

（其余随实现补全。）
