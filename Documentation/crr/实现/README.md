# I-KASLR 实现：工作约定与续跑指南

本目录是 I-KASLR 实现任务的工作区。核心是 [`PROGRESS.md`](./PROGRESS.md) 进度账本。
本文件规定**如何在可能随时中断（如用量耗尽）的前提下推进这项工作**。

## 为什么这样组织

整个 I-KASLR（编译器 pass + 内核运行时 + 两架构检测 + PA CFI + 12 组系统实验）
的工作量远超单次会话，其中相当一部分还依赖特定硬件（ARM PAuth/观察点、x86 EPT）
与长周期实验（LMBench/SPEC/真实 CVE 链）。因此任务被设计成**断点续跑**的：
状态落在磁盘上，谁来重启都能接。

## 三条硬约定

1. **每步一提交。** `PROGRESS.md` 里的每个步骤（S x.y）对应一次 git 提交，提交信息
   以 `ikaslr(Sx.y): ...` 开头，正文说清这一步做了什么、验证到什么程度。提交即检查点。

2. **每步更新账本。** 完成或推进一步后，立刻更新 `PROGRESS.md`：改状态图例、填提交、
   **改写「下一步（续跑指针）」**、在变更日志追加一行。账本必须始终反映真实进度。

3. **可编译不变量。** 每次提交后内核在 `CONFIG_IKASLR=n` 下必须能编译（对照基线不破坏）；
   `CONFIG_IKASLR=y` 在已实现的部分内能编译。半成品用 `#ifdef`/桩挡住，不阻塞 `git bisect`。

## 如何续跑（用量重置后 / 新会话）

```
1. git checkout topic/ikaslr && git log --oneline -5     # 看最近检查点
2. 读 Documentation/crr/实现/PROGRESS.md 的「下一步（续跑指针）」
3. 从该步骤继续；完成后按上面三条约定提交并更新账本
```

对 Claude 会话：直接说「按 PROGRESS.md 继续」即可。

## 状态验证等级（对应论文第1.5节的证据层级）

- `[q] QEMU 验证通过`：在 QEMU(+KVM) 上能加载/运行、功能正确。
- `[H] 待真机验证`：逻辑完成，但依赖真机硬件特性（PAuth/观察点/EPT），本机无法终验。
- 性能数字一律标注测量环境（QEMU+KVM / 真机），不混用。

## 目录

- `PROGRESS.md`　主控账本
- **`17-implementation-current.md`　当前实现全貌 —— 逐条对照代码核对,含
  「已实现 vs 设计要求但未实现」的诚实清单。想了解实现,先读这一份。**
- `01-overview.md`　实现总览与代码地图
- `03-randomization.md`（**运行时部分已被 17 取代**）/ `04-detection.md` / `05-pa-cfi.md`　各章实现细节
- `07-compiler.md`　编译期（**「还未实现」一节部分过时,以 17 §1 为准**）
- `06-experiments.md`　实验代码使用说明
- **`实验/`　每个实验一个文档**（目的/配置/步骤/原始数据/判据/发现），
  索引见 [`实验/README.md`](实验/README.md)
- `10-perf-plan.md`　裸机性能测试方案
- `11-related-comparison.md`　与 Adelie/Dbox 等的对比数据与方法
- `12-got-codemodel-results.md`　共享 GOT 与代码模型（T1–T5）
- **`18-不可随机化清单.md`　哪些内核函数不能被随机化的完整清单**(逐类给出全名单,
  并把"已解决"与"仍不行"分开;全树 9.6%,且**全部来自 per-CPU**)
- `14-code-model-and-unmovable.md`　代码模型实现设计 + **哪些内核代码不能随机化**（实测）
- `15-scope-selection.md`　**随机化对象怎么选、为什么**（名单在 `scripts/ikaslr/funcs/`）
- `16-position-independence.md`　**函数体位置无关改造怎么做通的**（x86/arm64 全过程、
  两条架构级不相容、ORC 与 RCU 两个坑）
- 代码落点：`kernel/ikaslr/`（运行时）、`scripts/ikaslr/`（实验脚本）、`tools/ikaslr/`（LLVM pass 桩与接口）
