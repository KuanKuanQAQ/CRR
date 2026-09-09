# 第 4 章实现细节：信息泄露检测与按需触发

> 对应论文第 4 章与 `PROGRESS.md` 的 Phase 2。设计依据见
> `../设计文档/草稿/毕业论文_第4章_v4.md`。本文随实现推进增补。

## 平台分工与部署模型

第 4 章有两条平台路径：

| 平台 | 检测机制 | 状态 |
| --- | --- | --- |
| x86 | 二级地址翻译（EPT）物理页级 XOM | 进行中（S2.1），嵌套 hypervisor |
| ARM | 硬件调试观察点 | 待做（S2.2），需 ARM 真机 |

**D-EPT 部署模型已定（作者 2026-09-09）：嵌套 EPT。** 内核**自带**一层薄
hypervisor 把自身降为 guest，用 EPT 施加"内核自己关不掉的"页权限。QEMU+KVM 只
提供裸机等价的嵌套虚拟化环境，**不参与 XOM 逻辑、没有 hypercall**——真机上就是
内核 late-launch 成 hypervisor（类似 BitVisor/SecVisor）。

### x86 EPT 路径的工程性质与分步

**这是一个学位论文级的子系统**，不同于前面每步都能一次干净验证的工作：它要正确
处理所有 VM exit、多核、且一旦 VMCS 字段出错就三重故障重启。因此分步实现，每步
独立可验证：

| 步 | 内容 | 状态 |
| --- | --- | --- |
| S2.1a | 进入/退出 VMX root 模式（VMXON/VMXOFF 往返） | ✓ 已验证 |
| S2.1b-1 | EPT identity 页表 + VMCS 加载 + EPTP 写入校验 | ✓ 已验证 |
| S2.1b-2a | 填满并校验全部 VMCS 字段（不 VMLAUNCH） | ✓ 已验证 |
| S2.1b-2b | VMLAUNCH 把内核降为 self-guest，捕获 VM exit | ✓ 已验证 |
| S2.1c | EPT 把代码物理页设为不可读，读触发 EPT violation 被捕获 | 待做 |
| S2.1d | 内存池划分 + 分配器适配 + 加载期页表切换 | 待做 |
| S2.5 | EPT violation → 控制流审计 → 触发随机化 | 待做 |

### S2.1a：VMX root 模式往返（已验证）

`kernel/ikaslr/xom_ept.c`，`CONFIG_IKASLR_XOM_EPT`。确认 L1 内核确实能进入 VMX
root 模式，即嵌套虚拟化对受保护内核可用——这是后续一切的地基。

流程：检查 `X86_FEATURE_VMX` → 确保 `IA32_FEAT_CTL` 启用 VMX（未锁定则锁定并启用）
→ 分配 VMXON 区域并写入 VMCS revision id（`IA32_VMX_BASIC[30:0]`）→ 关中断 →
置 `CR4.VMXE` → `VMXON` → `VMXOFF` → 复位。

QEMU+KVM（nested=Y）实测：

```
ikaslr/ept: S2.1a OK: entered and left VMX root mode (rev=300252880, VMXON 22441 ns)
ikaslr/ept: nested virtualization is available to the protected kernel
```

无三重故障。VMXON 延迟约 20 µs（含首次进入开销）。

### S2.1b-1：EPT 页表与 VMCS 基础设施（已验证）

EPT 用 **identity map + 1GB 大页**：guest 物理 = host 物理，512 个 1GB 项覆盖
512 GB，EPT 页表只需 PML4(1 项) + PDPT(512 项) 两级，几乎零内存开销；只有要
保护的 `.rand.text` 页在 S2.1b-2/c 里拆细设为 execute-only。VMCS 基础设施：
`vmclear`/`vmptrld`/`vmwrite`/`vmread` 封装，分配 VMCS region 并写 revision id。

在 VMX root 模式内实测：构造 EPT、加载 VMCS、写入 EPTP 并读回校验一致，无三重
故障：

```
ikaslr/ept: EPT identity map built: 512 GB, EPTP=3cf301e
ikaslr/ept: S2.1b OK: VMCS loaded, EPTP written+verified (3cf301e)
```

### S2.1b-2：VMLAUNCH（已验证）

**S2.1b-2a**：填满全部 host/guest/control 字段（约 55 个）并逐个校验 VMWRITE 成功。
guest state = 当前内核状态的延续；controls 配成最小拦截（不拦中断/异常，启用 EPT）。
先做这一步、不 VMLAUNCH，排除了字段编码/取值这一大类错误——事后证明这是 VMLAUNCH
一次通过的关键。

**S2.1b-2b**：最小 VMLAUNCH 回路。GUEST_RIP/RSP 指向 vmlaunch 之后，guest "继续"
执行到一条 `vmcall`；HOST_RIP 指向 exit 落点，VM exit 时以 host 状态继续。实测：

```
ikaslr/ept: S2.1b-2b OK: VMLAUNCH entered self-guest;
            caught VM exit reason=18 (18=VMCALL)
```

**内核成功把自身降为 self-guest、执行 guest 指令、捕获 VM exit，无三重故障，
全部 selftest 与用户态 SMOKE 仍 PASS，单次启动无重启循环。** 这是 EPT 路径技术上
最难的一关，一次通过（得益于 S2.1b-2a 先验证全字段合法）。

VMLAUNCH 失败可读 VM_INSTRUCTION_ERROR（VMCS 字段 0x4400）诊断，不会崩。

### S2.1c：物理页级 XOM（已验证）

identity map 用 1GB 大页，保护单页必须逐级拆细：**1GB → PD(512×2MB) → PT(512×4KB)**，
拆的同时把该区间其余部分照原样 identity 映射回去，否则 guest 会丢失同一 1GB 内其它
内存的映射。目标页设 **R=0, W=0, X=1**（可执行、不可读）。

先查 `IA32_VMX_EPT_VPID_CAP` 的 execute-only 能力位：不支持时 R=0,X=1 是非法组合，
会得到 EPT misconfiguration（reason 49）而非 violation，所以要区分这两种结果。

验证方式很直接：让 guest **先读被保护页再 vmcall**。XOM 生效则读被拦，永远到不了
vmcall——exit reason 直接给出结论。实测：

```
EPT: page 40d6000 now execute-only (R=0,W=0,X=1)
S2.1c OK: guest read of execute-only page trapped;
          EPT violation gpa=40d6000 qual=1a1
S2.1c: physical-page XOM is effective
```

`gpa` 与被保护页精确一致；`qual=0x1a1` 表示读访问且该页不可读。**第 4 章检测源一
（代码读取捕获）的硬件机制由此打通。**

> **踩到的坑**：第一次 VMLAUNCH 之后 VMCS 处于 launched 状态，再次 VMLAUNCH 会得到
> `VM_INSTRUCTION_ERROR=4`（VMLAUNCH with non-clear VMCS）。需 `VMCLEAR` 把它写回并
> 标为 clear（内容保留）、`VMPTRLD` 重新设为当前 VMCS，之后才能再次 VMLAUNCH；
> 持续运行的形态里则应改用 **VMRESUME**。

### S2.1d / S2.5（下一步）

- S2.1d：完整 exit/VMRESUME 循环（处理 CPUID 等无条件 exit），让内核主线**持续**在
  guest 里运行，以测量端到端开销与 EPT violation 处理延迟（§4.6.4）。
- S2.5：EPT violation → 标记被探测函数 → 第 4 章控制流审计 → 触发随机化；
  并把保护对象从测试页换成真正的 `.rand.text` / 当前 live 变体。

## S2.2/S2.3/S2.5（待做）

## S2.6 评估用含漏洞载体（已完成）

`kernel/ikaslr/evalvuln.c`，`CONFIG_IKASLR_EVAL_VULN`（依赖 DEBUG，默认 N）。

### 定位

论文 §4.6.1 的"攻击者能力载体"。威胁模型假设攻击者已具备任意内核读写；真实漏洞
成功率不稳定、原语强度不一，会把噪声混进检测覆盖率与响应延迟的测量。用一个能力
**恰好**为"任意读 + 任意写"的合成载体，使每次实验的攻击起点完全一致。真实 CVE
的完整攻击链验证另放 §6.8。

### 接口

`/proc/ikaslr/attack`（0600）：
- `r <addr> <len>` / `probe <addr> <len>`：读，随后从同一 fd 读回
- `w <addr> <hexbytes>`：任意写

读写用 `copy_{from,to}_kernel_nofault`，命中未映射区如实失败而非 oops——更贴近真实
越界读的行为。

### 三条约束（§4.6.1）的落实

1. **能力不多不少**：只有任意读/写，不额外泄露布局、不提供代码执行、不绕过任何机制；
2. **位于非随机化区域**：它代表攻击者已占据的立足点，未被标注为随机化函数；
3. **不参与性能测量**：由独立 Kconfig 控制，开销实验在不编译它的配置下进行。

### 伦理与安全

**这是一个故意开的内核后门**，把整个内核内存暴露给普通用户态。仅用于受控 QEMU
评估，绝不能进入可部署构建。加载时 `add_taint(TAINT_CRAP)` 并在日志里大声警告。
论文正文须写明它不随原型发布、不进任何部署配置。

### 验证（QEMU 实测）

普通用户态经后门读到随机化区域函数体的真实代码：

```
ikaslr/evalvuln: EVALUATION-ONLY arbitrary kernel R/W backdoor is ACTIVE.
ATTACK: target body at 0xffa000000004d1a0
ATTACK: read 16 code bytes: f3 0f 1e fa 85 f6 8d 04 37 0f 4e c7 c3 cc cc cc
ATTACK: PASS (leaked real code)
```

`f3 0f 1e fa`(endbr64) `85 f6`(test) `8d 04 37`(lea) 正是 `ikaslr_st_add_body`
的字节——这就是第 4 章检测机制要捕获的读取行为。当前无 XOM 硬件，读取尚不被
拦截；S2.1/S2.2 就位后同样的读取将触发随机化。

## S2.4 控制流审计（已完成）

`kernel/ikaslr/detect.c`。被探测函数的原地址被整体填成陷阱指令，别处保留正常副本；
经过被探测函数的控制流因此触发陷阱，由 `fixup.c` 的 die notifier 路由到
`ikaslr_detect_audit()`。

### 判据：陷阱在函数内的偏移

§4.4.4 的四类判定表，其核心区分是"目标是不是函数入口"：

| 陷阱落点 | 判定 | 处置 |
| --- | --- | --- |
| 入口（offset 0） | 正常的整函数调用/跳转 | 放行到副本，**不触发** |
| 中部（offset > 0） | 跳进函数中间 = gadget 使用模式 | 放行到副本，**触发随机化** |

关键：这个区分**直接从偏移读出，不需要知道来源指令**。精确的来源指令分类
（直接/间接调用、间接跳转、返回连击）需要 LBR(x86)/BRBE(arm64) 回溯，属于 S2.3
的增强。当前用偏移判据，抓住了四类表的核心区分且无 LBR 依赖即可验证。

**未覆盖（如实说明）**：§4.4.4"返回指令连续多次返回到被探测函数才触发"需要区分
返回与调用，同样依赖来源分类；当前偏移判据把"返回到函数中部"也当作 gadget，
对"返回到入口"则当作 benign。这是保守方向（可能少触发 return-to-entry 的 ROP），
S2.3 补 LBR 后可精确化。

### 验证（QEMU 实测，8 次压力测试全通过）

```
detect: marked audit_test probed: trap=ffa000000002d000 copy=ffa0000000055000 size=16
audit: entry call  -> 42, benign 0->1 gadget 0->0
audit: middle jump -> gadget 0->1 triggers 0->1
audit: PASS - entry=benign, middle=gadget+trigger
```

统计接入 `/proc/ikaslr/stats`（`audit_benign/gadget/triggers`）。

### 与随机化的集成边界（S2.5）

本文件独立验证审计逻辑，针对固定分配的陷阱/副本对。把"被探测"应用到随机化 live
副本、并在每次随机化后重新布设陷阱，是 S2.5 的集成工作。触发随机化已接
`ikaslr_request_rerandomize()`，链路是通的。

见 `PROGRESS.md`。S2.2（arm64 观察点，需真机）；S2.3（LBR 增强）；S2.5（与随机化集成）。
