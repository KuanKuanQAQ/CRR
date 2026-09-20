# CRR — Continuous Kernel Re-Randomization

CRR 是一个 Linux 内核研究分支，实现**连续内核地址空间布局随机化**（continuous KASLR，
代码中记为 `CKASLR`）。标准 KASLR 只在启动时随机化一次内核镜像的加载基址，一旦攻击者
通过信息泄露拿到任意一个内核地址，整个布局在这次启动的剩余时间里都是已知的。CRR 的目标
是让内核代码在**运行期间反复重新随机化**，使泄露出去的地址在短时间内就失效。

上游基线：**Linux 6.8**。主力平台：**arm64**。

## 两个阶段

项目先后走过两条路线，两者的代码都在 `main` 上。

### 阶段一：手工标注的可迁移段（x86，2025-09）

参与随机化的函数被手工拆成两半：`.tramp.text.<fn>` 里是**地址固定**的蹦床，保留原函数名，
所有既有调用者继续调用它；`.rand.text.<fn>` 里是**可迁移**的真正函数体。链接期还会生成
两张平行的指针表（`.data..tramp_ptr_tbl` / `.data..rand_ptr_tbl`），每个被标注的函数各占
一项且顺序一致，同步遍历即可把蹦床与函数体配对，并用相邻项的地址差算出每个函数体的大小。

由 `CONFIG_CKASLR` 开关控制。这条路线**只做到了「发现」**：`kernel/rerand.c` 能列出哪些
函数可以被搬移及其大小，但没有实现搬移本身。它的根本局限是**每个函数都要手工标注**，
`fs/read_write.c` 里那四个函数就是全部成果。

### 阶段二：LLVM pass 自动注入 + ktm 运行时（arm64，2025-11，当前方向）

改由一个**外部 LLVM pass**（`libInjectTrampoline.so`）在编译期自动为函数注入蹦床，
整个内核都能参与，不需要逐个改调用点。

运行时那一半是 `samples/kernel_trampoline_move`（ktm），它真正完成了阶段一没做到的搬移：

1. 把活着的函数复制到新分配的可执行映射；
2. 用 `UDF` 指令覆盖原位置；
3. 仍在旧副本里执行的线程会触发异常，落到 die notifier 里被**挂起**；
4. 搬移完成后，线程按相同偏移在新副本里恢复执行。

`samples/log_time` 用来测量开销。`set_memory_*`、`module_alloc`、`__vmalloc_node_range`
之所以被导出，是因为模块需要它们来分配目标映射并重设页保护属性。

早期启动代码、KVM nVHE hypervisor、EFI stub 运行在内核映射之外或之前，**不能**被注入
蹦床，所以那几个目录用 `ccflags-remove-y` 把 pass 摘掉了。

## 代码导览

| 路径 | 内容 |
| --- | --- |
| `samples/kernel_trampoline_move/` | **ktm**：搬移 + 异常挂起，当前工作重点 |
| `samples/log_time/` | 开销测量 |
| `Makefile` | `CRR_PLUGIN_DIR` / `CRR_TRAMPOLINE`，接入 LLVM pass |
| `arch/arm64/mm/pageattr.c`、`mm/vmalloc.c`、`kernel/module/main.c` | 为 ktm 导出的符号 |
| `include/linux/rerand.h` | 阶段一：`TRAMP_FN` / `RAND_FN` 标注宏 |
| `include/asm-generic/vmlinux.lds.h`、`arch/x86/kernel/vmlinux.lds.S` | 阶段一：段与指针表定义 |
| `kernel/rerand.c` | 阶段一：`late_initcall` 扫描指针表 |
| `fs/read_write.c` | 阶段一：仅有的四个被标注的函数 |
| `boot/` | QEMU 测试环境（arm64 / x86_64 通用） |

## 外部依赖：LLVM pass

**这是唯一一个仓库之外的构建依赖，且不在本仓库里。** 需要
[llvm-tutor](https://github.com/banach-space/llvm-tutor) 的构建产物提供两个 pass：

| pass | 作用 |
| --- | --- |
| `libInjectTrampoline.so` | 编译期为函数注入蹦床（**必需**） |
| `libFuncTimer.so` | 函数计时，供 `log_time` 使用（可选） |

`CRR_PLUGIN_DIR` 默认指向 `/home/lirk/llvm-tutor/build/lib`；路径不同时用
`make CRR_PLUGIN_DIR=...` 覆盖。找不到 pass 时构建会直接报错并说明怎么办，而不是
在 clang 里抛出一个看不懂的加载失败。

不想装 pass 时用 `CRR_TRAMPOLINE=n` 构建 —— 这也正是对照组基线需要的。

## 快速开始

```sh
# 1. 配置
make O=build ARCH=arm64 defconfig
make O=build ARCH=arm64 menuconfig     # 打开 samples 下的 ktm / log_time

# 2. 编译
make O=build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)"
#    没有 LLVM pass 时：追加 CRR_TRAMPOLINE=n

# 3. 制作 rootfs 镜像（首次，需要 sudo 与 debootstrap）
cd boot && ./create-image.sh

# 4. 启动
./boot.sh                  # arm64（默认）
ARCH=x86_64 ./boot.sh      # x86_64
./boot-S.sh                # 停在复位处等 gdb：gdb build/vmlinux -ex 'target remote :1234'
```

`boot/config.sh` 里的每一项都能用环境变量覆盖（`ARCH`、`SMP`、`MEM`、`KASLR`、`NET`、
`KVM`、`BUILD`、`DRIVE`）。在 aarch64 宿主机上用 `KVM=1` 会快很多。

阶段一（x86）的构建方式：

```sh
make O=build x86_64_defconfig
./scripts/config --file build/.config --enable CKASLR
make O=build olddefconfig && make O=build CRR_TRAMPOLINE=n -j"$(nproc)"
```

## 当前进度

- [x] 阶段一：链接期段基础设施、指针表、运行期*发现*（x86）
- [x] LLVM pass 编译期注入蹦床（arm64）
- [x] ktm：函数搬移、UDF 异常挂起与恢复
- [x] 开销测量（`log_time`）
- [ ] 把 ktm 从 sample 模块提升为内核内的常态机制
- [ ] 周期性自动重随机化（而非手工通过 procfs 触发）
- [ ] 内核模块的重随机化（见 `topic/module-reloc`）

## 开发流程

**动手改代码之前请先读 [`Documentation/crr/development.md`](Documentation/crr/development.md)。**
它规定了分支模型、提交信息规范、如何跟进上游版本，以及如何找回归档的历史工作。
