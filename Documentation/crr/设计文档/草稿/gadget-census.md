# ROP gadget 统计基线

CKASLR 的目标是让泄露出去的内核代码地址迅速作废，而 ROP/JOP/COP 攻击正是依赖这些
地址拼接 gadget。本文记录一次对内核镜像的 gadget 普查，作为**基线**：后续在开启
trampoline 注入 / 运行时重随机化之后再跑同样的统计，与此对比，就能量化 CKASLR 对
可利用 gadget 面的影响。

## 本次测量

| 项 | 值 |
| --- | --- |
| 日期 | 2026-09-08 |
| 工具 | ROPgadget 7.7 + capstone 5.0.9 |
| 目标 | `build/vmlinux` |
| 内核版本 | `6.8.0-g8fef1079aeeb`（x86-64） |
| 扫描范围 | ELF 可执行段（`.text` 约 18 MB，含 `.init.text` 等） |
| 参数 | 默认深度 10 条指令，去重（unique gadgets） |

> **这不是 6.16。** 仓库里唯一编译好的镜像是 6.8（即当前 `main` 的基线）。`v6.16-rc1`
> 只有 tag、从未编译。要 6.16 的数据需先 build，见文末「复现」。

## 关键前提：原始计数被噪声灌水

ROPgadget 7.7 默认把 **`jmp <立即地址>`**（跳转到固定目标）也算作 gadget。这类跳转的
目标是编译期定死的，攻击者无法借它转移控制流，对利用毫无价值 —— 却占了原始计数的
**77%**。因此下表把「可利用」与「噪声」分开；真正有意义的是可利用小计。

## 按 gadget 类型分列

| 类型 | 终止指令 | 数量 | 占全部 |
| --- | --- | ---: | ---: |
| **ROP** | `ret` | 30,148 | 2.6% |
| **ROP** | `ret imm` (retn) | 77,067 | 6.5% |
| **JOP** | `jmp reg` | 3,015 | 0.3% |
| **JOP** | `jmp [mem]` | 18,009 | 1.5% |
| **COP** | `call reg` | 3,515 | 0.3% |
| **COP** | `call [mem]` | 15,825 | 1.3% |
| **SYS** | `syscall` / `sysenter` / `int` | 697 | 0.1% |
| **可利用小计** | | **148,276** | **12.6%** |
| 噪声 | `jmp imm`（固定目标） | 907,765 | 77.1% |
| 噪声 | `retf`（远返回，罕用） | 71,429 | 6.1% |
| 噪声 | `iret` / `iretd` / `iretq` | 33,430 | 2.8% |
| 噪声 | `call imm`（固定目标） | 12,151 | 1.0% |
| 噪声 | 其他 | 4,210 | 0.4% |
| **原始总计** | | **1,177,261** | |

按大类归并：**经典 ROP 107,215** · **JOP 21,024** · **COP 19,340** · **SYS 697**。

## 可利用 ROP gadget（结尾 `ret` / `retn`）按首指令分类

| 首指令族 | 数量 | 占 ROP |
| --- | ---: | ---: |
| add / sub | 28,802 | 26.9% |
| 裸 `ret` | 13,179 | 12.3% |
| xor / and / or | 9,261 | 8.6% |
| mov | 5,719 | 5.3% |
| shift / rotate | 5,274 | 4.9% |
| test / cmp | 3,632 | 3.4% |
| inc / dec | 3,506 | 3.3% |
| push | 3,332 | 3.1% |
| xchg | 2,356 | 2.2% |
| **pop** | **1,730** | 1.6% |

## 最高价值：`pop <reg> ; ret`

这类 gadget 直接把栈上（攻击者可控）的值弹进寄存器，是 ROP 链里控制参数和状态的
核心原语。

| gadget | 数量 |
| --- | ---: |
| `pop rdi ; ret` | 208 |
| `pop rbp ; ret` | 86 |
| `pop rsp ; ret` | 66 |
| `pop rcx ; ret` | 65 |
| `pop rbx ; ret` | 63 |
| `pop rax ; ret` | 37 |
| `pop rdx ; ret` | 34 |
| `pop rsi ; ret` | 25 |
| `pop r14 ; ret` | 4 |
| `pop r13 ; ret` | 3 |
| `pop r8 ; ret` | 1 |

`pop rdi ; ret`（控制第 1 个参数）多达 208 条，是这份基线里最值得盯的数字。

## 复现

```sh
# 1. 装工具（纯 Python）
python3 -m venv /tmp/ropenv
/tmp/ropenv/bin/pip install ROPgadget capstone

# 2. 全量导出（当前 6.8 镜像）
/tmp/ropenv/bin/ROPgadget --binary build/vmlinux > gadgets_all.txt
#   末行给出 "Unique gadgets found: N"

# 3. 按终止指令分类（区分可利用 vs jmp/call 立即数噪声）
grep -E '^0x' gadgets_all.txt | sed 's/^[^:]*: //' \
  | awk -F' ; ' '{print $NF}' | awk '{print $1}' | sort | uniq -c | sort -rn

# 4. 若要真正的 6.16：先在 worktree 里编译 v6.16-rc1，再对它的 vmlinux 重跑
git worktree add ../CRR-6.16 v6.16-rc1
make -C ../CRR-6.16 O=build-616 x86_64_defconfig
make -C ../CRR-6.16 O=build-616 CRR_TRAMPOLINE=n -j"$(nproc)"
/tmp/ropenv/bin/ROPgadget --binary ../CRR-6.16/build-616/vmlinux > gadgets_616.txt
```

## 备注

- ROPgadget 7.7 把 `jmp/call` 立即数计入，Ropper 默认不计。用 Ropper 复核时，其数字
  应接近本文的「可利用」列而非「原始总计」。
- 深度、是否 `--multibr`、是否 `--all`（保留重复）都会改变绝对数值。做对比时务必对
  基线和目标用**完全相同**的参数。
- 这些计数针对静态镜像。CKASLR 不减少镜像里 gadget 的**数量**，而是让它们的**地址**
  在运行时不断变化，从而让「泄露地址 → 拼链」这条路失效 —— 对比实验应据此设计
  （比较地址稳定性 / 泄露有效期，而不是期待 gadget 计数下降）。
