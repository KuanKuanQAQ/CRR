# 真机实验操作手册

> 照着这份从上到下做即可。每一步都写明:**在哪台机器上、敲什么、跑多久、
> 产出什么、怎么判断这一步没白跑**。
>
> 对应 `Documentation/crr/设计文档/草稿/实验总清单.md`(v2)。
> 已在本机(QEMU+KVM)完成的实验见 `Documentation/crr/实现/实验/README.md`,
> 本手册只覆盖**必须上真机**的部分。

---

## 0. 你需要几台机器

| 机器 | 用途 | 必需性 |
|---|---|---|
| **x86-64 裸机** | E3 四档端到端、E6/E7、E9、E10、E12、E4-A/B | 必需 |
| **ARM64 裸机**(带 FEAT_PAuth) | R-61、E5-B、E5-5,以及 E3/E6/E7 的 ARM 一侧 | 第 5 章必需 |
| **第二台机器**(同网段) | E6 网络实验的对端(`netserver` / nginx) | E6 必需 |

> ⚠ **x86 的 `+RD` 档是单核机制**(EPT 只执行)。涉及 XOM 对比时,参与对比的
> **所有档**都要以 `nr_cpus=1` 启动,单核对单核才可比。多核场景由 ARM 观察点承担。

---

## 1. 先做:R-61 平台能力确认(ARM 机,半天)

**这一步决定主平台,一变则第 6 章大半重做。别跳过。**

```sh
# 在 ARM 机上,普通内核即可
cd <repo>/scripts/ikaslr/perf
./14-arm-precheck.sh
```

看结论段。**如果 `paca` 不可用,ARM 不能作为第 5 章的平台** —— 先换机器或改写正文,
再往下做。

脚本会明确告诉你两条它**确认不了**的事(观察点能否在生产配置下于 EL1 触发、
`DBGWCR_EL1.MASK` 的实际位宽) —— 那两条要等装上 I-KASLR 内核后读自测输出。

---

## 2. 装依赖(每台机器一次,约半小时)

```sh
sudo ./01-install-deps.sh
```

装 lmbench / UnixBench / fio / netperf / cyclictest / sysbench / perf 与编译依赖。
装完自查:

```sh
for c in lat_syscall fio netperf cyclictest perf clang; do
    printf "%-12s %s\n" "$c" "$(command -v $c || echo '缺')"
done
```

**`clang` 必须有** —— `-fpass-plugin` 是 clang 特性,gcc 编不出随机化内核。

---

## 3. E3 四档端到端(x86 与 ARM 各一遍,每遍约 1 周日历时间)

这是**性能实验的基线**,后面的 E9/E10/E12 都依赖它。

### 3.1 编四档内核(约 30 分钟)

```sh
cd <repo>/scripts/ikaslr/perf
./02-build-kernels.sh                 # 默认 S3(483 个函数)
```

**跑完必看两行**:

```
-- 校验可搬移性
✅ 全部可搬移：无指向区域外的 PC 相对引用
```

若报"不可搬移",**不要继续** —— 按提示把那些函数从名单里剔除后重编。
带着不可搬移的函数启动会崩。

三条纪律脚本已经替你把住了:
- 四档从同一份 defconfig 出发,只 toggle `IKASLR*`;
- `retpoline`/`unwinder`/`BTI` 这些**四档一律相同**(它们是"代码可搬移"的前提);
- **base 档用空名单而不是关掉插件** —— 否则两档的编译流水线不同,
  差额里会混进"加载插件本身"的影响。

### 3.2 装内核并进菜单(约 10 分钟)

```sh
sudo ./03-install-kernels.sh
```

它会打印每档的 `uname -r` 与对应的 GRUB 菜单项。

### 3.3 逐档重启并跑基准(每档约 2 小时)

对 `base` / `R` / `RD`(ARM 还有 `RDP`)各做一遍:

```sh
sudo ./04-boot-into.sh base --now      # 重启进 base

# 起来之后
uname -r                               # 确认进对了内核
sudo SERVER_IP=10.0.0.2 ./05-run-suite.sh
```

`05` 会**先自检再跑**,以下情况直接退出而不是白跑两小时:
- 非 base 档但 `/proc/ikaslr/stats` 不存在 → 内核没开 `CONFIG_IKASLR`
- 随机化函数数 ≤ 3 → 编内核时没给 `CRR_TRAMPOLINE=y` / `IKASLR_FUNCS`,四档等价
- base 档却有 `/proc/ikaslr/stats` → 启动错内核了

**先用 `QUICK=1` 打通一遍流程**(约 10 分钟),确认四档都能跑完、结果目录长对了,
再正式跑:

```sh
QUICK=1 sudo ./05-run-suite.sh         # 只为打通流程，数据别用
```

### 3.4 采 G(跨区域调用频度)—— 额外一档,约 2 小时

E3-A 的开销模型是 `总开销 ≈ G × 常驻税 + M × K`。要把端到端结果接回模型,
需要**同一负载下的 G**,而 `enters`/`outs` 计数要 `CONFIG_IKASLR_STATS=y`。

```sh
./02-build-kernels.sh Rg               # Rg = R + STATS，只用来测 G
sudo ./03-install-kernels.sh Rg
sudo ./04-boot-into.sh Rg --now
sudo SERVER_IP=10.0.0.2 ./05-run-suite.sh Rg
```

统计本身实测只值 1.0–1.6 ns/次(见 E3-A),因此 Rg 的吞吐与 R 相差约 2%,
用它测 G 是够的。**Rg 不参与开销对比**,只提供 G。

### 3.5 汇总

```sh
./06-analyze.py
```

产出三张表:逐项四档对比、逐负载的 G、以及**模型核对**(E3-A 的预测 vs 实测)。

**判据**:每项 ≥5 次,报中位数 + 四分位(脚本默认 `REPEAT=5`)。

---

## 4. E9 频率敏感性(x86,约 3–4 天)

清单称"全文最关键实验图之一"。在**已进 `+R` 或 `+RD` 档**之后:

```sh
sudo ./07-freq-sweep.sh
# 或自定义: FREQS="0 1 5 20 100 500" sudo ./07-freq-sweep.sh
```

产出 `sweep.csv` 与一张汇总表。作图:横轴频率(**对数刻度**),纵轴相对 0 Hz 的开销%。

**图上要标两处**:第 4 章实测触发频率的分布区间(阴影带,来自 §5 的 `13-detect-cost.sh`);
§1.1.5 的 1.5–3.5 s 间隔上界对应的频率(竖虚线)。

**判据(全章成败所系)**:实测触发频率要落在"安全所要求的频率之上、系统可承受的
频率之下"。两个区间若不重叠,全文论证有问题。

---

## 5. E4-A / E4-B 检测开销与触发频率(x86 **裸机**,约 1 周)

```sh
sudo ./04-boot-into.sh RD --now        # 需要 +RD 档
sudo MINUTES=30 ./13-detect-cost.sh
```

⚠ **必须裸机**。x86 的 EPT 检测路径在嵌套虚拟化下开销被放大约 10 倍
(实测 VM exit 18 279 周期/次 vs 裸机约 1 000–2 000)。脚本检测到虚拟化会警告。

**E4-B 的第一步结论最重要**:若正常负载下触发频率≈0,那本身就是需求 D2
("不因正常行为频繁误触发")的证据,**原 E4-3 误报率实验不必做**。
若触发频繁,说明内核自身的合法代码读取(kprobes/ftrace/栈回溯/`/proc/kcore`)
被当成了攻击,那时才需要回头做来源归类。

脚本会明确列出**尚缺的一块**:XOM/EPT 那条链的分阶段计时还没插桩
(当前只测到审计路径)。要补的话在 `kernel/ikaslr/xom_ept.c` 里加。

---

## 6. E10 范围敏感性 + E5-B 降幅(x86 与 ARM,约 1 周)

清单要求两者**合并做**:同一批内核跑一次即可。

```sh
sudo ./08-range-sweep.sh               # 编并装 16/64/256/全量 四个规模
# 逐个规模：
sudo ./04-boot-into.sh scope-e10-16 --now
sudo ./08b-range-collect.sh
sudo ./05-run-suite.sh scope-e10-16    # 端到端开销（可选，慢）
# ...对 e10-64 / e10-256 / s3-full 重复...
./08c-range-analyze.py
```

**为什么必须做**:这组数据是三章之间参数耦合的**唯一定量呈现**。缺了它,
三章的取值看起来是各自拍定的。

**读表时注意**:G **随范围饱和而非线性增长**(本机实测 21→485 个函数,
G 只从 44 M 涨到 59 M 次/秒)。正文要写清,否则读者会线性外推。

---

## 7. E6 / E7 网络与存储(x86,约 4–5 天)

已经包含在 `05-run-suite.sh` 里(fio 四组 job + netperf 报文尺寸扫描 + wrk)。
需要:

```sh
# 对端机器
netserver -D                            # netperf 服务端
# 若要 wrk：起一个 nginx，默认页即可

# 被测机
sudo SERVER_IP=<对端IP> ./05-run-suite.sh <档名>
```

**fio 要独占 scratch**:用固定分区、`--direct=1`,每档测前状态一致,
别让文件系统老化跨档变化。job 文件在 `fio/` 下。

**判据**:小报文/小块尺寸每单位数据要跨更多次区域边界,开销应**显著高于**
大报文/大块。若不呈现这一趋势,说明常驻税模型有问题,回头核对 E3-A 的 G 项。

---

## 8. E12 开销归因(x86,约 3 天,依赖前面全部数据)

```sh
sudo ./09-attribution.sh
```

⚠ 一个必须知道的限制:**perf 无法符号化变体里的函数体** —— 它们在 vmalloc 区,
kallsyms 解析不到,在 `perf report` 里表现为一大块 `[unknown]`。脚本因此**按地址
归桶**:落在当前变体区间内的样本单独成一桶。只看符号名会把随机化代码的开销整块漏掉。

**本节的真正价值**:若端到端开销显著高于各微观开销之和,**要解释差额来自何处**
(缓存局部性、页表压力、变体准备与前台争抢 CPU)。这类二阶效应只有在系统级才观察得到,
**如实报告反而是本章的价值**。

---

## 9. E8 与 Adelie/Dbox 对比(约 1 周,取决于 R-64)

三步(清单):

```sh
# 第一步：同范围对照 —— 把随机化范围限到 Adelie 评测用的驱动集合
IKASLR_FUNCS=<repo>/scripts/ikaslr/funcs/adelie-scope.txt ./02-build-kernels.sh R
sudo ./03-install-kernels.sh R && sudo ./04-boot-into.sh R --now
sudo ./10-compare-adelie-dbox.sh

# 第二步：built-in 范围（真正的目标）—— 就是 §3 的 S3 结果
# 第三步：相同 workload —— 10 脚本已按 Adelie 论文的负载与 20 ms 触发间隔设好
```

**核心口径**:两者保护范围不同,直接比开销不公平,**而且不公平的方向对本文有利**
(本文范围更大、代价自然更高)。要主动说明范围差异,把"范围更大"变成论据而不是软肋。

**R-64 要尽早确认**:Adelie/Dbox 能否在本平台复现。若不能,只能引用原文数字并在
正文明确"实验环境不同,仅作数量级参照" —— 这决定 §6.6 是"同台对比"还是
"与文献数字对照",两者写法完全不同。

---

## 10. R-66 长跑漂移(挂着跑,不占人力)

建议在别的实验做完之后挂上:

```sh
nohup sudo HOURS=72 ./15-longrun.sh &
```

判据:`functions` 与 `whitelist_entries` 是静态量**不该变**;`vmalloc_kb` 应稳定在
变体池 + 陷阱影像那个量级,**持续上涨就是泄漏**。

被探测函数集合只进不出,其陷阱页与副本会随时间增长 —— 那是**已知未解决项**
(见 `实验/E4-D` 的判据核对),本脚本的 vmalloc 曲线正是它的证据。

---

## 11. 常见坑(都踩过)

| 现象 | 原因 | 办法 |
|---|---|---|
| `05` 报"随机化函数数 ≤ 3" | 编内核时没给 `CRR_TRAMPOLINE=y` / `IKASLR_FUNCS` | 重编,见 §3.1 |
| 编译报"不可搬移的函数" | 静态启发式漏网(per-CPU、内联汇编里的全局操作数) | 按提示剔除后重编,迭代到 0 |
| 启动即崩 | 带着不可搬移的函数启动 | 同上。**别跳过 `verify_movable`** |
| 四档差异异常大 | 强制配置(retpoline/unwinder/BTI)四档不一致 | `02` 已统一,别手改 `.config` |
| `perf` 归因看不到随机化代码 | 变体在 vmalloc 区,kallsyms 解析不到 | 看 `09` 的"按地址归桶"那一桶 |
| ftrace/kprobes 对被随机化的函数无效 | **已知行为,不是 bug** | 见 `实验/E3-C`;它们**不必关闭**,四档一致即可 |
| 数字比预期好太多 | 微基准被编译器优化掉了 | 输入取自 `volatile`、结果写回。做任何微基准前先自查这一条 |

---

## 12. 每步产出落在哪

```
scripts/ikaslr/perf/results/<arch>/
├── base/ R/ RD/ RDP/ Rg/        每档的基准结果（05 产出，06 汇总）
│   └── <负载>/rep<N>.txt, g.csv
├── freq-sweep/<档>/sweep.csv    E9
├── range-sweep/scope-*.{env,rounds.csv,g}.txt   E10（08b 产出，08c 汇总）
├── attribution/<档>/            E12
├── detect/                      E4-A/E4-B
├── longrun/                     R-66
└── arm64/precheck/              R-61
```

汇总脚本:`06-analyze.py`(E3/E6/E7)、`08c-range-analyze.py`(E10)。
