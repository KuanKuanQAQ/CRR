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

REL="$(uname -r)"; CFG="${REL##*-ikaslr-}"
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

# 分类小计
python3 - "$D/report_all.txt" <<'PY' | tee -a "$D/attribution.txt"
import re,sys
cats={"跳板间接跳转/体":[r"\.tramp\.", r"_body\b"],
      "进出计数":[r"ikaslr_enter",r"ikaslr_leave"],
      "fixed_out/白名单":[r"ikaslr_out_enter",r"whitelist"],
      "重随机化关键路径":[r"ikaslr_rerandom",r"ikaslr_update_target"],
      "陷阱修复/检测":[r"ikaslr_fixup",r"detect"],
      "PA 指令":[r"pacia",r"autia"],
      "XOM/EPT":[r"xom",r"\bept\b"]}
tot={k:0.0 for k in cats}
try: lines=open(sys.argv[1],errors="replace").read().splitlines()
except OSError: lines=[]
for ln in lines:
    m=re.match(r"\s*([\d.]+)%",ln)
    if not m: continue
    pct=float(m.group(1))
    for k,pats in cats.items():
        if any(re.search(p,ln,re.I) for p in pats): tot[k]+=pct; break
print("\n== 归因小计（%总采样）：")
for k,v in tot.items(): print(f"  {k:<18} {v:6.2f}%")
print("  注：CRR_TRAMPOLINE=n 时热路径未随机化，占比会接近 0；真实归因需 pass 铺到负载热路径。")
PY
echo
echo "== 完成： $D/  （perf.data / report_all.txt / attribution.txt）"
