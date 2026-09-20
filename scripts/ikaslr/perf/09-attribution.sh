#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 步骤 E12：端到端开销归因（§6.9）。用 perf 采样把开销拆回来源：
#   跳板间接跳转 / 进出计数(enter,leave) / 白名单 / fixed_out / 检测异常处理 /
#   PAC 指令(pacia,autia) / XOM 处理。在**已启动进某 +R/+RD/+RDP 档**后跑。
#   sudo ./09-attribution.sh
#   WORKLOAD='fio ...' DUR=30 sudo ./09-attribution.sh
set -uo pipefail
. "$(dirname "$0")/env.sh"
command -v perf >/dev/null || { echo "!! 需要 perf（apt: linux-perf / dnf: perf）"; exit 1; }

REL="$(uname -r)"; CFG="$(current_variant "$REL")"
DUR="${DUR:-20}"
D="$RESULTS_DIR/$ARCH/attribution/$CFG"; mkdir -p "$D"
echo "== 归因采样 @ $CFG ($REL)，$DUR s"

# 触发随机化（若该档需要）
TRIG=""
if variant_needs_trigger "$CFG" && [ -w /proc/ikaslr/trigger ]; then
    "$PERF_DIR/trigger-loop.sh" "$TRIGGER_MS" >/dev/null 2>&1 & TRIG=$!
fi
cleanup(){ [ -n "$TRIG" ] && kill "$TRIG" 2>/dev/null || true; }
trap cleanup EXIT

# 负载：默认 fio randread；WORKLOAD 可覆盖
WL="${WORKLOAD:-}"
if [ -z "$WL" ]; then
    if command -v fio >/dev/null && [ -r "$PERF_DIR/fio/randread-4k.fio" ]; then
        WL="fio $PERF_DIR/fio/randread-4k.fio"
    else
        WL="sysbench cpu --time=$DUR run"
    fi
fi
echo "   负载: $WL"

# 全系统采样（带调用图），采样时长由负载决定；用 timeout 兜底
perf record -a -g -o "$D/perf.data" -- timeout "$DUR" bash -c "$WL" >/dev/null 2>&1 || true

# 总体符号占比
perf report -i "$D/perf.data" --stdio --sort symbol 2>/dev/null > "$D/report_all.txt" || true

# I-KASLR 各来源的占比归因
echo "== I-KASLR 相关符号占比（占总采样%）：" | tee "$D/attribution.txt"
grep -iE 'ikaslr_enter|ikaslr_leave|ikaslr_out_enter|whitelist|ikaslr_rerandom|ikaslr_update_target|ikaslr_fixup|\.tramp\.|_body\b|pacia|autia|xom|ept' \
     "$D/report_all.txt" 2>/dev/null | head -40 | tee -a "$D/attribution.txt" \
     || echo "  (未采到 I-KASLR 符号——多半是 CRR_TRAMPOLINE=n，热路径未随机化)" | tee -a "$D/attribution.txt"

# ---------------------------------------------------------------------------
# 按地址归桶：**perf 无法符号化变体里的函数体**。
#
# 被随机化的函数体运行在 vmalloc 区，不在 _stext.._etext 内，kallsyms 解析不到，
# 于是 perf report 里它们表现为一大块 [unknown] 或裸地址——而那恰恰就是我们最想
# 归因的那部分。只看符号名会把随机化代码的开销整块漏掉。
#
# 因此这里用 perf script 取原始地址，落在当前变体区间内的样本单独成一桶。
# ---------------------------------------------------------------------------
LIVE_BASE=$(dmesg 2>/dev/null | grep -oE 'live=[0-9a-f]+' | tail -1 | cut -d= -f2)
POOL_SZ=$(dmesg 2>/dev/null | grep -oE 'variants x [0-9]+ B' | tail -1 | grep -oE '[0-9]+')
perf script -i "$D/perf.data" -F ip 2>/dev/null > "$D/ips.txt" || true

python3 - "$D/report_all.txt" "$D/ips.txt" "${LIVE_BASE:-0}" "${POOL_SZ:-0}" <<'PY' | tee -a "$D/attribution.txt"
import re, sys
report, ips, live_hex, pool = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4] or 0)

cats = {
    "跳板 .tramp.text":   [r"\.tramp\."],
    "进出计数 enter/leave": [r"ikaslr_enter", r"ikaslr_leave"],
    "fixed_out + 白名单":  [r"ikaslr_out_enter", r"ikaslr_out_leave", r"whitelist"],
    "随机化关键路径":      [r"ikaslr_rerandom", r"ikaslr_update_target", r"ikaslr_remap"],
    "陷阱修正":            [r"ikaslr_fixup"],
    "检测（第4章）":       [r"ikaslr_detect", r"xom", r"\bept\b"],
    "PA 指令（第5章）":    [r"pacia", r"autia", r"pacfi"],
}
tot = {k: 0.0 for k in cats}
for ln in open(report, errors="replace").read().splitlines():
    m = re.match(r"\s*([\d.]+)%", ln)
    if not m:
        continue
    pct = float(m.group(1))
    for k, pats in cats.items():
        if any(re.search(p, ln, re.I) for p in pats):
            tot[k] += pct
            break

print("\n== 归因小计（占总采样 %）")
for k, v in tot.items():
    print(f"  {k:<22} {v:6.2f}%")

# 变体区间内的样本
if pool and live_hex != "0":
    lo = int(live_hex, 16); hi = lo + pool
    n = inr = 0
    try:
        for ln in open(ips, errors="replace"):
            t = ln.strip().split()
            if not t:
                continue
            try:
                a = int(t[0], 16)
            except ValueError:
                continue
            n += 1
            if lo <= a < hi:
                inr += 1
    except OSError:
        n = 0
    if n:
        print(f"  {'随机化函数体（按地址）':<22} {100.0*inr/n:6.2f}%"
              f"   [{lo:#x},{hi:#x}) 内 {inr:,}/{n:,} 个样本")
        print("     ↑ perf 无法符号化这一段，只能按地址归桶；这就是 report 里那块 [unknown]")
else:
    print("  （拿不到当前变体的地址区间，无法按地址归桶——需要 dmesg 里的")
    print("     'variant pool: N variants x M B, live=...' 一行）")

print()
print("== 与 E3-A 模型闭环（清单 §E12 的真正价值所在）")
print("  E3-A 给出 总开销 ≈ G × 常驻税 + M × K 的**预测**，本节给出**实测的分解**。")
print("  两者之差就是模型未覆盖的部分：缓存局部性损失、页表压力、变体准备与前台争抢 CPU。")
print("  **若端到端开销显著高于各微观开销之和，必须解释差额来自何处**——如实报告这类")
print("  二阶效应反而是本章的价值，不要把它抹平。")
PY
echo
echo "== 完成： $D/  （perf.data / report_all.txt / ips.txt / attribution.txt）"
