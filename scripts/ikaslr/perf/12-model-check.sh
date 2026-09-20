#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# E3-A 开销模型的端到端交叉验证：Base vs +R 的直接吞吐比。
#
# E3-A 给出 总开销 ≈ G × 常驻税 + M × K。若 G 与常驻税都测准了，那么同一负载在
# Base 与 +R 两档下的**吞吐比**应当与模型预测相符。这是把微观数字接回端到端的
# 最短路径，不需要装任何 benchmark（见 tools/ikaslr/init_tput.c）。
#
# 负载刻意选纯系统调用循环，把跳板税放大到最明显——因此结果是**上界**。
#
#   用法: ./12-model-check.sh [输出目录]
set -euo pipefail
. "$(dirname "$0")/env.sh"

OUT="${1:-$RESULTS_DIR/model-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"
IRFS="$OUT/tput.cpio.gz"
"$ROOT/scripts/ikaslr/mkinitramfs.sh" "$ROOT/tools/ikaslr/init_tput.c" "$IRFS" >/dev/null

FUNCS="$ROOT/scripts/ikaslr/funcs/s3-full.txt"
# Base 档用空名单而不是关掉插件：见 funcs/empty.txt 里的说明。
FUNCS_BASE="$ROOT/scripts/ikaslr/funcs/empty.txt"
REPEAT="${REPEAT:-5}"

build() {   # $1=base|R
    local v="$1" o="$BUILD_ROOT/model-$1" f="$FUNCS"
    [ "$v" = base ] && f="$FUNCS_BASE"
    mkdir -p "$o"
    echo "== 编译 $v -> $o" >&2
    # 纪律：两档只差 IKASLR 相关 CONFIG；强制项（retpoline/unwinder）**两档相同**，
    # 否则测的是那些设施的开销而不是随机化开销。
    kbuild "$o" CRR_TRAMPOLINE=y IKASLR_FUNCS="$f" x86_64_defconfig >/dev/null
    # shellcheck disable=SC2046
    "$ROOT/scripts/config" --file "$o/.config" \
        --set-str LOCALVERSION "-model-$v" --disable LOCALVERSION_AUTO \
        $(ikaslr_mandatory_config) \
        $(case "$v" in
            base)    echo "--disable IKASLR" ;;
            R)       echo "--enable IKASLR --disable IKASLR_STATS --disable IKASLR_DEBUG" ;;
            R-stats) echo "--enable IKASLR --enable IKASLR_STATS --disable IKASLR_DEBUG" ;;
          esac)
    kbuild "$o" CRR_TRAMPOLINE=y IKASLR_FUNCS="$f" olddefconfig >/dev/null
    kbuild "$o" CRR_TRAMPOLINE=y IKASLR_FUNCS="$f" -j"$JOBS" bzImage \
        > "$o/build.log" 2>&1 || { echo "!! $v 编译失败，见 $o/build.log" >&2; return 1; }
    echo "$o"
}

run() {     # $1=构建目录 $2=标签
    local o="$1" tag="$2" i
    for i in $(seq 1 "$REPEAT"); do
        timeout 200 qemu-system-x86_64 -enable-kvm -cpu host -smp 1 -m 2G \
            -kernel "$o/arch/x86/boot/bzImage" -initrd "$IRFS" -nographic \
            -append "console=ttyS0 nokaslr panic=-1 processor.max_cstate=1 intel_idle.max_cstate=0" \
            2>&1 | grep -E '^TPUT' >> "$OUT/$tag.log" || true
    done
    grep -oE 'loops_per_s=[0-9]+' "$OUT/$tag.log" | cut -d= -f2
}

# R-stats 只用来在**同一负载**上取 G（enters/outs 计数需要 STATS）；
# 吞吐比仍取自 base 与 R 两档，因为 STATS 本身也有开销（约 1.0~1.6 ns/次）。
for v in base R R-stats; do
    o=$(build "$v")
    echo "== 采集 $v（$REPEAT 次）"
    run "$o" "$v" > "$OUT/$v.tput"
    grep -h '^TPUT-G' "$OUT/$v.log" 2>/dev/null | head -3 | sed 's/^/     /' || true
    printf "   %s: %s\n" "$v" "$(tr '\n' ' ' < "$OUT/$v.tput")"
done

python3 - "$OUT" <<'PY'
import statistics as st, sys, pathlib
d = pathlib.Path(sys.argv[1])
def load(n):
    return sorted(int(x) for x in (d / f"{n}.tput").read_text().split())
b, r = load("base"), load("R")
import re
gline = ""
gp = d / "R-stats.log"
if gp.exists():
    for ln in gp.read_text().splitlines():
        if ln.startswith("TPUT-G d_enters"):
            gline = ln
            break
mb, mr = st.median(b), st.median(r)
print()
print("=" * 62)
print("E3-A 开销模型的端到端交叉验证（纯系统调用负载，单核，上界）")
print("=" * 62)
for n, xs in (("Base", b), ("+R", r)):
    print(f"  {n:<6} 中位数 {st.median(xs):>12,.0f} 轮/秒   "
          f"[{xs[0]:,} … {xs[-1]:,}]  n={len(xs)}")
print()
print(f"  吞吐比 +R/Base = {mr/mb:.3f}")
print(f"  端到端开销      = {(mb/mr - 1) * 100:.1f}%   （相对 Base 的变慢幅度）")
print(f"  等效 CPU 占用   = {(1 - mr/mb) * 100:.1f}%   （模型预测 57%）")
print()
if gline:
    g = dict(re.findall(r"(\w+)=([-\d.]+)", gline))
    pin, pout = float(g["per_loop_in"]), float(g["per_loop_out"])
    # 常驻税取自 E3-A 的 STATS=n 档（生产配置）
    TAX_IN, TAX_OUT = 13.02e-9, 5.00e-9
    per_loop_tax = pin * TAX_IN + pout * TAX_OUT
    tb, tr = 1.0 / mb, 1.0 / mr
    print(f"  同一负载下每轮的跨区域调用：fixed_in {pin:.1f} 次，fixed_out {pout:.1f} 次")
    print(f"  每轮耗时  Base {tb*1e6:.3f} µs  ->  +R {tr*1e6:.3f} µs   "
          f"实测增量 {(tr-tb)*1e6:.3f} µs")
    print(f"  模型预测每轮增量 = {pin:.1f}×13.02ns + {pout:.1f}×5.00ns "
          f"= {per_loop_tax*1e6:.3f} µs")
    print(f"  模型覆盖率 = {per_loop_tax/(tr-tb)*100:.0f}%")
    print()
    print("  模型未覆盖的部分是二阶效应（缓存局部性、页表压力、间接调用的分支预测），")
    print("  交给 E12 归因。")
else:
    print("  （缺 R-stats 档的 G 数据，无法代入模型）")
PY
echo
echo "原始数据: $OUT"
