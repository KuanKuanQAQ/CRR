#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Linux 内核 CVE 的「可模块化性」统计（论文 §1.2.2、表 1-2、附录 B）。

回答的问题：有多少 CVE 涉及**在任何内核配置下都无法编译为模块**的代码？
这些漏洞完全落在只保护模块/驱动的部分随机化方案（Adelie、Chaos、Dbox）之外，
不是"配置得不巧"，而是原理上不可能被覆盖。

方法（与 §1.2.2 的四步一致）：

 1. 从内核社区的 vulns 仓库读取指定年份的已发布 CVE，取其 programFiles 字段；
 2. **闸门链求解**：从文件所在目录出发逐级向上，求出控制它被编译进内核的一连串
    赋值。`obj-y` 判为无条件内建；`obj-$(CONFIG_X)` 则查 Kconfig 中 X 的类型，
    bool 判为只能内建、tristate 判为可模块化。只要链中任何一级是 tristate，
    该文件就有可能成为模块；
 3. **收敛性要求**：闸门链必须从文件所在目录一路解析到源码树顶层，中间任何一级
    无法解析即记为「未判定」，不作推断——宁可少判出"只能内建"，也不冒过判的风险；
 4. 归并到 CVE：只要有任一受影响文件被判为"只能内建"，该 CVE 即计入该类。

用法:
    cve_builtin_stats.py --vulns <vulns仓库> --ktree <内核树> [--years 2024 2025 2026]
                         [--json out.json] [--per-cve out.csv]

`--ktree` 可以是完整内核树，也可以是只含 Makefile/Kconfig/Kbuild 的轻量树
（后者足够，且省去检出整棵树）。
"""
import argparse, json, os, re, sys, glob
from collections import Counter, defaultdict

# ---------------- Kconfig：符号类型 ----------------

CONFIG_RE = re.compile(r'^\s*(menuconfig|config)\s+(\w+)')
TYPE_RE = re.compile(r'^\s*(bool|tristate|int|hex|string|def_bool|def_tristate)\b')

def load_kconfig_types(ktree):
    """返回 {符号名: 类型}。类型取 bool / tristate / 其它。"""
    types = {}
    for path in glob.glob(os.path.join(ktree, '**', 'Kconfig*'), recursive=True):
        if not os.path.isfile(path):
            continue
        cur = None
        try:
            with open(path, errors='replace') as f:
                for line in f:
                    m = CONFIG_RE.match(line)
                    if m:
                        cur = m.group(2)
                        continue
                    if cur:
                        t = TYPE_RE.match(line)
                        if t:
                            k = t.group(1).replace('def_', '')
                            # 同名符号可在多处声明；tristate 优先（更宽松的一侧）
                            if types.get(cur) != 'tristate':
                                types[cur] = k
                            cur = None
                        elif line.strip() and not line.startswith((' ', '\t')):
                            cur = None
        except OSError:
            pass
    return types

# ---------------- Makefile：闸门 ----------------

# obj-y += a.o   /   obj-$(CONFIG_X) += a.o sub/   /   foo-y += a.o
ASSIGN_RE = re.compile(
    r'^\s*([\w.$()-]+?)-(objs|objects|y|m|n|\$\((CONFIG_\w+)\))\s*[:+]?=\s*(.*)$')

def parse_makefile(path):
    """
    返回 (gates, composite)：
      gates[target] = 条件列表；target 可以是 'foo.o' 或 'subdir/'
      composite[obj] = 复合目标名（obj 属于 <name>.o）
    条件为 'y' / 'm' / 'CONFIG_X'。
    """
    gates = defaultdict(list)
    composite = {}
    try:
        with open(path, errors='replace') as f:
            text = f.read()
    except OSError:
        return gates, composite
    text = text.replace('\\\n', ' ')          # 续行
    for line in text.splitlines():
        line = line.split('#')[0]
        m = ASSIGN_RE.match(line)
        if not m:
            continue
        lhs, suffix, cfg, rhs = m.group(1), m.group(2), m.group(3), m.group(4)
        # foo-objs/foo-objects 是复合目标的成员列表，不是构建条件
        cond = cfg if cfg else suffix
        items = [t for t in rhs.split() if t.endswith('.o') or t.endswith('/')]
        if not items:
            continue
        if suffix in ('objs', 'objects'):
            # foo-objs := a.o b.o  =>  a.o、b.o 都属于 foo.o
            for it in items:
                if it.endswith('.o'):
                    composite[it] = lhs + '.o'
        elif lhs == 'obj':
            for it in items:
                gates[it].append(cond)
        elif lhs in ('lib', 'core', 'drivers', 'libs', 'net', 'virt', 'arch'):
            # 顶层与少数目录用这些变量聚合子目录
            for it in items:
                gates[it].append(cond)
        else:
            # 复合目标：foo-y += a.o  =>  a.o 属于 foo.o
            if lhs.endswith('-objs') or lhs.endswith('-y'):
                lhs = lhs.rsplit('-', 1)[0]
            for it in items:
                if it.endswith('.o'):
                    composite[it] = lhs + '.o'
    return gates, composite

class Tree:
    def __init__(self, ktree):
        self.ktree = ktree
        self._cache = {}

    def dirinfo(self, d):
        if d in self._cache:
            return self._cache[d]
        gates, comp = defaultdict(list), {}
        for name in ('Makefile', 'Kbuild'):
            p = os.path.join(self.ktree, d, name) if d else os.path.join(self.ktree, name)
            if os.path.isfile(p):
                g, c = parse_makefile(p)
                for k, v in g.items():
                    gates[k].extend(v)
                comp.update(c)
        self._cache[d] = (gates, comp)
        return gates, comp

    def obj_conds(self, d, obj, depth=0):
        """求出目录 d 内目标 obj 的控制条件；None 表示无法解析。"""
        if depth > 8:
            return None
        gates, comp = self.dirinfo(d)
        if obj in gates:
            return list(gates[obj])
        if obj in comp:                       # 复合目标：转而求父目标
            return self.obj_conds(d, comp[obj], depth + 1)
        return None

    def dir_step(self, d):
        """
        求目录 d 被纳入构建的条件，返回 (条件列表, 上一级目录)；None 表示解析失败。

        注意目录不一定以单段名字出现在**直接**父目录里：drivers/gpu/drm/Makefile
        写的是 `obj-$(CONFIG_DRM_AMDGPU) += amd/amdgpu/`，即多级相对路径。
        因此逐级向上尝试「从祖先 A 到 d 的相对路径」这个键。
        """
        # arch/<a> 由顶层 Makefile 经 arch/$(SRCARCH)/ 间接包含，源码里没有
        # 字面量 "arch/x86/" 可匹配。为目标架构构建时该目录必然被编入，
        # 因此这一跳记为无条件内建（'y'）。
        parts = d.split('/')
        if len(parts) <= 2 and parts[0] == 'arch':
            return ['y'], ''
        for up in range(1, len(parts) + 1):
            parent = '/'.join(parts[:len(parts) - up])
            key = '/'.join(parts[len(parts) - up:]) + '/'
            gates, _ = self.dirinfo(parent)
            if key in gates:
                return list(gates[key]), parent
        return None

    def chain(self, srcfile):
        """
        求文件的完整闸门链。返回 (conds, ok)。
        ok=False 表示链中有一级无法解析（记为未判定）。
        """
        # 头文件没有对应的目标文件，本方法在原理上无法判定它的构建方式
        # （它被全树多处包含）。如实记为未判定，不作推断。
        if srcfile.startswith('include/') or srcfile.endswith(('.h', '.rst', '.txt')):
            return [], False
        d = os.path.dirname(srcfile)
        obj = os.path.basename(srcfile)
        obj = re.sub(r'\.[cS]$', '.o', obj)
        conds = []

        c = self.obj_conds(d, obj)
        if c is None:
            return conds, False
        conds += c

        # 逐级向上直到顶层
        seen = set()
        while d:
            if d in seen:                 # 防御异常的 Makefile 结构
                return conds, False
            seen.add(d)
            step = self.dir_step(d)
            if step is None:
                return conds, False
            c, d = step
            conds += c
        return conds, True

def classify(conds, ok, types):
    """'builtin' / 'modular' / 'undetermined'"""
    if not ok:
        return 'undetermined'
    for c in conds:
        if c == 'm':
            return 'modular'
        if c == 'y':
            continue
        t = types.get(c[len('CONFIG_'):] if c.startswith('CONFIG_') else c)
        if t is None:
            return 'undetermined'         # 符号类型未知：不推断
        if t == 'tristate':
            return 'modular'
        if t != 'bool':
            return 'undetermined'
    return 'builtin'

# ---------------- 主流程 ----------------

def cve_files(path):
    try:
        d = json.load(open(path, errors='replace'))
    except Exception:
        return []
    out = set()
    for a in d.get('containers', {}).get('cna', {}).get('affected', []):
        for f in a.get('programFiles', []) or []:
            out.add(f)
    return sorted(out)

def main():
    ap = argparse.ArgumentParser(description="内核 CVE 可模块化性统计")
    ap.add_argument('--vulns', required=True, help='vulns 仓库路径')
    ap.add_argument('--ktree', required=True, help='内核树（可只含 Makefile/Kconfig）')
    ap.add_argument('--years', nargs='+', default=['2024', '2025', '2026'])
    ap.add_argument('--json')
    ap.add_argument('--per-cve')
    ap.add_argument('--by-subsystem', action='store_true',
                    help='另外输出按子系统的分布（§1.2.2 末段、W-02 的数据）')
    a = ap.parse_args()

    print(f"# 读取 Kconfig 符号类型 …", file=sys.stderr)
    types = load_kconfig_types(a.ktree)
    print(f"#   {len(types):,} 个符号", file=sys.stderr)
    tree = Tree(a.ktree)

    per_year = {}
    rows = []
    subsys = Counter()
    for y in a.years:
        files = sorted(glob.glob(os.path.join(a.vulns, 'cve', 'published', y, '*.json')))
        cnt = Counter()
        for jf in files:
            cve = os.path.basename(jf)[:-5]
            verdicts = []
            for src in cve_files(jf):
                conds, ok = tree.chain(src)
                verdicts.append(classify(conds, ok, types))
            if not verdicts:
                v = 'undetermined'
            elif 'builtin' in verdicts:
                v = 'builtin'
            elif 'modular' in verdicts:
                v = 'modular'
            else:
                v = 'undetermined'
            cnt[v] += 1
            rows.append((cve, y, v))
            if v == 'builtin':
                for src in cve_files(jf):
                    conds, ok = tree.chain(src)
                    if classify(conds, ok, types) != 'builtin':
                        continue
                    parts = src.split('/')
                    sub = '/'.join(parts[:2]) if parts[0] in (
                        'drivers', 'net', 'arch', 'fs') else parts[0]
                    subsys[sub] += 1
                    break        # 每个 CVE 只记一次，避免重复计数
        cnt['total'] = len(files)
        per_year[y] = dict(cnt)
        print(f"#   {y}: {len(files)} CVE", file=sys.stderr)

    tot = Counter()
    for y, c in per_year.items():
        for k, v in c.items():
            tot[k] += v

    def pct(n, d):
        return f"{n}（{100.0*n/d:.1f}%）" if d else "-"

    print("\n**表 1-2　Linux 内核 CVE 所涉代码的可模块化性**\n")
    print("| 编号年份 | CVE 总数 | 只能内建 | 可模块化 | 未判定 |")
    print("| --- | ---: | ---: | ---: | ---: |")
    for y in a.years:
        c = per_year[y]
        t = c['total']
        print(f"| {y} | {t} | {pct(c.get('builtin',0),t)} | "
              f"{pct(c.get('modular',0),t)} | {pct(c.get('undetermined',0),t)} |")
    t = tot['total']
    print(f"| 合计 | {t} | {pct(tot['builtin'],t)} | "
          f"{pct(tot['modular'],t)} | {pct(tot['undetermined'],t)} |")

    decided = tot['builtin'] + tot['modular']
    if decided:
        print(f"\n只在已判定的 {decided} 个 CVE 中计算，只能内建占 "
              f"{100.0*tot['builtin']/decided:.1f}%")

    if a.by_subsystem:
        print("\n按子系统看「只能内建」的 CVE 分布（每个 CVE 只计入其首个只能内建的文件）")
        for k, v in subsys.most_common(15):
            print(f"  {v:>5}  {k}/")

    if a.per_cve:
        with open(a.per_cve, 'w') as f:
            f.write("cve,year,verdict\n")
            for r in rows:
                f.write(",".join(r) + "\n")
        print(f"\n逐条结果 -> {a.per_cve}")
    if a.json:
        json.dump(dict(per_year=per_year, total=dict(tot)),
                  open(a.json, 'w'), ensure_ascii=False, indent=2)
        print(f"汇总 -> {a.json}")

if __name__ == '__main__':
    sys.exit(main())
