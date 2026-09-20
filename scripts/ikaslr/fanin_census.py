#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
高扇入函数普查与"本文机制"的等价类（论文 §5.4.2、§5.6.2、§5.6.4；实验 E5-5）。

两路输入，分别回答"实际发生了什么"与"静态上可能发生什么"：

  动态  CONFIG_FANIN_TRACE 内核在负载下记录的 (间接调用点, 目标) 对，
        即 /sys/kernel/debug/fanin_pairs 的快照（每阶段一份）。
  静态  FaninStaticPass 插件对每个编译单元输出的 TSV（CS / AT 两类记录）。

口径（与第 5 章归属模型一致）
---------------------------
  扇入(F)       触及函数 F 的**不同间接调用点**个数。
  高扇入        扇入 ≥ 2（归属冲突即判定，§5.5.3：阈值语义上固定为 2）。
  单归属        扇入 = 1；其唯一调用点即归属调用点 owner(F)。
  本文机制的等价类
                调用点 CS 的上下文 = HASH(CS)；以它签名的函数 = { F : owner(F)=CS }。
                等价类大小 = 该集合的大小。高扇入函数移入随机化区域、不在其中。
  上下文传播基线（纯 PA，无随机化）
                调用点—函数二分图的连通分量：共享任一目标的调用点必须共享上下文，
                分量内的函数构成一个等价类（§5.2.1）。

用法:
    fanin_census.py --vmlinux V --snap P0.txt [--snap P1.txt ...]
                    [--static-dir DIR --static-vmlinux V2] --json out.json
"""
import argparse, bisect, collections, json, os, re, subprocess, sys

ADDR2LINE = "/usr/lib/llvm-14/bin/llvm-addr2line"


def norm_path(p):
    p = re.sub(r"^.*?fanin-src/", "", p)
    return p[2:] if p.startswith("./") else p


# ---------------------------------------------------------------- symbols
class Syms:
    def __init__(self, vmlinux):
        self.addrs, self.names = [], []
        out = subprocess.check_output(["nm", "-n", "--defined-only", vmlinux],
                                      text=True, errors="replace")
        for line in out.splitlines():
            a, t, n = line.split()[:3]
            if t in "tTwW":
                self.addrs.append(int(a, 16))
                self.names.append(n)
        self.exact = {}
        for a, n in zip(self.addrs, self.names):
            self.exact.setdefault(a, n)
        self.vmlinux = vmlinux
        sym = {n: a for a, n in zip(self.addrs, self.names)}
        allsym = {}
        for line in out.splitlines():
            f = line.split()
            if len(f) >= 3:
                allsym[f[2]] = int(f[0], 16)
        self.init_lo, self.init_hi = allsym.get("_sinittext", 0), allsym.get("_einittext", 0)

    def is_init(self, a):
        return self.init_lo <= a < self.init_hi

    def at(self, a):
        """函数入口地址 -> 名字；不在入口上时给 名字+偏移。"""
        if a in self.exact:
            return self.exact[a]
        i = bisect.bisect_right(self.addrs, a) - 1
        return f"{self.names[i]}+{a - self.addrs[i]:#x}" if i >= 0 else hex(a)

    def containing(self, a):
        i = bisect.bisect_right(self.addrs, a) - 1
        return self.names[i] if i >= 0 else "?"

    def lines(self, addrs):
        """调用点地址 -> file:line。钩子的返回地址落在间接调用指令上，取 -1 回到钩子
        调用本身的位置，二者同属一条源码语句。"""
        if not addrs:
            return {}
        inp = "".join(f"{a - 1:#x}\n" for a in addrs)
        out = subprocess.run([ADDR2LINE, "-e", self.vmlinux], input=inp,
                             capture_output=True, text=True).stdout.splitlines()
        return {a: norm_path(l) for a, l in zip(addrs, out)}


# ---------------------------------------------------------------- dynamic
def load_snap(path):
    pairs, ext, hdr = [], 0, ""
    for line in open(path):
        if line.startswith("#"):
            hdr = line.strip()
            continue
        cs, tgt, ts = line.split()
        if tgt == "ext":
            ext += 1
            continue
        pairs.append((int(cs, 16), int(tgt, 16), int(ts)))
    return pairs, ext, hdr


def components(edges):
    """二分图（调用点, 函数）连通分量；返回 [(函数数, 调用点数, 代表函数)]。"""
    parent = {}

    def find(x):
        while parent.setdefault(x, x) != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for cs, f in edges:
        a, b = find(("c", cs)), find(("f", f))
        if a != b:
            parent[a] = b
    comp = collections.defaultdict(lambda: [0, 0, None])
    for node in list(parent):
        r = comp[find(node)]
        if node[0] == "f":
            r[0] += 1
            r[2] = r[2] or node[1]
        else:
            r[1] += 1
    return sorted((tuple(v) for v in comp.values()), reverse=True)


def histo(values, bounds=(1, 2, 3, 5, 10, 20, 50, 100)):
    out, lo = [], 0
    for b in bounds:
        out.append((f"{lo + 1}-{b}" if b - lo > 1 else f"{b}",
                    sum(1 for v in values if lo < v <= b)))
        lo = b
    out.append((f">{lo}", sum(1 for v in values if v > lo)))
    return out


def analyse_dynamic(pairs, syms):
    fan = collections.defaultdict(set)      # F -> {CS}
    tgts = collections.defaultdict(set)     # CS -> {F}
    for cs, f, _ in pairs:
        fan[f].add(cs)
        tgts[cs].add(f)
    hf = {f for f, s in fan.items() if len(s) >= 2}
    owned = collections.Counter()
    for f, s in fan.items():
        if len(s) == 1:
            owned[next(iter(s))] += 1
    classes = [owned.get(cs, 0) for cs in tgts]          # 含 0：全部目标都已高扇入
    comps = components([(cs, f) for cs, f, _ in pairs])

    # 收敛：按首次出现时间回放，记录每个函数成为高扇入的时刻
    seen = collections.defaultdict(set)
    became = []
    for cs, f, ts in sorted(pairs, key=lambda p: p[2]):
        s = seen[f]
        if cs not in s:
            s.add(cs)
            if len(s) == 2:
                became.append(ts)
    return dict(fan=fan, tgts=tgts, hf=hf, owned=owned, classes=classes,
                comps=comps, became=became)


# ---------------------------------------------------------------- static
def load_static(d, text_names):
    cs, at_type, at_uses = [], {}, collections.defaultdict(set)
    for fn in os.listdir(d):
        for line in open(os.path.join(d, fn), errors="replace"):
            r = line.rstrip("\n").split("\t")
            if r[0] == "CS":
                cs.append(dict(tu=norm_path(r[1]), caller=r[2], loc=norm_path(r[3]),
                               type=r[4], field=None if r[5] == "-" else r[5]))
            elif r[0] == "AT":
                sym, name, ty, use = r[1], r[2], r[3], r[4]
                if name not in text_names:        # 声明了但未链接进映像
                    continue
                at_type[sym] = (name, ty)
                at_uses[sym].add(use)
    return cs, at_type, at_uses


def analyse_static(cs, at_type, at_uses):
    by_type = collections.defaultdict(set)
    by_field = collections.defaultdict(set)
    esc_by_type = collections.defaultdict(set)
    for sym, (name, ty) in at_type.items():
        by_type[ty].add(sym)
        for u in at_uses[sym]:
            if u == "ESC":
                esc_by_type[ty].add(sym)
            else:
                by_field[u].add(sym)

    res = {}
    for mode in ("type", "field"):
        fan = collections.Counter()
        owner = {}
        per_cs = []
        for i, c in enumerate(cs):
            if mode == "field" and c["field"]:
                t = by_field.get(c["field"], set()) | esc_by_type.get(c["type"], set())
            else:
                t = by_type.get(c["type"], set())
            per_cs.append(t)
            for f in t:
                fan[f] += 1
                owner[f] = i
        hf = {f for f, n in fan.items() if n >= 2}
        owned = collections.Counter(owner[f] for f, n in fan.items() if n == 1)
        classes = [owned.get(i, 0) for i in range(len(cs))]
        base_max = max((len(t) for t in per_cs), default=0)
        # 纯 PA（无随机化）的上下文传播：并查集，调用点与其全部目标并为一类
        parent = {}

        def find(x):
            while parent.setdefault(x, x) != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x
        for i, t in enumerate(per_cs):
            ri = find(("c", i))
            for f in t:
                rf = find(("f", f))
                if rf != ri:
                    parent[rf] = ri
        comp_f = collections.Counter(find(("f", f)) for f in fan)
        comp_c = collections.Counter(find(("c", i)) for i, t in enumerate(per_cs) if t)
        big = max(comp_f, key=comp_f.get) if comp_f else None
        res[mode] = dict(fan=fan, hf=hf, owned=owned, classes=classes,
                         per_cs=per_cs, targets_max=base_max,
                         pa_max_component_funcs=comp_f[big] if big else 0,
                         pa_max_component_callsites=comp_c.get(big, 0),
                         pa_max_component_callsites_any=max(comp_c.values(), default=0))
    # kCFI 式分组：地址被取过的函数按函数类型分组，不看调用点
    res["kcfi_like"] = dict(
        groups=len(by_type), max_group=max((len(v) for v in by_type.values()), default=0),
        top=sorted(((len(v), k) for k, v in by_type.items()), reverse=True)[:8])
    return res


# ---------------------------------------------------------------- report
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vmlinux", required=True)
    ap.add_argument("--snap", action="append", default=[])
    ap.add_argument("--static-dir")
    ap.add_argument("--static-vmlinux")
    ap.add_argument("--json")
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--src-callsite", action="store_true",
                    help="以源码位置 file:line 而非指令地址识别调用点（内联副本合并为一个）")
    ap.add_argument("--runtime", action="store_true",
                    help="丢弃调用点或目标位于 .init.text 的对（启动结束即被释放）")
    a = ap.parse_args()

    syms = Syms(a.vmlinux)
    src_id = {}
    if a.src_callsite:
        allcs = sorted({p[0] for path in a.snap for p in load_snap(path)[0]})
        locs = syms.lines(allcs)
        canon = {}
        for c in allcs:                      # 同一 file:line 的所有地址映射到其中最小者
            canon.setdefault(locs[c], c)
            src_id[c] = canon[locs[c]]

    def own(x):                              # 测量自身的代码（kernel/fanin_trace.c）
        return syms.containing(x).startswith("fanin_")

    def filt(ps):
        ps = [p for p in ps if not own(p[0]) and not own(p[1])]
        if a.runtime:
            ps = [p for p in ps if not syms.is_init(p[0]) and not syms.is_init(p[1])]
        if src_id:
            ps = [(src_id.get(p[0], p[0]), p[1], p[2]) for p in ps]
        return ps
    report = {"phases": []}
    last = None
    for path in a.snap:
        pairs, ext, hdr = load_snap(path)
        pairs = filt(pairs)
        d = analyse_dynamic(pairs, syms)
        ph = dict(snap=os.path.basename(path), header=hdr, pairs=len(pairs), ext_pairs=ext,
                  call_sites=len(d["tgts"]), functions=len(d["fan"]),
                  high_fanin=len(d["hf"]),
                  single_owner=len(d["fan"]) - len(d["hf"]),
                  max_class=max(d["classes"], default=0),
                  mean_class_nonzero=(sum(d["classes"]) / max(1, sum(1 for c in d["classes"] if c))),
                  call_sites_fully_exempt=sum(1 for c in d["classes"] if c == 0),
                  class_hist=histo(d["classes"], (0, 1, 2, 3, 5, 10, 20, 50, 100)),
                  fanin_hist=histo([len(s) for s in d["fan"].values()]),
                  pa_baseline_max_component_funcs=d["comps"][0][0] if d["comps"] else 0,
                  pa_baseline_max_component_callsites=d["comps"][0][1] if d["comps"] else 0,
                  pa_baseline_components=len(d["comps"]),
                  pa_baseline_max_callsites_in_component=max((m for _, m, _ in d["comps"]), default=0),
                  pa_baseline_callsites_sharing_context=sum(m for _, m, _ in d["comps"] if m >= 2),
                  pa_baseline_hist_funcs=histo([n for n, _, _ in d["comps"]], (1, 2, 3, 5, 10, 20, 50, 100)),
                  call_sites_with_class_gt1=sum(1 for c in d["classes"] if c > 1))
        report["phases"].append(ph)
        last = (path, pairs, d, ph)
        print(f"[{ph['snap']}] pairs={ph['pairs']} cs={ph['call_sites']} funcs={ph['functions']} "
              f"HF={ph['high_fanin']} maxclass={ph['max_class']} "
              f"PA-baseline max comp={ph['pa_baseline_max_component_funcs']}f/"
              f"{ph['pa_baseline_max_component_callsites']}cs", file=sys.stderr)

    # 阶段间的漏识别：早期判为单归属、后期被第二个调用点触及的函数（§5.6.4 所说的误杀风险）
    if len(a.snap) >= 2:
        def pairs_of(path):
            return filt(load_snap(path)[0])
        first = analyse_dynamic(pairs_of(a.snap[0]), syms)
        report["single_owner_in_first_but_hf_in_last"] = sum(
            1 for f in last[2]["hf"] if f in first["fan"] and len(first["fan"][f]) == 1)
        report["hf_in_last_unseen_in_first"] = sum(1 for f in last[2]["hf"] if f not in first["fan"])
        # 每一阶段（含第一阶段）判为单归属、到最后一阶段变成高扇入的数目
        report["late_conflicts_by_phase"] = []
        for path in a.snap[:-1]:
            dd = analyse_dynamic(pairs_of(path), syms)
            report["late_conflicts_by_phase"].append(
                (os.path.basename(path),
                 sum(1 for f in last[2]["hf"] if f in dd["fan"] and len(dd["fan"][f]) == 1)))

    if last:
        path, pairs, d, ph = last
        top_cs = sorted(d["tgts"], key=lambda c: -d["owned"].get(c, 0))[:a.top]
        top_hf = sorted(d["hf"], key=lambda f: -len(d["fan"][f]))[:a.top]
        loc = syms.lines(top_cs + [c for f in top_hf for c in list(d["fan"][f])[:1]])
        report["top_classes"] = [
            dict(call_site=hex(c), in_func=syms.containing(c), loc=loc.get(c),
                 owned=d["owned"].get(c, 0), targets=len(d["tgts"][c]),
                 sample=sorted(syms.at(f) for f in d["tgts"][c] if len(d["fan"][f]) == 1)[:8])
            for c in top_cs]
        report["top_high_fanin"] = [
            dict(func=syms.at(f), fanin=len(d["fan"][f]),
                 sample_callers=sorted({syms.containing(c) for c in d["fan"][f]})[:6])
            for f in top_hf]
        report["pa_baseline_top_components"] = [
            dict(funcs=n, call_sites=m, example=syms.at(rep)) for n, m, rep in d["comps"][:8]]
        report["pa_baseline_top_components_by_callsites"] = [
            dict(funcs=n, call_sites=m, example=syms.at(rep))
            for n, m, rep in sorted(d["comps"], key=lambda x: -x[1])[:8]]
        t0 = min(p[2] for p in pairs)
        report["hf_convergence_ns"] = d["became"]

        # 动态边能否被静态目标集覆盖（静态上界的健全性抽检）
        if a.static_dir:
            text_names = set(syms.names)
            if a.static_vmlinux:
                text_names |= set(Syms(a.static_vmlinux).names)
            cs, at_type, at_uses = load_static(a.static_dir, text_names)
            st = analyse_static(cs, at_type, at_uses)
            report["static"] = {}
            report["static_kcfi_like"] = st.pop("kcfi_like")
            for mode, r in st.items():
                top = sorted(range(len(cs)), key=lambda i: -r["classes"][i])[:a.top]
                report["static"][mode] = dict(
                    call_sites=len(cs), address_taken=len(at_type),
                    reachable=len(r["fan"]), high_fanin=len(r["hf"]),
                    single_owner=len(r["fan"]) - len(r["hf"]),
                    max_class=max(r["classes"]), max_targets_per_cs=r["targets_max"],
                    pa_max_component_funcs=r["pa_max_component_funcs"],
                    pa_max_component_callsites=r["pa_max_component_callsites"],
                    pa_max_component_callsites_any=r["pa_max_component_callsites_any"],
                    class_hist=histo(r["classes"], (0, 1, 2, 3, 5, 10, 20, 50, 100)),
                    top_classes=[dict(loc=cs[i]["loc"], caller=cs[i]["caller"],
                                      type=cs[i]["type"], field=cs[i]["field"],
                                      owned=r["classes"][i]) for i in top])
                print(f"[static/{mode}] cs={len(cs)} AT={len(at_type)} reach={len(r['fan'])} "
                      f"HF={len(r['hf'])} maxclass={max(r['classes'])}", file=sys.stderr)

            # 抽检：按 file:line 把动态调用点对到静态调用点，看目标是否在静态目标集中
            locs = syms.lines(sorted(d["tgts"]))
            by_loc = collections.defaultdict(list)
            for i, c in enumerate(cs):
                by_loc[c["loc"]].append(i)
            name_to_syms = collections.defaultdict(set)
            for sym, (name, _) in at_type.items():
                name_to_syms[name].add(sym)
            chk = collections.Counter()
            for c, fs in d["tgts"].items():
                idx = by_loc.get(locs.get(c))
                for f in fs:
                    if not idx:
                        chk["cs_unmatched"] += 1
                        continue
                    n = syms.at(f)
                    for mode in ("type", "field"):
                        ok = any(s in st[mode]["per_cs"][i] for i in idx for s in name_to_syms.get(n, ()))
                        chk[f"{mode}_covered" if ok else f"{mode}_missed"] += 1
            report["static_vs_dynamic_edges"] = dict(chk)

    if a.json:
        json.dump(report, open(a.json, "w"), indent=1, ensure_ascii=False, default=list)
    print(json.dumps({k: v for k, v in report.items()
                      if k not in ("hf_convergence_ns",)}, indent=1, ensure_ascii=False, default=list))


if __name__ == "__main__":
    main()
