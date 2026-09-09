# I-KASLR 性能开销完整测试方案（裸机部署）

面向作者在**两台真机**（x86-64 一台、arm64 一台）上**裸机**部署测试。方法：同一台机器、
同一 rootfs、同一套 benchmark，**只换内核 + 重启**做 A/B。配套脚本在 `scripts/ikaslr/perf/`。

> **为什么裸机（而非 QEMU）**：本仓库开发期的性能数字取自 QEMU（x86 EPT 走嵌套虚拟化，
> VM exit 开销被放大约 10 倍；arm64 走 TCG 纯模拟，绝对时间无意义）。**那些不是论文最终
> 值**，仅验证功能。裸机直接消掉嵌套虚拟化放大——`bench.c` 的周期计数在裸机上才是可写进
> 论文的真值。这是裸机方案的核心理由。

---

## 0. 先决条件（务必先做）

**编译器 pass 必须先应用到被测子系统。** 否则跳板/PAC 不在 benchmark 实际走的代码路径上
（syscall、VFS、网络栈…），测出来的开销不反映真实防护。步骤：

1. 用名单文件指定要随机化的函数（`IKASLR_FUNCS`），覆盖被测负载的热路径。起步集合建议：
   `fs/`（VFS 核心）、`net/core/`、目标网卡/存储驱动。先小后大。
2. 用 LLVM 或 GCC 插件编译内核（见 `07-compiler.md`）。
3. 编译后先跑 `scripts/ikaslr/scan_xregion.py vmlinux` 确认无绕过跳板的跨区域转移，再进入
   性能测试。

**范围（阈值）是自变量之一**（§6.7.2 / E10）：不同 `funcs.txt` 规模对应不同开销，须扫多个规模。

---

## 1. 基准软件：是什么、怎么获取

| 软件 | 授权 | 测什么 | 获取 |
| --- | --- | --- | --- |
| **LMBench** | 免费(GPL) | 系统调用/上下文切换/页错误/内存带宽延迟（微基准，§6.5.3） | `git clone https://github.com/intel/lmbench` 或发行版包 |
| **UnixBench** | 免费(GPL) | syscall/pipe/进程创建/execl/shell（对内核路径敏感，§6.5.3） | `git clone https://github.com/kdlucas/byte-unixbench && cd UnixBench && make` |
| **SPEC CPU2006** | **授权收费** | 纯计算负载（静态开销下界，§6.5.4） | 见下方专节 |
| **fio** | 免费(GPL) | 存储 IOPS/带宽/延迟（§6.6.3 / E7） | `apt install fio` |
| **netperf** | 免费 | 网络吞吐/延迟（§6.6.2 / E6） | `apt install netperf` |
| **wrk / nginx** | 免费 | HTTP 吞吐（web 负载） | `apt install wrk nginx` |
| **cyclictest** | 免费 | 中断/调度延迟（§6.5.3） | `apt install rt-tests` |
| **stress-ng / sysbench** | 免费 | 综合压力 / SPEC 替代 | `apt install stress-ng sysbench` |

### SPEC CPU2006 怎么拿（重要）

论文用 SPEC 只为一个**纯计算负载**，证明"静态开销下界"（几乎不触发随机化，开销只来自跳板
间接跳转 + PAC 指令）。

- **正路**：SPEC CPU2006 已停售但仍可授权，**大学/实验室多半有 SPEC 会员**——问导师/实验室/
  图书馆拿完整 ISO + license。或用更易拿到较新授权的 **SPEC CPU2017**。
- **注意**：GitHub 上的 `spec-cpu2006-redist` 只含光盘的 `redistributable_sources/`（第三方库
  源码），**不含 benchmark 负载、runspec 工具链、输入数据集，无法编译或运行**，别用它。
- **拿不到时的免费替代**（在 §6.5.4 写明替代理由即可，审稿接受）：
  `phoronix-test-suite`（7-zip/compress-gzip/c-ray/povray…）、`7z b`、`openssl speed`、
  `sysbench cpu`、`stress-ng --cpu N --metrics`。

---

## 2. 内核配置矩阵（自变量：启用的机制组合）

| 配置 | 第3章 | 第4章 | 第5章 | 构建 |
| --- | :-: | :-: | :-: | --- |
| **Base** | | | | IKASLR=n（纯净基线） |
| **+R** | ✓ | | | IKASLR=y，`trigger-loop.sh` 定频驱动 |
| **+RD** | ✓ | ✓ | | +XOM_EPT(x86，**单核**) / +观察点(arm) |
| **+RDP** | ✓ | ✓ | ✓ | +PACFI（**仅 arm64**） |

- **关键纪律：四档之间只能差 IKASLR 相关 CONFIG。** 同一份 base `.config`，只 toggle
  `IKASLR*` 开关，编译器/CONFIG_HZ/mitigations/base-KASLR 全部保持一致——否则测的是 config
  噪声不是随机化开销。配置生成：`scripts/ikaslr/perf/gen-configs.sh`。
- **+R 的定频驱动**：`trigger-loop.sh <间隔ms>` 从用户态定时写 `/proc/ikaslr/trigger`，间隔取
  与 Adelie 相同值（如 20 ms）便于同台对比；正文写明该值（§6.5.1 R-62）。
- **x86 XOM 是单核机制**：+RD(x86) 内核只在单核成立。做 XOM 开销对比时，**所有参与对比的档
  都用 `nr_cpus=1` 启动**（单核对单核才可比），或把 XOM 开销限定在单核 LMBench 子集。多核场
  景由 arm64 观察点覆盖。

---

## 3. 部署流程（裸机重启 A/B）

在**一台机器**上，装一个正常发行版（作 host/工作环境），把所有内核和 benchmark 都放好，
然后靠"换内核 + 重启"逐档测：

```
一次性准备（在正常内核下）：
  1. apt 装全部 benchmark（§1）；git clone + make LMBench / UnixBench。
  2. 为配置矩阵每档编译一个内核（gen-configs.sh + 编译器 pass，见 §0），
     make modules_install install —— 装进本机 /boot，各档一个 GRUB 菜单项。
  3. 记录每个内核的 GRUB entry 名（menuentry id），供 grub-reboot 脚本化选择。
  4. 造独占 scratch 分区/文件给 fio（§5 E7）。

逐档测（每档一轮）：
  5. grub-reboot "<该档 entry>" && reboot     # 指定下次启动进哪个内核
  6. 起来后 uname -a / dmesg | grep ikaslr 确认进对了内核、机制已启用。
  7. +R/+RD/+RDP：后台起 trigger-loop.sh <间隔> 让随机化真在跑。
  8. run-suite.sh <config>  ——  收集 /results/<arch>/<config>/ 原始数据。
  9. 跑完回到 host 环境，换下一档，回到第 5 步。
所有档跑完：
 10. analyze.py 汇总成四档对比表/图。
```

**测量纪律（裸机比 VM 更敏感，务必执行）**：
- **频率/热钉死**：`cpupower frequency-set -g performance`；关 turbo（intel_pstate
  `no_turbo=1` / AMD boost / ARM 对应项）；每档多次重启测，报中位数 + 离散度（重启间方差真实存在）。
- **延迟类（cyclictest）**再关深 C-state：内核启动参数加
  `processor.max_cstate=1 intel_idle.max_cstate=0`，并 `isolcpus=` + `taskset` 绑核隔核。
- 每项测 ≥5 次取分布，报中位数 + 置信区间（§6.5.2 W-65）；关无关服务、绑核降噪。
- 四档用**同一 rootfs、同一 benchmark 二进制**，只换内核。
- fio 用**固定独占 scratch**、`--direct=1`、每档测前状态一致（别让文件系统老化跨档变化）。

---

## 4. 测试项与命令（逐项）

### E4 LMBench（微基准，§6.5.3）
关注项：系统调用延迟、进程创建、上下文切换、页错误。
```
cd lmbench && make results          # 交互式配一次
# 或非交互：编辑 bin/*/CONFIG.* 后 make rerun
# 关键单测：
lat_syscall null read write open stat        # 系统调用延迟
lat_ctx -s 0 2 4 8                            # 上下文切换
lat_proc fork exec shell                      # 进程创建
lat_pagefault <file>                          # 页错误
bw_mem 256m rd wr                             # 内存带宽
```
表格：行=各项，列=四档配置。**特别关注中断相关**——第3章推迟随机化影响中断返回
路径、第4章检测在异常路径增开销。

### UnixBench（系统基准，§6.5.3）
经典系统吞吐基准，其中 `syscall`、`pipe`、`context1`、`spawn`、`execl`、`shell*`
子项直接反映内核路径开销，最贴合跳板/检测的影响。
```
cd UnixBench && ./Run -c 1 syscall pipe context1 spawn execl shell1 shell8   # 单核
./Run -c $(nproc)                                                            # 满核综合指数
```
报告各子项得分（相对 Base 的下降%）与总 index。

### cyclictest（中断/调度延迟，§6.5.3 补充）
```
cyclictest -l 1000000 -m -S -p 90 -i 200 -h 400 > cyclictest.txt
```
报告 max/99.9 分位延迟（安全由最坏情况定）。

### E5 SPEC CPU2006（纯计算，§6.5.4）
```
source shrc && runspec --config=ikaslr.cfg --size=ref --iterations=3 int fp
# 或免费替代：
phoronix-test-suite batch-run compress-7zip openssl c-ray
7z b ; openssl speed -evp aes-256-gcm ; sysbench cpu --threads=$(nproc) run
```
这类负载几乎不触发随机化，开销 = 跳板间接跳转 + PAC 指令的**静态下界**。

### E6 网络负载（§6.6.2）
netperf（吞吐 + 延迟，按报文尺寸/并发扫描）：
```
# 服务端: netserver
netperf -H <server> -t TCP_STREAM -l 30 -- -m 64,256,1024,16384   # 吞吐
netperf -H <server> -t TCP_RR -l 30 -- -r 1,1                     # 请求-响应延迟
```
web 负载：`wrk -t8 -c256 -d30s http://<server>/`。

### E7 存储负载（§6.6.3）
fio（按块大小/队列深度扫描），job 文件在 `scripts/ikaslr/perf/fio/`：
```
fio scripts/ikaslr/perf/fio/randread-4k.fio
fio scripts/ikaslr/perf/fio/randwrite-4k.fio
fio scripts/ikaslr/perf/fio/seqread-1m.fio
fio scripts/ikaslr/perf/fio/randrw-qd.fio    # 扫队列深度
```

### E3 四档端到端开销（§6.5.2）
以上各项在四档配置下各跑一遍，相对 Base 算开销百分比，分组柱状图。目标线：个位数%
（§1.4）。

### E9 随机化频率敏感性（§6.7.1 / 图 6-3，全文最关键图之一）
固定负载，人为扫触发频率，测开销曲线：
```
for hz in 0.5 1 5 20 100 500; do
    scripts/ikaslr/perf/trigger-loop.sh $((1000/hz)) &   # 后台定频触发
    <跑一项固定负载，如 fio randread 或 netperf>
    kill %1
done
```
图上标出：第4章实测触发频率区间（阴影带）+ §1.1.5 的 1.5–3.5 s 间隔上界（竖线）。
须落在"安全要求的频率之上、系统可承受的频率之下"。

### E10 随机化范围敏感性（§6.7.2）
以高扇入识别阈值 / `funcs.txt` 规模为自变量，测：随机化区域函数数、单次随机化耗时、
跨区域间接调用占比、PA 验证降幅、端到端开销。用不同规模的 `funcs.txt` 各编译一个
内核。

---

## 5. 与 Adelie / Dbox 的对比方法（§6.6.1，R-63/R-64）

**核心问题：范围不同不可直接比。** Adelie/Dbox 保护驱动/模块，I-KASLR 保护 built-in。
方案：

1. **同范围对照**：把 I-KASLR 的随机化范围限到与 Adelie 相同的**模块集**（如它评测
   用的网卡/NVMe 驱动），测一组"同范围"开销——这组可与 Adelie 直接比。
2. **built-in 范围**：另给 I-KASLR 在 built-in 范围上的开销（本文的真正目标，范围更大、
   开销自然更高）。主动说明这一点，把"范围更大"变成论据而非软肋。
3. **相同 workload**：跑 Adelie/Dbox 论文所用的**同类负载**（网络吞吐、存储、LMBench），
   参数尽量对齐它们论文的报告条件（报文尺寸、队列深度、间隔）。
4. **引用其数字**：Adelie/Dbox 的开源实现难在本平台复现，故**引用其论文的开销数字**，
   并在正文明确"实验环境不同、仅作数量级参照"（R-64）。

> **须核对**：Adelie（ASPLOS'22）与 Dbox 论文里 benchmark 的**确切配置**（工具、参数、
> 间隔、硬件），把本方案的参数对齐它们，才能引用其数字做对比。本方案给的是这些工作
> 通用的负载结构，具体参数以其论文为准。

---

## 6. 数据采集与分析

- 原始数据落 `/results/<arch>/<config>/<test>.txt`。
- `scripts/ikaslr/perf/analyze.py` 解析各工具输出，出：四档对比表、开销柱状图、
  频率敏感性折线、范围敏感性曲线。
- 每项 ≥5 次，报中位数 + 四分位/置信区间。
- E12 归因（§6.9）：用 `perf` 采样把端到端开销拆回来源（跳板间接跳转、计数、白名单、
  检测异常处理、PAC 指令），与各章微观测量对照；差额（缓存局部性/页表压力）如实报告。

---

## 附：最小可跑子集（先验证流程再铺开）

资源有限时先跑这几项验证整条流程通：
1. LMBench `lat_syscall`（四档）—— 微基准，最能看跳板开销。
2. fio `randread-4k`（四档）—— 存储，走 VFS 热路径。
3. netperf `TCP_STREAM` + `TCP_RR`（四档）—— 网络。
4. E9 频率敏感性（固定 fio 负载扫频率）—— 全文最关键图。
跑通后再加 SPEC/web/范围敏感性/对比。
