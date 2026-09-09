# I-KASLR 性能测试脚本（裸机重启 A/B）

在**一台真机**上，为每档配置编译并安装一个内核，靠"改下次启动项 + 重启"逐档跑基准，
测 I-KASLR 的性能开销。方法与理由见
[`../../../Documentation/crr/实现/10-perf-plan.md`](../../../Documentation/crr/实现/10-perf-plan.md)。

## 目录内容

| 文件 | 作用 |
| --- | --- |
| `env.sh` | 公共配置：架构、配置矩阵、路径、触发间隔、编译器 pass 开关。**所有脚本 source 它**，改参数改这里或从环境传入。 |
| `01-install-deps.sh` | 下载并安装全部基准与编译依赖。**自动识别包管理器**（apt / dnf / yum / zypper——Debian/Ubuntu 与 openEuler/RHEL/Fedora/openSUSE 都可）。 |
| `02-build-kernels.sh` | 为每档配置编译一个内核（同一 defconfig，只 toggle IKASLR）。 |
| `03-install-kernels.sh` | 把各档装进 `/boot` 并加入 GRUB 菜单（各档独立 `uname -r`）。 |
| `04-boot-into.sh` | 用 `grub-reboot` 把下次启动设成某档并重启进入。 |
| `05-run-suite.sh` | 启动进某档后跑该档全部基准（绑频、定频触发随机化、收数据）。 |
| `06-analyze.py` | 汇总各档结果为对比表（相对 Base 的开销%）。 |
| `07-freq-sweep.sh` | **E9** 随机化频率敏感性：固定负载扫触发频率，出 §6.7.1 曲线数据。 |
| `08-range-sweep.sh` | **E10** 随机化范围敏感性：按 `funcs.txt` 规模各编一档内核并安装，逐档测。 |
| `09-attribution.sh` | **E12** 开销归因：perf 采样把开销拆回跳板/计数/白名单/检测/PA/XOM。 |
| `10-compare-adelie-dbox.sh` | **E8** 与 Adelie/Dbox 同类负载对比（@20ms）。方法+文献数字见 `实现/11-related-comparison.md`。 |
| `trigger-loop.sh` | 定频写 `/proc/ikaslr/trigger` 驱动随机化（被 05 调用）。 |
| `fio/` | fio job 文件（存储负载）。 |
| `funcs.txt` | 编译器 pass 的随机化函数名单（§0，真实测量时必填）。 |
| `results/` | 测试结果输出（`<arch>/<variant>/`，默认不入库）。 |

## 配置矩阵（自变量 = 启用的机制）

| 档 | 第3章 R | 第4章 D | 第5章 P | 说明 |
| --- | :-: | :-: | :-: | --- |
| `base` | | | | IKASLR=n 纯净基线 |
| `R` | ✓ | | | 随机化 + 定频触发 |
| `RD` | ✓ | ✓ | | x86=XOM_EPT（**单核**）/ arm=观察点 |
| `RDP` | ✓ | ✓ | ✓ | +PACFI（**仅 arm64**） |

## 用法（一台机器，逐档轮测）

```bash
# —— 一次性准备（在正常内核下，联网）——
sudo ./01-install-deps.sh
./02-build-kernels.sh                 # 编全部档（CRR_TRAMPOLINE=n：仅示例函数）
sudo ./03-install-kernels.sh          # 装进 /boot + GRUB，打印各档 uname -r

# —— 逐档测（每档一轮）——
sudo ./04-boot-into.sh base --now     # 设下次启动=base 并重启
#   ...重启进入 base 后...
sudo ./05-run-suite.sh                # 自动识别当前档，跑基准，落 results/<arch>/base/
sudo ./04-boot-into.sh R --now        # 下一档
sudo ./05-run-suite.sh
# ...RD、RDP 同理...

# —— 汇总 ——
./06-analyze.py                       # 读 results/<arch>/，出四档对比表
```

## 进阶实验（E8/E9/E10/E12）

在四档端到端测完（05）之后，按需跑这几项——它们各自需要**已启动进相应档**：

```bash
# E9 频率敏感性（启动进 R 或 RD 后）：
FREQS="0 1 5 20 100 500" sudo ./07-freq-sweep.sh

# E10 范围敏感性（需编译器 pass + 足够大的 funcs.txt）：
CRR_TRAMPOLINE=y IKASLR_FUNCS=大名单.txt SIZES="16 64 256 1024" sudo ./08-range-sweep.sh
#   ...再逐个 04-boot-into.sh scope<N> + 05-run-suite.sh + 读 /proc/ikaslr/layout

# E12 归因（启动进 R/RD/RDP 后）：
sudo ./09-attribution.sh

# E8 与 Adelie/Dbox 同类负载对比（@20ms；scoped 档与 built-in 档各一遍）：
sudo ./10-compare-adelie-dbox.sh
```
对比对象的范围、开销与公平对比方法见 [`../../../Documentation/crr/实现/11-related-comparison.md`](../../../Documentation/crr/实现/11-related-comparison.md)。

## 真实测量必读（否则测出来的开销不反映真实防护）

1. **编译器 pass（§0）—— 不开就等于没随机化。** 进入随机化集只有两条路：手工标注
   `IKASLR_RAND_FN`（只有 selftest.c / randfuncs.c 里的 3 个示例函数，且它们是
   `IKASLR_DEBUG` 专属，性能档 DEBUG 关闭时**根本不编译**），或编译器 pass。因此：
   - 默认 `./02-build-kernels.sh`（R/RD 档，pass off、DEBUG off）：随机化表为空，
     **一个函数都不随机化**，+R/+RD 与 base 几乎无差别——仅供跑通流程，**测不出真实开销**。
   - 真实测量必须开 pass 并填名单：
     ```bash
     # 1) 构建 LLVM 插件（见仓库根 README / tools/ikaslr/llvm），让主 Makefile 找得到
     # 2) 编辑 funcs.txt 填入被测负载热路径上的真实内核函数（vfs_read、tcp_sendmsg …）
     CRR_TRAMPOLINE=y ./02-build-kernels.sh
     ```
     这样名单里的真实函数才会被改造成"可迁移体 + 固定跳板"，benchmark 的热路径才真正走随机化代码。

2. **降噪**：05 会自动 `governor=performance` + 关 turbo。延迟类（cyclictest）还应关深
   C-state：给 `/etc/default/grub` 的 `GRUB_CMDLINE_LINUX_DEFAULT` 加
   `processor.max_cstate=1 intel_idle.max_cstate=0`，并考虑 `isolcpus=`。

3. **x86 +RD 是单核 XOM**：做 XOM 开销对比时，参与对比的**所有档都以 `nr_cpus=1` 启动**
   （改 `GRUB_CMDLINE_LINUX_DEFAULT` 加 `nr_cpus=1`、`update-grub` 后逐档测），单核对单核才可比。

4. **每项 ≥5 次取中位数**：05 跑单轮；多轮时对同一档多次运行到不同 `RESULTS_DIR`，
   各自取中位再比较。重启间方差真实存在，建议每档多次重启。

5. **只改 IKASLR**：各档从同一 defconfig 出发只 toggle IKASLR 相关 CONFIG，保证测的是
   随机化开销而非 config 噪声（02 已保证）。

## 关于 openEuler / rpm 系

`01-install-deps.sh` 会自动用 `dnf` 安装(而非 `apt`),包名已按 rpm 系映射
(如 `libssl-dev`→`openssl-devel`、`linux-cpupower`→`kernel-tools`、`p7zip-full`→`p7zip`)。
openEuler 仓库通常没有 `netperf` 和 `wrk`,脚本会**自动从源码编译**它们;若某些基准包
(如 `sysbench`)仓库里也没有,可先启用 EPOL 源再重跑:
```bash
sudo dnf install -y epol-release && sudo dnf makecache   # 视 openEuler 版本而定
```
`7z` 在 openEuler 上叫 `7za`,`05-run-suite.sh` 已自动兼容。

## 常用可调参数（env.sh 或环境变量）

```bash
ARCH=arm64 ./02-build-kernels.sh         # 目标架构（默认按本机）
JOBS=64 ./02-build-kernels.sh            # 编译并行度
LOCALMOD=1 ./02-build-kernels.sh         # 只编当前 lsmod 已加载的模块（快很多；仅本机语义）
TRIGGER_MS=20 sudo ./05-run-suite.sh     # 随机化触发间隔（与 Adelie 对齐）
SERVER_IP=10.0.0.2 sudo ./05-run-suite.sh# 带网络项（对端需跑 netserver/nginx）
BENCH_DIR=... BUILD_ROOT=... RESULTS_DIR=...   # 各类路径
```
