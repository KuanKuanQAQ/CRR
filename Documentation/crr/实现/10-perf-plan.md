# I-KASLR 性能开销完整测试方案（可部署）

面向作者在**两台真机**（x86-64 一台、arm64 一台，各装 KVM）上部署测试。本文给出
所有测试项、获取方式、部署流程与数据采集。配套脚本在 `scripts/ikaslr/perf/`。

> **关于本机 QEMU 数字**：本仓库到目前的性能数字取自开发机 QEMU（x86 EPT 走嵌套
> 虚拟化，开销被放大约 10 倍；arm64 走 TCG 纯模拟）。**这些不是论文最终值**，只用于
> 验证功能正确。论文的性能数字须在真机 + KVM 上按本方案重测。

---

## 0. 先决条件（务必先做）

**编译器 pass 必须先应用到被测子系统。** 否则跳板/PAC 不在 benchmark 实际走的代码
路径上（syscall、VFS、网络栈…），测出来的开销不反映真实防护。步骤：

1. 用名单文件指定要随机化的函数（`IKASLR_FUNCS`），覆盖被测负载的热路径。
   起步集合建议：`fs/`（VFS 核心：`vfs_read/write/open/...`）、`net/core/`、
   目标网卡/存储驱动。先小后大。
2. 用 LLVM 或 GCC 插件编译内核（见 `07-compiler.md`）：
   ```
   make ... KBUILD_CFLAGS+=" -fpass-plugin=$PWD/tools/ikaslr/llvm/libIKaslrPass.so"
   IKASLR_FUNCS=funcs.txt make ...
   ```
   或 GCC：`-fplugin=tools/ikaslr/gcc/ikaslr_gcc.so -fplugin-arg-ikaslr_gcc-funcs=funcs.txt`
3. 编译后先跑 `scripts/ikaslr/scan_xregion.py vmlinux` 确认无绕过跳板的跨区域转移
   （§6.4 的地基），再进入性能测试。

**范围（阈值）是自变量之一**（§6.7.2 / E10）：不同的 `funcs.txt` 规模对应不同开销，
须扫多个规模。

---

## 1. 基准软件：是什么、怎么获取

| 软件 | 授权 | 测什么 | 获取 |
| --- | --- | --- | --- |
| **LMBench** | 免费(GPL) | 系统调用/上下文切换/页错误/内存带宽延迟（微基准，§6.5.3） | `git clone https://github.com/intel/lmbench` 或发行版包 `lmbench` |
| **SPEC CPU2006** | **授权收费** | 纯计算负载（静态开销下界，§6.5.4） | spec.org 购买（~$800–1k）；**多数大学有会员授权**，问实验室/图书馆；或用下方免费替代 |
| **fio** | 免费(GPL) | 存储 IOPS/带宽/延迟（§6.6.3 / E7） | `apt install fio` |
| **netperf** | 免费 | 网络吞吐/延迟（§6.6.2 / E6） | `apt install netperf` |
| **iperf3** | 免费 | 网络带宽（备选） | `apt install iperf3` |
| **wrk / ab** | 免费 | HTTP 吞吐（web 负载） | `apt install wrk apache2-utils` |
| **nginx/apache** | 免费 | web 服务端 | `apt install nginx` |
| **cyclictest** | 免费 | 中断/调度延迟（§6.5.3 关注项） | `apt install rt-tests` |
| **stress-ng** | 免费 | 综合压力（SPEC 替代之一） | `apt install stress-ng` |

### SPEC CPU2006 拿不到时的免费替代

论文用 SPEC 只是要一个**纯计算负载**证明"静态开销下界"（几乎不触发随机化，开销
只来自跳板间接跳转 + PAC 指令）。以下免费替代可达到同样目的（在正文说明替代理由）：

- **Phoronix Test Suite**（`phoronix-test-suite`）：含大量开源计算基准（7-zip、
  compress-gzip、c-ray、povray、build-linux-kernel 等）。
- **单点程序**：`openssl speed`、`7z b`（7-Zip 内置基准）、`sysbench cpu`、
  `stress-ng --cpu N --metrics`、内核自身 `perf bench`。
- 也可考虑 **SPEC CPU2017**（同样授权，但更易通过大学拿到较新授权）。

> 建议：优先争取 SPEC CPU2006 授权（与论文一致、可比性最好）；确实拿不到，用
> Phoronix + 7-Zip + openssl speed 组合，并在 §6.5.4 说明"因授权原因用开源计算
> 基准替代 SPEC"。

---

## 2. 四档内核配置（自变量：启用的机制组合）

| 配置 | 第3章 | 第4章 | 第5章 | 构建方式 |
| --- | :-: | :-: | :-: | --- |
| **Base** | | | | IKASLR=n（纯净基线） |
| **+R** | ✓ | | | IKASLR=y，固定间隔驱动（见下）|
| **+RD** | ✓ | ✓ | | +XOM_EPT(x86) / +观察点(arm) 按需触发 |
| **+RDP** | ✓ | ✓ | ✓ | +PACFI（**仅 arm64**）|

- **+R 的"固定间隔驱动"**：第3章本身不含触发策略。用
  `scripts/ikaslr/perf/trigger-loop.sh <间隔ms>` 从用户态定时写 `/proc/ikaslr/trigger`，
  间隔取与 Adelie 相同的值（如 20 ms）以便同台对比；须在正文写明该值及其与 +RD 实测
  触发频率的关系（§6.5.1 R-62）。
- 配置生成脚本：`scripts/ikaslr/perf/gen-configs.sh`。

---

## 3. rootfs 准备

真机 + KVM 上跑真实负载需要完整 rootfs（本项目原有的 `boot/create-image.sh` 造的
Debian bullseye 镜像可复用），并预装上述基准工具。脚本
`scripts/ikaslr/perf/mkrootfs-bench.sh` 在现有镜像上 `chroot` 安装 fio/netperf/
iperf3/wrk/nginx/rt-tests/lmbench/stress-ng 等。

SPEC/Phoronix 体积大、需单独安装，脚本留了挂载点说明。

---

## 4. 部署流程

```
每台真机（x86 / arm64）：
  1. 装 KVM，确认 /dev/kvm 可用、CPU 虚拟化开启（BIOS）。
  2. 为四档配置各编译一个内核（gen-configs.sh + 编译器 pass，见先决条件）。
  3. 造一个装好基准的 rootfs（mkrootfs-bench.sh）。
  4. 逐档启动 QEMU+KVM：
     x86 : boot/boot.sh                （ARCH=x86_64，KVM=1）
     arm : KVM=1 ARCH=arm64 boot/boot.sh
  5. 每档跑 run-suite.sh，收集 /results/<config>/ 下的原始数据。
  6. analyze.py 汇总成表/图。
```

**测量纪律**：
- 每项测 ≥5 次取分布（不是单次），报告中位数 + 置信区间（§6.5.2 W-65）。
- 关闭无关服务、固定 CPU 频率（`cpupower frequency-set -g performance`）、
  绑核（`taskset`）以降噪。
- +R/+RD/+RDP 与 Base 用**同一 rootfs、同一 QEMU 参数**，只换内核。
- 中断延迟类（cyclictest）在虚拟化下噪声大，须多跑并如实标注环境。

---

## 5. 测试项与命令（逐项）

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

## 6. 与 Adelie / Dbox 的对比方法（§6.6.1，R-63/R-64）

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

## 7. 数据采集与分析

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
