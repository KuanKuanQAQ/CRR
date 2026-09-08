# CRR 开发规范

本文规定 CRR 仓库的分支模型、提交规范和日常工作流。CRR 是一个 Linux 内核研究分支，
仓库里 99.9% 的代码来自上游，属于我们自己的改动只有几千行 —— 分支模型必须让这几千行
**始终清晰可见**，而不是淹没在上游历史里。

---

## 1. 为什么要有规范：旧布局的问题

改版之前仓库里有 6 条分支：

```
main                   纯净的 Linux 6.16-rc1，不含任何 CRR 代码
Linux-6.6              纯净的 v6.6
Linux-6.8              纯净的 v6.8
dev/watchpoint         基于 6.6，arm64 硬件观察点实验          （2024-09）
dev/load-text          基于 6.6，arm64 模块加载器改造          （2024-09）
dev/rand-path          基于 6.8，x86 CKASLR 段基础设施         （2025-09）
dev/watchpoint-module  基于 6.8，arm64 硬件断点驱动            （2025-10）
dev/rand-module        基于 6.8，模块加载调查                  （2025-11）
dev/suspend            基于 6.8，LLVM pass + ktm 搬移与挂起    （2025-11）
```

六个具体问题：

1. **主干上没有项目代码。** 别人（或半年后的你）克隆仓库，默认签出的 `main` 是一份
   原版内核，看不出这个项目在做什么。项目本身没有「所在之处」。
2. **各条线基于不同的内核版本。** 6.6 上的 arm64 工作和 6.8 上的工作永远无法合并、
   无法互相验证 —— 它们不共享任何一个可以作为公共祖先的基线。
3. **用分支来标记版本。** `Linux-6.6` / `Linux-6.8` 从创建之日起就再也不会移动。
   分支的语义是「一条还在推进的开发线」，不动的点应该用 **tag**。之所以当初用了分支，
   是因为克隆时没有带上游 tag（仓库里一个 tag 都没有）—— 这个根因已经修好了。
4. **同一份基础设施被复制了三遍。** `boot/` 在三条分支上各有一份，而且各自带着别人
   没有的修复：ext4 inode 数量的修复只在 x86 那份里，tap0 网卡、`mnt_path/.gitkeep`、
   正确的网卡名只在 arm64 那两份里。想换个架构启动就得手工来回改脚本。
5. **提交信息没有信息量。** `Tmp 9/18`、`Bug fixed`、`Print free`、`MASK & BAS`、
   `Everything is yet to be completed`。半年后没人知道某次提交改了什么、为什么改。
6. **本地看到的分支不等于远端的分支。** 本地克隆的 remote-tracking 引用长期没有更新，
   `git branch -a` 少列了三条分支 —— 其中就包括**最新的那条**。见第 3 节开头。

---

## 2. 新布局

```
        v6.8 (tag，上游基线)
          │
          ├─── boot 测试环境 ──── 阶段一 x86 CKASLR ──┬── merge ──► main
          │                                          │
          │                              topic/ktm-suspend（已并入）
          │
      main ──┬── topic/hw-breakpoint                  ← 短期特性分支，合并后删除
             └── topic/module-reloc

其他 tag：v6.6  v6.16-rc1                             ← 上游锚点，只读
          archive/watchpoint  archive/load-text       ← 已归档的历史工作
          backup/2026-09-08/*                         ← 本次整理前的原始状态
```

### `main` —— 唯一的主干

`main` = 上游基线 `v6.8` + 全部 CRR 代码。它**永远可编译**。
克隆下来直接就是完整的项目。

`main` 不接受直接提交。所有改动都先在 topic 分支上做，再合回来。

### `topic/<名字>` —— 短期特性分支

一条 topic 分支只做一件事，从 `main` 拉出，合并后**立即删除**。

命名用**内容**而不是状态：`topic/hw-breakpoint`、`topic/module-reloc`、
`topic/perf-eval` 都可以；`topic/tmp`、`topic/dev2`、`topic/new` 不行。

当前存活的 topic 分支：

| 分支 | 内容 |
| --- | --- |
| `topic/hw-breakpoint` | arm64 硬件断点驱动（~2600 行 + proc 接口 + 中文文档） |
| `topic/module-reloc` | 内核模块加载路径调查，目前只有 5 行 `pr_info` 探针 |

寿命目标是**几天到两周**。分支活得越久，与 `main` 的偏离越大，合并越痛苦。工作做不完
不是不合并的理由 —— 把已完成、可编译的部分先合进 `main`（功能由 Kconfig 挡着，半成品
不影响别人），剩下的开新分支继续。旧的 `dev/rand-path` 拖了一年、`boot/` 分叉成三份，
都是这么来的。

### tag —— 所有不移动的点

| tag | 含义 |
| --- | --- |
| `v6.6`、`v6.8`、`v6.16-rc1` | 上游版本锚点。需要对照原版内核时签出它们 |
| `archive/<名字>` | 已归档、不再推进的历史工作 |
| `backup/<日期>/<名字>` | 危险操作（改写历史、force push）前的现场存档 |

**规则：任何不会再移动的引用，一律用 tag，不要用分支。**

### `upstream` remote —— 上游内核

```sh
git fetch upstream --tags
```

只 fetch，永远不要往它 push（push URL 已被设成 `DISABLED_fetch_only`）。
上游代码通过 tag 进入仓库，不通过分支。

---

## 3. 日常开发流程

**每次开工先同步远端状态**，否则会像这次一样，本地根本看不到最新的分支：

```sh
git fetch --all --prune --tags
git branch -vv          # 确认本地和远端一致
```

`--prune` 会删掉远端已经不存在的 remote-tracking 引用，`--tags` 拉回归档和备份点。

```sh
# 1. 从最新的 main 拉一条 topic 分支
git checkout main && git pull
git checkout -b topic/relocate-in-tree

# 2. 干活，小步提交（见第 4 节的提交规范）
vim samples/kernel_trampoline_move/ktm_move.c
git add -p && git commit

# 3. 编译 + 启动验证（见第 5 节）
make O=build ARCH=arm64 -j"$(nproc)" && (cd boot && ./boot.sh)

# 4. 合回 main 之前，先把分支 rebase 到最新的 main 上
git fetch origin
git rebase origin/main
#    ——此时再编译一次，确认 rebase 之后仍然是好的

# 5. 合并（用 --no-ff，保留「这是一条特性分支」的形状）
git checkout main
git merge --no-ff topic/relocate-in-tree
git push origin main

# 6. 删掉 topic 分支
git branch -d topic/relocate-in-tree
git push origin --delete topic/relocate-in-tree   # 如果推过的话
```

**为什么第 4 步先 rebase 再合并**：这样 `main` 上每条特性都是一段连续、线性、
基于当时最新主干的提交，`git log --first-parent main` 读起来就是一份干净的
项目进展流水账。

**在合并进 `main` 之前**，topic 分支上的历史随便改（`rebase -i`、`commit --amend`
整理提交都鼓励）；**合并之后就不要再改写**了。

**公共基础设施要放在 `main` 上，不要放在 topic 分支上。** `boot/` 分叉成三份就是
反例：三条分支各自改进了启动脚本，谁都拿不到别人的修复。凡是不止一条分支会用到的
东西 —— 启动脚本、测量工具、文档 —— 先合进 `main`，再从 `main` 拉分支。

---

## 4. 提交规范

沿用内核社区的格式，因为这个仓库本来就是内核：

```
子系统: 一句话说清这次改了什么（祈使句，不超过 ~72 字符，句末不加句号）

空一行，然后是正文：说明**为什么**这样改，而不是复述 diff 改了哪几行
（改了什么看 diff 就知道了）。写清楚这次改动解决的问题、为什么选这个
方案、有什么已知的限制。换行控制在 72 字符左右。
```

子系统前缀用改动所在的位置，和内核惯例一致：

```
ktm:             ktm 模块（samples/kernel_trampoline_move）
ckaslr:          CKASLR 核心（kernel/rerand*.c）
x86/ckaslr:      x86 相关的 CKASLR 改动
arm64:           arm64 相关改动
kbuild:          构建系统（顶层 Makefile、LLVM pass 接入）
boot:            QEMU 测试环境
Documentation:   文档
```

好例子（本仓库现有提交）：

```
fs/read_write: split the read/write paths into trampoline and body
boot: unify the QEMU harness across architectures
kbuild: make the CRR LLVM pass path configurable
```

反例（整理前的旧提交）：

```
Tmp 9/18            ← 日期不是信息
Bug fixed           ← 修了哪个 bug？
Print free          ← 三个月后完全看不懂
MASK & BAS          ← 缩写没有上下文
Initial commit      ← 什么的初始提交？
```

**一次提交做一件事。** 「实现了搬移 + 顺手改了 create-image.sh 的 inode 数」应该是
两次提交。粒度的判断标准：这次提交能不能用一句话说清楚，且不需要用「以及」。

**每次提交都应该能编译。** 这样 `git bisect` 才能用 —— 在内核上调试问题时，
这一条的价值远超过它的成本。

---

## 5. 编译与验证

构建需要外部的 LLVM pass，见 [`README.md`](../../README.md) 的「外部依赖」一节。
没有它时用 `CRR_TRAMPOLINE=n`。

```sh
# arm64（主力平台）
make O=build ARCH=arm64 defconfig
make O=build ARCH=arm64 menuconfig      # 打开 samples 下的 ktm / log_time
make O=build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)"

# 启动
cd boot && ./boot.sh                    # 覆盖项见 boot/config.sh
```

所有构建产物都在 `build/`，rootfs 和磁盘镜像在 `boot/`，都已被 `.gitignore` 忽略。
**永远不要提交构建产物** —— 仓库的 `.git` 已经有 5.8 GB 了。

提交前的自检清单：

- [ ] 目标架构能编译、能启动
- [ ] `CRR_TRAMPOLINE=n` 能编译（对照组基线必须始终可用）
- [ ] 新增代码用 Kconfig 或 `#ifdef` 挡住，关掉之后内核与原版逐字节相同
- [ ] 没有把本机绝对路径写进构建系统（用 `CRR_PLUGIN_DIR` 那样的可覆盖变量）
- [ ] `git status` 干净，没有误提交的构建产物

---

## 6. 跟进上游版本

CRR 一次只钉在**一个**上游基线上（当前是 `v6.8`）。要迁到新版本时：

```sh
# 1. 拿到新的上游 tag
git fetch upstream --tags

# 2. 先存档当前状态（改写历史之前必做）
git tag backup/$(date +%F)/main main
git push origin --tags

# 3. 把 CRR 的提交搬到新基线上
git checkout -b topic/rebase-v6.12 main
git rebase --onto v6.12 v6.8

# 4. 逐个解决冲突。内核的链接脚本、构建系统和 fs/ 在版本间会变，
#    尤其注意 include/asm-generic/vmlinux.lds.h、顶层 Makefile 和 fs/read_write.c

# 5. 编译 + 启动验证，确认没问题后再让 main 指过来
make O=build ARCH=arm64 -j"$(nproc)" && (cd boot && ./boot.sh)
git checkout main && git merge --ff-only topic/rebase-v6.12
git push origin main --force-with-lease
```

**用 rebase 而不是 merge 上游**：CRR 的改动只有几千行，把它们重放到新基线上，
`main` 的历史始终是「干净的上游 + 我们的补丁」这种可读的形状。反过来把上游 merge
进来，会往历史里灌进几十万条上游提交，我们自己的工作就找不着了。

**force push 时用 `--force-with-lease`**，不要用 `--force`：前者在远端有你没见过的
提交时会拒绝推送，能挡住误覆盖。

---

## 7. 归档与找回历史

不再推进但不想丢的工作，打成 `archive/` tag 后把分支删掉：

```sh
git tag archive/<名字> <分支名>
git push origin --tags
git branch -d <分支名>
git push origin --delete <分支名>
```

已归档的工作（基于 v6.6 的 arm64 实验，2024-09）：

| tag | 内容 |
| --- | --- |
| `archive/watchpoint` | arm64 硬件观察点开销测量。用 watchpoint 监视蹦床区域，实测数据在 `samples/hw_breakpoint/watchpoint.c` 的注释里：命中监视区的读写比未监视区慢约 4200 倍，说明「用观察点拦截蹦床访问」这条路走不通。后来的 `topic/hw-breakpoint` 是这条线的重写版 |
| `archive/load-text` | arm64 模块加载器改造，把模块的 `.text` 单独加载到可迁移的内存区域（`kernel/module/main.c`、`arch/arm64/kernel/module.c`）—— 与 ktm 在 vmlinux 内的做法思路相同，做模块级随机化时值得回看 |

找回方式：

```sh
git log archive/load-text                       # 看历史
git checkout archive/load-text                  # 签出（进入 detached HEAD）
git checkout -b topic/xxx archive/load-text     # 在其基础上继续开发
git diff v6.6 archive/watchpoint                # 只看它改了什么
git cherry-pick <提交号>                         # 挑单个提交到当前分支
```

`backup/2026-09-08/*` 是本次仓库整理**之前**的原始状态（含原始的 `main` 和全部六条
`dev/*` 分支的原始提交）。确认新布局无误后可以删除：

```sh
git tag -d      backup/2026-09-08/main               # 本地
git push origin :refs/tags/backup/2026-09-08/main    # 远端
```

---

## 8. 不要做的事

| 不要 | 因为 |
| --- | --- |
| 直接往 `main` 提交 | 绕过了「先在 topic 分支上验证」这一步 |
| 用分支标记不再移动的点 | 那是 tag 的用途；分支列表应该只包含**在动**的东西 |
| 让 topic 分支活过一个月 | 偏离越大合并越痛苦，`dev/rand-path` 拖了一年就是例子 |
| 把共用的基础设施留在 topic 分支上 | `boot/` 分叉成三份，各自的修复谁也拿不到 |
| 把上游 merge 进 `main` | 会用几十万条上游提交淹没我们自己的工作，改用 rebase |
| 把本机绝对路径写进构建系统 | 别的机器上（包括新克隆）根本编不了 |
| 提交构建产物 | `.git` 已经 5.8 GB |
| 写 `Tmp`、`WIP`、`Bug fixed` 这类提交信息 | 半年后的你读不懂 |
| `git push --force` | 用 `--force-with-lease`，它能挡住误覆盖 |
| 改写已经合进 `main` 的历史 | 已经推出去的历史是共享状态 |
| 长期不 `git fetch --prune` | 本地会看不到远端的新分支，这次就漏了三条 |

---

## 9. 命令速查

```sh
# 我现在在哪、有哪些分支（先同步！）
git fetch --all --prune --tags
git branch -vv
git log --oneline --graph --first-parent main -20   # 只看主干进展

# CRR 到底改了上游内核的哪些地方 —— 最常用的一条
git diff v6.8 main --stat
git diff v6.8 main -- samples/                       # 只看某个子系统

# 某一行是什么时候、为什么变成这样的
git log -p --follow samples/kernel_trampoline_move/ktm_move.c
git blame samples/kernel_trampoline_move/ktm_move.c

# 找出引入某个问题的提交
git bisect start && git bisect bad main && git bisect good v6.8

# 清理已经合并掉的本地分支
git branch --merged main | grep -v ' main$' | xargs -r git branch -d

# 整理还没合并的 topic 分支上的提交
git rebase -i main
```
