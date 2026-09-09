# 与相关内核持续随机化工作的对比（E8，§6.6）

本文档汇总从原始论文核对到的对比数据、公平对比的方法，以及配套脚本
（`scripts/ikaslr/perf/07..10`）如何产出 E8/E9/E10/E12 的数据。数字均标注来源，
供正文第 6.6 节引用。

## 1. 对比对象与关键事实（核对自原文）

| 系统 | 会场/年 | 保护范围 | 粒度 | 触发策略 | 索引/指针处理 | 报告开销 | 评测负载 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **Adelie** | ASPLOS'22 | 内核**模块/驱动**：主评 E1000E(网络)+NVMe(存储)，另测 E1000、ENA、xHCI、FUSE、ext4 | 模块 | 专用内核线程**固定 20ms**（另测 5ms/1ms） | 模块编 PIC + 每模块**只读 GOT**；`schedule_on_each_cpu()`+RCU 宽限期暂停执行流；栈重随机化+地址加密 | **重随机化 < 2%**；随机化线程 CPU 占用 **0.4%@20ms**；网络吞吐至 110MB/s 无差异 | Sysbench(fileio、OLTP/mySQL)、Kernbench、ApacheBench(512B–8KB)、NVMe O_DIRECT 读微基准、IOCTL |
| **Dbox** | CCS'24 | **不可信驱动**（无源码） | 模块/驱动 | 私有空间随驱动执行**滑动**（sliding space）；所依赖的核心内核/LKM 空间也动态滑动 | 轻量 hypervisor 监控；到/离驱动的**所有控制流可检测** | **< 3.6%**（一般场景） | 见原文（含系统级基准；具体项待正文按原文补） |
| **Remix** | CODASPY'16 | 用户进程 + **内核模块** | **基本块**（函数内洗牌，原地） | 按需/短周期 | 原地打乱基本块 | 低（CPU+I/O 基准，短间隔仍低） | CPU/I/O 基准、内核模块 |
| **Shuffler** | OSDI'16 | 用户态 | 函数 | 固定 50ms | 用户态跳板表 | SPEC 均 **14.9%**、最坏 45% | SPEC |
| **CodeArmor** | — | 用户态 | 代码空间虚拟化 | 可低至 55μs | 地址空间虚拟化 | SPEC 均 **3.2%**、最坏 55% | SPEC |
| **MARDU** | — | 用户态 | — | 按需 | 单份代码+trampoline | SPEC 均 **5.5%**、最坏 18.3% | SPEC |
| **I-KASLR（本文）** | — | **built-in 内核代码**（核心子系统，非模块） | **函数**（可迁移体 `.rand.text` + 固定跳板 `.tramp.text`） | 第4章按需触发；+R 档用固定间隔（对齐 Adelie 取 20ms） | **每函数一个 target 槽**，一次随机化更新 O(1) 个索引；跳板进出计数精确追踪执行流（无副本、覆盖非抢占上下文） | 见 E3/E8 实测 | LMBench、UnixBench、fio、netperf、7z/openssl/sysbench（SPEC 替代）、见 10-perf-plan |

> **W-21（第2章表 2-3 的 Dbox 行）现可补齐**：粒度=模块/驱动；触发=滑动空间随执行；
> 索引更新=轻量 hypervisor + 空间滑动；开销 < 3.6%。**请对照原文再核一遍**用词后填入正文。

## 2. 为什么不能直接比，以及怎么比才公平（R-63/R-64）

**范围不同：Adelie/Dbox/Remix 保护的是模块/驱动，本文保护的是 built-in 核心代码。**
built-in 范围更大、热路径更核心，开销天然更高——这个不公平的方向**对本文不利但对论点有利**
（能保护别人原理上保护不了的 built-in 代码，见第1章 21.6% 的 built-in-only CVE）。因此正文须：

1. **同范围对照组**：把本文随机化范围（`funcs.txt`）限到与 Adelie 同类的**驱动/热路径等价的 built-in 函数**
   （网络栈 + 块/NVMe 路径），跑**同类负载、同 20ms 间隔**，得到一组"同范围"数字——这组可与
   Adelie 的 <2% 直接对照。脚本：先按该名单 `CRR_TRAMPOLINE=y` 编一档（记为 `scoped`），
   启动后跑 `10-compare-adelie-dbox.sh`。
2. **built-in 全范围**：另给本文在更大 built-in 范围上的开销（本文真正目标），主动说明范围更大。
3. **同类负载**：`10-compare-adelie-dbox.sh` 已对齐 Adelie 的负载结构——sysbench fileio(rnd/seq 读,
   cached)、O_DIRECT 读吞吐、sysbench OLTP/mySQL、ApacheBench/wrk(512B–8KB)、Kernbench，
   触发间隔固定 20ms。
4. **引用其数字，不同机复现**：Adelie/Dbox 的开源实现难在本 ARM/x86 平台复现，故**引用其论文开销数字**，
   并在正文明确"实验环境不同、仅作数量级参照"（R-64）。Adelie 硬件：Xeon Silver 4114 2.20GHz/96GB、
   Intel E1000E 1GbE、Samsung 970 EVO NVMe、Ubuntu 18.04/GCC 8.4/Linux 5.0.4——本文若在正文并列，
   须注明平台差异。

## 3. 脚本 → 实验项对应

| 实验 | 脚本 | 说明 |
| --- | --- | --- |
| **E8** 同类负载对比 | `10-compare-adelie-dbox.sh` | Adelie 同类负载 @20ms；scoped 档与 built-in 档各跑一遍 |
| **E9** 频率敏感性（§6.7.1） | `07-freq-sweep.sh` | 固定负载扫触发频率，出敏感性曲线 |
| **E10** 范围敏感性（§6.7.2） | `08-range-sweep.sh` | 按 `funcs.txt` 规模各编一档，逐档测函数数/随机化耗时/端到端开销 |
| **E12** 开销归因（§6.9） | `09-attribution.sh` | perf 采样拆分：跳板/计数/白名单/检测/PA/XOM |

## 4. 来源

- Adelie（ASPLOS'22）原文：<https://rusnikola.github.io/files/adelie-asplos22.pdf> ，
  arXiv：<https://arxiv.org/abs/2201.08378> 。核到：范围=E1000E+NVMe 等驱动模块；20/5/1ms；
  重随机化 <2%；线程 CPU 0.4%@20ms；负载 Sysbench/Kernbench/ApacheBench/mySQL。
- Dbox（CCS'24，Y. Li 等）：<https://dl.acm.org/doi/10.1145/3658644.3670269> 。
  核到：不可信驱动；滑动空间；控制流到/离驱动可检测；开销 <3.6%。
- Remix（CODASPY'16）：<https://www.cs.fsu.edu/~whalley/papers/codaspy16.pdf> 。基本块洗牌，用户+内核模块。
- Shuffler（OSDI'16）：<https://www.cs.columbia.edu/~junfeng/papers/shuffler-osdi16.pdf> 。
- MARDU：<https://arxiv.org/pdf/1909.09294> 。
（SPEC 数字转引自上述各文/综述，正文引用时以原文表格为准。）
