# CRR — Continuous Kernel Re-Randomization

CRR 是一个 Linux 内核研究分支，实现**连续内核地址空间布局随机化**（continuous KASLR，
代码中记为 `CKASLR`）。标准 KASLR 只在启动时随机化一次内核镜像的加载基址，一旦攻击者
通过信息泄露拿到任意一个内核地址，整个布局在这次启动的剩余时间里都是已知的。CRR 的目标
是让内核代码在**运行期间反复重新随机化**，使泄露出去的地址在短时间内就失效。

当前基线：**Linux 6.8**（见 [`Documentation/crr/development.md`](Documentation/crr/development.md)
中的「跟进上游版本」一节）。

## 实现思路

参与随机化的函数被拆成两半：

| 半边 | 段 | 地址 | 作用 |
| --- | --- | --- | --- |
| 蹦床（trampoline） | `.tramp.text.<fn>` | **固定** | 保留原函数名，所有既有调用者调用它 |
| 函数体（body） | `.rand.text.<fn>` | **可迁移** | 真正的代码，运行时可被复制到新的随机地址 |

调用者不需要修改：它照旧调用 `vfs_read()`，落到蹦床上，蹦床再按当前地址跳转到
`vfs_read_real()`。重新随机化时只需搬移函数体、改写蹦床里的跳转目标，不必回改内核镜像里
成千上万处调用点。

链接期还会生成两张**平行的指针表**（`.data..tramp_ptr_tbl` / `.data..rand_ptr_tbl`），
每个被标注的函数各占一项，且与代码的链接顺序一致。同步遍历这两张表即可把每个蹦床和它的
函数体配对，并用相邻项的地址差得出每个函数体的大小 —— 仅靠段边界是拿不到这些信息的。

## 代码导览

| 路径 | 内容 |
| --- | --- |
| `include/linux/rerand.h` | `TRAMP_FN` / `RAND_FN` 等标注宏，以及段边界符号声明 |
| `include/asm-generic/vmlinux.lds.h` | `TRAMP_TEXT` / `RAND_TEXT` / `*_TABLE_DATA` 段定义 |
| `arch/x86/kernel/vmlinux.lds.S` | 在 x86 内核镜像里安置上述段 |
| `arch/x86/Kconfig` | `CONFIG_CKASLR`、`CONFIG_CKASLR_DEBUG` |
| `kernel/rerand.c` | 核心：`late_initcall` 扫描指针表，构建 `func_array` |
| `kernel/rerand_utils.c` | 早期 out-of-tree `randmod` 原型，暂以注释保留作参考 |
| `fs/read_write.c` | 首批被标注的函数：`vfs_read/write`、`ksys_read/write` |
| `boot/` | QEMU 测试环境（构建 rootfs、启动内核、gdb 调试） |

**当前进度**：链接期基础设施与运行期*发现*逻辑已完成 —— `func_array` 能列出哪些函数可以
被搬移及其大小。**搬移与蹦床改写尚未实现**（`func_entry.new_addr` 预留未用）。

## 快速开始

```sh
# 1. 配置（首次）
make O=build x86_64_defconfig
./scripts/config --file build/.config --enable CKASLR
make O=build olddefconfig

# 2. 编译
make O=build -j"$(nproc)"

# 3. 制作 rootfs 镜像（首次，需要 sudo 与 debootstrap）
cd boot && ./create-image.sh

# 4. 启动
./boot.sh            # 直接运行
./boot-S.sh          # 停在复位处等待 gdb 连接（另开终端 gdb vmlinux -ex 'target remote :1234'）
```

`CONFIG_CKASLR=n` 时所有改动都被编译掉，内核与原版逐字节相同 —— 可以用它来做对照组。

## 开发流程

**动手改代码之前请先读 [`Documentation/crr/development.md`](Documentation/crr/development.md)。**
它规定了分支模型、提交信息规范、如何跟进上游版本，以及如何找回归档的历史工作。
