# 第 4 章实现细节：信息泄露检测与按需触发

> 对应论文第 4 章与 `PROGRESS.md` 的 Phase 2。设计依据见
> `../设计文档/草稿/毕业论文_第4章_v4.md`。本文随实现推进增补。

## 平台分工与待定项

第 4 章有两条平台路径：

| 平台 | 检测机制 | 状态 |
| --- | --- | --- |
| x86 | 二级地址翻译（EPT）物理页级 XOM | 待做（S2.1），见下方 D-EPT |
| ARM | 硬件调试观察点 | 待做（S2.2），需 ARM 真机 |

**待定项 D-EPT（部署模型）**：EPT 要给"受保护内核自己关不掉的"页权限，这意味着
受保护内核要么跑在一层薄 hypervisor 之下、要么让 XOM 强制落在 L0/KVM。在 QEMU+KVM
里跑受保护内核（L1）、要它自己用 EPT 保护自己，需要**嵌套 EPT**；否则 XOM 逻辑
得实现在宿主 KVM 侧。这一决策留待 S2.1，当前运行时写成平台无关。

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

## S2.4/S2.3/S2.1/S2.2（待做）

见 `PROGRESS.md` Phase 2。下一步 S2.4 控制流审计（陷阱页重映射），可复用已有的
`fixup.c` die notifier，且无 XOM 硬件依赖即可验证。
