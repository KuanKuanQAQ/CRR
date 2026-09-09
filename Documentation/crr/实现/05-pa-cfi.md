# 第 5 章实现细节：随机化辅助的 ARM PA 精准 CFI

> 对应论文第 5 章与 `PROGRESS.md` 的 Phase 3。设计依据见
> `../设计文档/草稿/毕业论文_第5章_v4.md`。**arm64 专属，需 FEAT_PAuth。**
> 本机用 QEMU TCG（`-cpu max`）功能验证；性能须真机。

## 核心思想（§5.1）

随机化区域内的控制流因目标地址不可预测而天然不可劫持，因此**进入随机化区域的
间接调用不需要 PA 验证**。把造成上下文传播的高扇入函数移入随机化区域，即可从
传播链中剪除它，链两侧的调用点得以各自建立唯一上下文。于是随机化与 CFI 互为条件。

## S3.1 PA 原语（已完成）

`kernel/ikaslr/pacfi.c`，`CONFIG_IKASLR_PACFI`（arm64 + ARM64_PTR_AUTH_KERNEL）。

用内核已安装的 APIA 密钥，配合我们自己的上下文：

```
sign(p, ctx) = PACIA p, ctx     认证是否通过：auth(sign(p,c),c) == p
auth(p, ctx) = AUTIA p, ctx      用错误 ctx 认证得到 != p
```

- 类型上下文 = 函数原型哈希（§5.5.1）；调用点上下文 = 调用点地址哈希。
- 显式 `pacia`/`autia` 需在 inline asm 里加 `.arch_extension pauth`（内核基础 -march
  不含 armv8.3；这条指令只**启用**扩展、不改限制，最自足）。

QEMU TCG 实测：签名在高位产生不同 PAC，roundtrip 通过，错误上下文被拒，
不同上下文签名互异。

```
probe: ptr=...16e4 sign(type)=3da1...16e4 sign(cs)=c0bf...16e4
probe: PASS - PAC works; roundtrip ok, wrong-context rejected, contexts distinct
```

## S3.2–S3.5（进行中）

见 `PROGRESS.md`。高扇入识别（三阶段）、渐进式精准化、跳板分区验证、迁入随机化区域。
