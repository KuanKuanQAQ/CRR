#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# R-66　长时间运行的漂移观察（§6.10）。清单说"挂着跑即可，占日历时间不占人力"，
# 建议在别的实验做完之后挂上去。
#
#   sudo ./15-longrun.sh                 # 默认 24 小时，每 5 分钟采一次
#   HOURS=72 INTERVAL=300 sudo ./15-longrun.sh
#   nohup sudo ./15-longrun.sh &         # 后台挂着
#
# 观察四件事随时间是否漂移，并提前发现内存泄漏：
#   随机化区域的地址空间占用 / 跳板表规模 / 被探测函数集合规模 / 触发频率
set -uo pipefail
. "$(dirname "$0")/env.sh"

[ -r /proc/ikaslr/stats ] || { echo "!! 没有 /proc/ikaslr/stats，先启动进 +R/+RD 档"; exit 1; }
HOURS="${HOURS:-24}"; INTERVAL="${INTERVAL:-300}"
D="$RESULTS_DIR/$ARCH/longrun"; mkdir -p "$D"
CSV="$D/drift-$(date +%Y%m%d-%H%M%S).csv"
stat_of() { awk -v k="$1" '$1==k{print $2; exit}' /proc/ikaslr/stats; }

N=$(( HOURS * 3600 / INTERVAL ))
echo "== R-66 长跑观察：$HOURS 小时，每 $INTERVAL 秒一次，共 $N 个采样点"
echo "   输出: $CSV"

# 后台放一个温和的负载，让随机化真的在跑；不加负载的话区域一直是空的，测不到什么。
if variant_needs_trigger "$(current_variant)"; then
    "$PERF_DIR/trigger-loop.sh" "$TRIGGER_MS" > "$D/_trigger.log" 2>&1 &
    TRIG=$!
    trap '[ -n "${TRIG:-}" ] && kill "$TRIG" 2>/dev/null' EXIT
fi

echo "t_s,rounds,rounds_missed,functions,whitelist_entries,audit_triggers,enter_backoffs,enter_forced,nested_admitted,stale_fixups,stale_fixup_fails,whitelist_rejects,slab_kb,vmalloc_kb,memfree_kb" > "$CSV"
T0=$(date +%s)
for i in $(seq 1 "$N"); do
    printf "%d,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
        "$(( $(date +%s) - T0 ))" \
        "$(stat_of rounds)" "$(stat_of rounds_missed)" "$(stat_of functions)" \
        "$(stat_of whitelist_entries)" "$(stat_of audit_triggers)" \
        "$(stat_of enter_backoffs)" "$(stat_of enter_forced)" \
        "$(stat_of nested_admitted)" "$(stat_of stale_fixups)" \
        "$(stat_of stale_fixup_fails)" "$(stat_of whitelist_rejects)" \
        "$(awk '/^Slab:/{print $2}' /proc/meminfo)" \
        "$(awk '/^VmallocUsed:/{print $2}' /proc/meminfo)" \
        "$(awk '/^MemFree:/{print $2}' /proc/meminfo)" \
        >> "$CSV"
    sleep "$INTERVAL"
done

python3 - "$CSV" <<'PY' | tee "$D/drift-summary.txt"
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
if len(rows) < 2:
    sys.exit("采样点太少")
a, b = rows[0], rows[-1]
hrs = (int(b["t_s"]) - int(a["t_s"])) / 3600
print("=" * 66)
print(f"R-66　长跑漂移　观察 {hrs:.1f} 小时，{len(rows)} 个采样点")
print("=" * 66)
print(f"{'指标':<24}{'起始':>14}{'结束':>14}{'变化':>14}")
for k in ("functions", "whitelist_entries", "vmalloc_kb", "slab_kb", "memfree_kb"):
    try:
        x, y = int(a[k]), int(b[k])
    except (KeyError, ValueError):
        continue
    print(f"{k:<24}{x:>14,}{y:>14,}{y-x:>+14,}")
print()
for k, want in (("stale_fixup_fails", 0), ("whitelist_rejects", 0)):
    v = int(b.get(k, 0))
    print(("  ✅ " if v == want else "  ❌ ") + f"{k} = {v}（要求 {want}）")
rounds = int(b["rounds"]) - int(a["rounds"])
print(f"  随机化 {rounds:,} 轮，平均 {rounds/max(hrs*3600,1):.2f} 次/秒")
print()
print("  判据：functions 与 whitelist_entries 是静态量，**不该变**；")
print("        vmalloc_kb 应稳定在变体池 + 陷阱影像那个量级，**持续上涨就是泄漏**；")
print("        被探测函数集合只进不出，其陷阱页与副本会随时间增长——那是已知未解决项")
print("        （见 实验/E4-D §判据核对），本表的 vmalloc 曲线正是它的证据。")
PY
echo
echo "== 完成： $CSV"
