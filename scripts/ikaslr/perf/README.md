# I-KASLR 性能测试脚本（裸机重启 A/B）

在**一台真机**上，为每档配置编译并安装一个内核，靠"改下次启动项 + 重启"逐档跑基准，
测 I-KASLR 的性能开销。方法与理由见
[`../../../Documentation/crr/实现/10-perf-plan.md`](../../../Documentation/crr/实现/10-perf-plan.md)。

## 目录内容

| 文件 | 作用 |
| --- | --- |
| `env.sh` | 公共配置：架构、配置矩阵、路径、触发间隔、编译器 pass 开关。**所有脚本 source 它**，改参数改这里或从环境传入。 |
| `01-install-deps.sh` | 下载并安装全部基准（lmbench/unixbench/fio/netperf/…）与编译依赖。 |
| `02-build-kernels.sh` | 为每档配置编译一个内核（同一 defconfig，只 toggle IKASLR）。 |
| `03-install-kernels.sh` | 把各档装进 `/boot` 并加入 GRUB 菜单（各档独立 `uname -r`）。 |
| `04-boot-into.sh` | 用 `grub-reboot` 把下次启动设成某档并重启进入。 |
| `05-run-suite.sh` | 启动进某档后跑该档全部基准（绑频、定频触发随机化、收数据）。 |
| `06-analyze.py` | 汇总各档结果为对比表（相对 Base 的开销%）。 |
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

## 真实测量必读（否则测出来的开销不反映真实防护）

1. **编译器 pass（§0）**：默认 `CRR_TRAMPOLINE=n` 只随机化内建的 3 个示例函数，开销偏小，
   仅供跑通流程。真实测量要：
   ```bash
   # 编辑 funcs.txt 填入被测负载热路径上的内核函数
   CRR_TRAMPOLINE=y ./02-build-kernels.sh
   ```
   需已按 `../llvm/` 构建好 LLVM 插件并让主 Makefile 找得到（见仓库 README）。

2. **降噪**：05 会自动 `governor=performance` + 关 turbo。延迟类（cyclictest）还应关深
   C-state：给 `/etc/default/grub` 的 `GRUB_CMDLINE_LINUX_DEFAULT` 加
   `processor.max_cstate=1 intel_idle.max_cstate=0`，并考虑 `isolcpus=`。

3. **x86 +RD 是单核 XOM**：做 XOM 开销对比时，参与对比的**所有档都以 `nr_cpus=1` 启动**
   （改 `GRUB_CMDLINE_LINUX_DEFAULT` 加 `nr_cpus=1`、`update-grub` 后逐档测），单核对单核才可比。

4. **每项 ≥5 次取中位数**：05 跑单轮；多轮时对同一档多次运行到不同 `RESULTS_DIR`，
   各自取中位再比较。重启间方差真实存在，建议每档多次重启。

5. **只改 IKASLR**：各档从同一 defconfig 出发只 toggle IKASLR 相关 CONFIG，保证测的是
   随机化开销而非 config 噪声（02 已保证）。

## 常用可调参数（env.sh 或环境变量）

```bash
ARCH=arm64 ./02-build-kernels.sh         # 目标架构（默认按本机）
JOBS=64 ./02-build-kernels.sh            # 编译并行度
TRIGGER_MS=20 sudo ./05-run-suite.sh     # 随机化触发间隔（与 Adelie 对齐）
SERVER_IP=10.0.0.2 sudo ./05-run-suite.sh# 带网络项（对端需跑 netserver/nginx）
BENCH_DIR=... BUILD_ROOT=... RESULTS_DIR=...   # 各类路径
```
