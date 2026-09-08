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
| S2.1b | 构造 EPT 页表 + VMCS，VMLAUNCH 把内核降为 self-guest | 待做 |
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

无三重故障。VMXON 延迟 22 µs（含首次进入开销；§4.6.4 的 EPT 相关延迟测量应在
S2.1c 后补全）。

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
