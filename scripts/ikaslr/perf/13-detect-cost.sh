#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# E4-A（单次检测与审计的处理开销）+ E4-B（触发频率与触发延迟分布）
#
# 在**已启动进 +RD 档之后**跑。攻击行为一律由**内核内受控自测**驱动 ——
# 本仓库不提供任何用户态攻击程序（漏洞模块已按作者要求永久删除）。
#
#   sudo ./13-detect-cost.sh                    # 正常负载下的触发频率（E4-B 第一步）
#   MINUTES=30 sudo ./13-detect-cost.sh         # 观察更久
#
# ⚠ 平台口径（清单 §0.2，这条最要紧）：
#   **x86 的检测路径若取自 QEMU 嵌套环境，数字会被放大约 10 倍**（实测 VM exit
#   18,279 周期/次 vs 裸机约 1,000–2,000）。因此本脚本**必须在裸机上跑**，
#   或把结果标注为"悲观上界"。ARM 的观察点路径不经虚拟化，数字直接可用。
set -uo pipefail
. "$(dirname "$0")/env.sh"

[ -r /proc/ikaslr/stats ] || { echo "!! 没有 /proc/ikaslr/stats，先启动进 +RD 档"; exit 1; }
MIN="${MINUTES:-10}"
D="$RESULTS_DIR/$ARCH/detect"; mkdir -p "$D"
stat_of() { awk -v k="$1" '$1==k{print $2; exit}' /proc/ikaslr/stats; }

# 裸机核对：QEMU 会在 dmesg 里留痕
if grep -qiE 'qemu|kvm|hypervisor' /proc/cpuinfo /sys/class/dmi/id/sys_vendor 2>/dev/null; then
    cat <<'W'
!! 检测到虚拟化环境。x86 的 EPT 检测路径在嵌套虚拟化下开销被放大约 10 倍，
   这一档的绝对时间**只能作为悲观上界**，不能直接写进论文。
   正式数据请在裸机上采（清单 §0.2）。
W
fi

echo "== E4-A/E4-B 采集：观察 $MIN 分钟"
echo "   档: $(current_variant)   内核: $(uname -r)"
echo "   被随机化函数: $(stat_of functions)"

snap() { cp /proc/ikaslr/stats "$1"; }
snap "$D/stats_t0.txt"
T0=$(date +%s)

# ---- 每分钟采一次，得到触发频率的时间序列（E4-B 因变量①）----
echo "min,elapsed_s,audit_benign,audit_gadget,audit_triggers,rounds,audit_benign_ns,audit_gadget_ns" \
    > "$D/timeline.csv"
for m in $(seq 1 "$MIN"); do
    sleep 60
    printf "%d,%d,%s,%s,%s,%s,%s,%s\n" "$m" "$(( $(date +%s) - T0 ))" \
        "$(stat_of audit_benign)" "$(stat_of audit_gadget)" \
        "$(stat_of audit_triggers)" "$(stat_of rounds)" \
        "$(stat_of audit_benign_ns)" "$(stat_of audit_gadget_ns)" \
        | tee -a "$D/timeline.csv"
done
snap "$D/stats_t1.txt"

python3 - "$D/stats_t0.txt" "$D/stats_t1.txt" "$MIN" <<'PY' | tee "$D/summary.txt"
import sys
def load(p):
    d = {}
    for l in open(p):
        t = l.split()
        if len(t) == 2:
            try: d[t[0]] = int(t[1])
            except ValueError: pass
    return d
a, b, mins = load(sys.argv[1]), load(sys.argv[2]), int(sys.argv[3])
secs = mins * 60
def dd(k): return b.get(k, 0) - a.get(k, 0)

print("=" * 70)
print("E4-B　正常负载下的触发频率（对应需求 D2 的举证）")
print("=" * 70)
tr = dd("audit_triggers")
print(f"  观察时长                : {mins} 分钟")
print(f"  审计放行（入口，良性）  : {dd('audit_benign'):,}")
print(f"  审计判为 gadget         : {dd('audit_gadget'):,}")
print(f"  由检测触发的随机化      : {tr:,}   → {tr/secs:.4f} 次/秒")
print(f"  总随机化轮数            : {dd('rounds'):,}")
print()
if tr == 0:
    print("  ✅ 正常负载下触发频率为 **0**。")
    print("     清单 §E4-B：'若正常负载下触发频率接近零，那本身就是不因正常行为频繁")
    print("     误触发（需求 D2）的证据，不必再单独统计误报率。'")
    print("     → D2 的举证到此完成，原 E4-3 误报率实验不必做。")
else:
    print("  ⚠ 正常负载下有触发。清单要求这时**回头做来源归类**：")
    print("     内核自身的合法代码读取（kprobes / ftrace / 栈回溯 / /proc/kcore）")
    print("     正在被当作攻击。先查这些来源，再决定是否放宽判据。")

print()
print("=" * 70)
print("E4-A　单次审计的处理时间")
print("=" * 70)
for tag, cnt_k, ns_k, max_k in (("放行（入口）", "audit_benign", "audit_benign_ns", "audit_benign_max"),
                                ("判为 gadget", "audit_gadget", "audit_gadget_ns", "audit_gadget_max")):
    n, ns = dd(cnt_k), dd(ns_k)
    mx = b.get(max_k, 0)
    if n:
        print(f"  {tag:<14} 次数 {n:>8,}   平均 {ns/n:>8.0f} ns   历史最大 {mx:>8,} ns")
    else:
        print(f"  {tag:<14} 次数        0   （本次观察期内未发生）")
print()
print("  口径：内核只累计 总和 与 最大值，不留样本数组——这条路径在异常上下文里执行，")
print("        不能分配内存。**分位数需要靠多次短窗口采样差分**得到，")
print("        用 timeline.csv 逐分钟的增量算即可。")
print()
print("  ⚠ 尚缺：清单 §E4-A 要求把检测拆成'异常捕获 → 定位被读页 → 临时开读+单步")
print("     → 标记被探测函数 → 恢复权限'各阶段分别计时。当前只测到了**审计**这一条")
print("     路径（陷阱 → 按偏移判定 → 重定向）。XOM/EPT 那条链的分阶段计时尚未插桩，")
print("     需要在 kernel/ikaslr/xom_ept.c 里补，且必须在裸机上测。")
PY

echo
echo "== 完成： $D/  （timeline.csv / summary.txt / stats_t*.txt）"
echo "   E4-B 的因变量②③（触发延迟分布、攻击者凑齐 gadget 所需时间）"
echo "   由内核内受控自测驱动，见 kernel/ikaslr/selftest.c 的 test_cf_audit。"
