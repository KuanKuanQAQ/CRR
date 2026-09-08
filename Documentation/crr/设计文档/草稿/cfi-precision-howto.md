# CFI 精度基线测量手册

对应第 2 章的【待补 W-14】（`endbr64` 计数）与【待补 W-15】（等价类大小分布）。
两项测量共用同一个内核镜像，建议一次做完。产出同时回填三处：

| 产出 | 回填位置 |
| --- | --- |
| `endbr64` 总数与封印后剩余数 | 第 2.5.1 节；第 1.1.4 节"等价类规模以万计"一句 |
| 等价类大小分布与最大等价类 | 第 2.5.3 节；第 1.1.4 节【W-03】；第 5.6.2 节的对比基线 |

与 `gadget-census.md` 一样，这里的目标是**基线**：部署 I-KASLR 之后用同样的参数重跑，
两组数字对比才是第 5 章要的结果。

---

## 0. 准备：一个开启 IBT 与 kCFI 的内核

两项测量都需要内核在编译时开启相应选项。建议编译两个配置，用于交叉对比。

```sh
cd ~/linux            # 你的 6.16 树
mkdir -p build-cfi && cd build-cfi

make -C .. O=$PWD x86_64_defconfig
../scripts/config --file .config \
    --enable CONFIG_X86_KERNEL_IBT \
    --enable CONFIG_CFI_CLANG \
    --enable CONFIG_DEBUG_INFO \
    --disable CONFIG_DEBUG_INFO_REDUCED
make -C .. O=$PWD olddefconfig
make -C .. O=$PWD LLVM=1 -j"$(nproc)"
```

> `CONFIG_CFI_CLANG` 需要用 Clang 编译（`LLVM=1`），且要求 Clang 版本足够新。
> 若 `olddefconfig` 之后这两个选项没有留在 `.config` 里，说明依赖没满足，
> 用 `make menuconfig` 搜索 `IBT` 与 `CFI_CLANG` 看依赖提示。

确认两个选项确实生效：

```sh
grep -E "CONFIG_X86_KERNEL_IBT|CONFIG_CFI_CLANG" .config
```

**记录下来写进论文的信息**：内核版本（`make kernelrelease`）、编译器版本
（`clang --version`）、配置名（`x86_64_defconfig` + 上述两项）。

---

## 1. W-14：`endbr64` 的数量与 objtool 封印的效果

### 1.1 这个数字说明什么

Intel CET-IBT 只检查间接分支的目标处是否有一条 `endbr64`。因此**所有带
`endbr64` 的位置构成单一的等价类**：任何间接调用都可以合法地跳向其中任意一个。
这个等价类有多大，直接就是 vanilla IBT 的精度上限。

内核用 `objtool` 做了一次收缩：静态分析出不可能被间接调用的函数，把它们入口处的
`endbr64` 改写为 4 字节 `nop`（即"封印"）。因此要报两个数字：

- **封印前**：编译器一共插入了多少条 `endbr64`
- **封印后**：链接完成的 `vmlinux` 里还剩多少条

### 1.2 测量

封印后的数量（直接数 `vmlinux` 里的 `endbr64`）：

```sh
objdump -d --no-show-raw-insn vmlinux \
  | grep -c '\bendbr64\b'
```

被封印掉的数量（objtool 把它们换成了特定的 4 字节 nop，
x86 上是 `nopl 0x0(%rax)` 这一条 4 字节形式）：

```sh
# objtool 用的替换序列定义在 arch/x86/include/asm/ibt.h 的 ANNOTATE / 
# arch/x86/kernel/alternative.c 的 poison_endbr()，确认一下当前内核用的字节序列
grep -rn "poison_endbr\|ENDBR_INSN\|gen_endbr_poison" ../arch/x86/ | head
```

> **这一步需要你按 6.16 的实际实现核对。** `poison_endbr()` 写入的字节在不同版本
> 里有过变化，直接按字节序列 grep 才可靠。确认之后：
>
> ```sh
> objdump -d --no-show-raw-insn vmlinux | grep -c '<对应的 nop 形式>'
> ```

另一条路更省事，**推荐**：内核自己在启动日志里打了这个数。

```sh
dmesg | grep -i "CFI\|IBT\|sealed\|endbr"
```

x86 的 `apply_seal_endbr()` 会打印类似 `Sealed N endbr` 的信息。若日志里有，
直接引用这个数字，并注明来源是内核自身的统计，比自己数 objdump 更可信。

### 1.3 要填进论文的表

**表 2-x　Linux 6.16 上 IBT 的等价类规模**

| 项 | 数量 |
| --- | ---: |
| 编译器插入的 `endbr64` 总数 | |
| 经 objtool 封印（改写为 nop）的数量 | |
| `vmlinux` 中剩余的 `endbr64`（即 IBT 等价类大小） | |
| 内核中间接调用点的数量 | |

最后一行用于说明"一个调用点面对多少个合法目标"。测法见 2.2。

---

## 2. W-15：kCFI 的等价类大小分布

### 2.1 kCFI 是怎么划分等价类的

`CONFIG_CFI_CLANG` 为每个函数类型算一个 32 位哈希，调用点检查目标函数的类型哈希
是否匹配。因此**等价类 = 具有相同类型签名的函数集合**。要得到分布，需要两样东西：

1. 每个可被间接调用的函数的**类型签名**
2. 每个间接调用点期望的**类型签名**

有两条获取途径，建议都做，互相印证。

### 2.2 途径 A：从 DWARF 调试信息统计（推荐先做）

思路：`vmlinux` 里带完整的 DWARF，可以取出每个函数的原型，按原型分组即得等价类。

```sh
python3 - <<'EOF'
# 需要 pyelftools:  pip install pyelftools
from elftools.elf.elffile import ELFFile
from collections import Counter, defaultdict

def type_name(die, cu):
    """把 DW_AT_type 递归解析成一个可比较的类型字符串。"""
    if 'DW_AT_type' not in die.attributes:
        return 'void'
    ref = die.attributes['DW_AT_type'].value
    t = cu.get_DIE_from_refaddr(ref + cu.cu_offset)
    tag = t.tag
    if tag == 'DW_TAG_base_type':
        return t.attributes['DW_AT_name'].value.decode()
    if tag == 'DW_TAG_pointer_type':
        return type_name(t, cu) + '*'
    if tag in ('DW_TAG_const_type', 'DW_TAG_volatile_type'):
        return type_name(t, cu)
    if tag == 'DW_TAG_typedef':
        return type_name(t, cu)
    if tag in ('DW_TAG_structure_type', 'DW_TAG_union_type'):
        n = t.attributes.get('DW_AT_name')
        return ('struct ' + n.value.decode()) if n else 'struct <anon>'
    if tag == 'DW_TAG_enumeration_type':
        return 'enum'
    return tag

sig2fn = defaultdict(list)
with open('vmlinux','rb') as f:
    elf = ELFFile(f)
    dw = elf.get_dwarf_info()
    for cu in dw.iter_CUs():
        for die in cu.iter_DIEs():
            if die.tag != 'DW_TAG_subprogram':
                continue
            if 'DW_AT_low_pc' not in die.attributes:      # 只要有实体的函数
                continue
            name = die.attributes.get('DW_AT_name')
            if not name:
                continue
            ret = type_name(die, cu)
            args = []
            for ch in die.iter_children():
                if ch.tag == 'DW_TAG_formal_parameter':
                    args.append(type_name(ch, cu))
            sig = '%s (*)(%s)' % (ret, ', '.join(args) or 'void')
            sig2fn[sig].append(name.value.decode())

sizes = Counter(len(v) for v in sig2fn.values())
total_fn = sum(len(v) for v in sig2fn.values())
print('函数总数        %d' % total_fn)
print('不同类型签名数  %d' % len(sig2fn))
print('平均等价类大小  %.2f' % (total_fn/len(sig2fn)))
print()
print('等价类大小分布（大小: 该大小的类的个数）')
for k in sorted(sizes):
    print('  %6d : %d' % (k, sizes[k]))
print()
print('最大的 10 个等价类')
for sig, fns in sorted(sig2fn.items(), key=lambda kv: -len(kv[1]))[:10]:
    print('  %5d  %s' % (len(fns), sig[:100]))
EOF
```

**这段脚本产出的就是论文要的三个数字**：不同类型签名数、等价类大小分布、最大等价类。

> **注意一个口径问题。** 上面统计的是"内核中所有有实体的函数"，而 kCFI 的等价类
> 应当只包含**可能被间接调用**的函数。分母不同，数字会差不少。收紧的办法是只统计
> 地址被取过的函数——即出现在 `.data`/`.rodata` 中的函数指针目标。粗略做法：
>
> ```sh
> # 取出所有函数符号地址，再看哪些地址值出现在数据段里
> nm -n vmlinux | awk '$2 ~ /[tT]/ {print $1, $3}' > funcs.txt
> ```
>
> 然后扫描 `.data`/`.rodata` 的 8 字节对齐位置，匹配 `funcs.txt` 中的地址。
> 这一步比较费事，**建议先出途径 A 的粗口径数字，如果第 5 章的对比需要更严的分母
> 再补。** 正文里把口径写清楚即可。

### 2.3 途径 B：从 kCFI 的类型哈希直接统计

Clang 在启用 `-fsanitize=kcfi` 时，会在每个可间接调用函数的入口前放一个
4 字节的类型哈希（即 `__kcfi_typeid_*` 符号 / 前缀数据）。直接数这些哈希的分布，
得到的就是 kCFI 实际使用的等价类划分，比途径 A 的 DWARF 推断更贴近实现。

```sh
# 1) 取出所有 __kcfi_typeid_ 符号及其值（值就是类型哈希）
nm vmlinux | grep '__kcfi_typeid_' | awk '{print $1}' | sort | uniq -c | sort -rn > kcfi_hash_hist.txt

head -20 kcfi_hash_hist.txt      # 最大的等价类
wc -l kcfi_hash_hist.txt         # 等价类个数
awk '{s+=$1} END {print "受保护函数总数", s}' kcfi_hash_hist.txt
```

> **这一步的可行性要你先确认。** `__kcfi_typeid_` 符号是否出现在 `vmlinux` 的
> 符号表里，取决于链接时是否被丢弃。先跑 `nm vmlinux | grep -c __kcfi_typeid_`：
> 若为 0，改从 `.o` 文件里取（`nm built-in.a | grep __kcfi_typeid_`），
> 或直接反汇编函数入口前 4 字节。**这条途径若能走通，比途径 A 更有说服力，
> 因为它统计的就是硬件/编译器实际使用的划分。**

### 2.4 要填进论文的图和表

**图 2-x　kCFI 等价类大小的分布**
横轴为等价类大小（对数刻度），纵轴为该大小的等价类个数（对数刻度）。
典型形态是长尾：绝大多数类只有 1–2 个成员，少数通用原型聚集上千个函数。
**图上要用箭头标出最大等价类的位置和大小**，因为决定安全性的是它而不是平均值。

**表 2-x　三类硬件 CFI 机制的精度对比（Linux 6.16）**

| 机制 | 等价类划分依据 | 等价类个数 | 最大等价类 | 平均等价类 |
| --- | --- | ---: | ---: | ---: |
| 无 CFI | 无 | 1 | 全部间接可达函数 | |
| CET-IBT（封印后） | 有无 `endbr64` | 1 | 见 W-14 | |
| kCFI | 函数类型哈希 | | | |
| 本文机制 | 调用点唯一上下文 | | | |

最后一行由第 5 章填。**这张表是第 1.1.4 节、第 2.5 节、第 5.6.2 节共用的**，
做一次填三处。

---

## 3. 常见问题

**Q：一定要真的编译内核吗？能不能用发行版的 vmlinux？**
可以用发行版带调试符号的 `vmlinux`（如 Ubuntu 的 `linux-image-*-dbgsym`），
途径 A 能跑。但发行版内核未必开了 `CONFIG_CFI_CLANG`，途径 B 和 W-14 的
封印统计就做不了。**论文里要报的是"你自己的实验内核"上的数字，所以最终还是
要自己编译一份，配置与第 6 章的实验内核一致。**

**Q：ARM64 上怎么做？**
`endbr64` 对应 ARM 的 `BTI` 指令（`bti c` / `bti j` / `bti jc`），
用 `objdump -d vmlinux | grep -c '\bbti\b'` 统计。kCFI 在 arm64 上同样可用，
途径 A/B 不变。**第 5 章的主平台是 ARM，因此 ARM 上的这组数字才是第 5.6.2 节
真正需要的基线；x86 的数字用于第 2.5 节的横向说明。** 两边都做。

**Q：数字对不上怎么办？**
途径 A（DWARF 原型）和途径 B（kCFI 哈希）的结果会有出入，原因是 Clang 在计算
类型哈希时做了一些归一化（例如指针类型的合并）。**出入本身值得在正文里说一句**，
它正好说明"类型哈希"这一划分比"源码原型"更粗，从而进一步支持第 2.5.3 节的论断。

---

## 4. 记录模板

做完后把下面这段填好，直接可以贴进论文的实验配置小节。

```
测量日期      ：
内核版本      ：            （make kernelrelease）
编译器        ：            （clang --version）
配置          ：x86_64_defconfig + CONFIG_X86_KERNEL_IBT + CONFIG_CFI_CLANG
架构          ：x86-64 / arm64
vmlinux 大小  ：
.text 大小    ：

endbr64 总数（封印前）：
endbr64 剩余（封印后）：
kCFI 等价类个数        ：
kCFI 最大等价类        ：
kCFI 平均等价类        ：
统计口径               ：途径 A / 途径 B
```
