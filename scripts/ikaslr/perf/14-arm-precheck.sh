#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# R-61　ARM 平台能力确认 —— 清单第一批的第 1 项，**决定主平台，一变则第 6 章大半重做**。
#
# 在**目标 ARM 机上**跑（普通内核即可，不必先装 I-KASLR 内核）：
#   ./14-arm-precheck.sh
#
# 要确认三件事：
#   ① FEAT_PAuth 可用（第 5 章的 PA CFI 依赖它）
#   ② 调试观察点可用，且**能在生产配置下于 EL1 数据访问触发**（第 4 章的 ARM 检测路径）
#   ③ DBGWCR_EL1.MASK 的**实际位宽**（决定一个观察点能覆盖多大范围）
#
# ①② 本脚本能从 /proc/cpuinfo、dmesg 与内核配置确认到"平台支持"这一层；
# ③ 以及"生产配置下 EL1 触发"这一条**必须由内核内探测**给出——用户态读不到
#   ID_AA64DFR0_EL1 / DBGWCR_EL1。脚本会明确告诉你哪一条还没被确认，不替你打包票。
set -uo pipefail

ARCH_M=$(uname -m)
[ "$ARCH_M" = aarch64 ] || { echo "!! 本脚本要在 aarch64 上跑（当前 $ARCH_M）"; exit 1; }
OUT="${1:-$(dirname "$0")/results/arm64/precheck}"; mkdir -p "$OUT"
exec > >(tee "$OUT/precheck.txt") 2>&1

echo "=================================================================="
echo " R-61  ARM 平台能力确认      $(date -Is)"
echo " 机器: $(grep -m1 'model name\|CPU implementer' /proc/cpuinfo 2>/dev/null || echo 未知)"
echo " 内核: $(uname -r)"
echo "=================================================================="

ok()   { echo "  ✅ $*"; }
no()   { echo "  ❌ $*"; }
warn() { echo "  ⚠  $*"; }

# ---------------- ① FEAT_PAuth ----------------
echo
echo "① 指针认证 FEAT_PAuth"
FEAT=$(awk -F: '/^Features/{print $2; exit}' /proc/cpuinfo)
echo "   cpuinfo Features: $FEAT"
HAS_PACA=0; HAS_PACG=0
case " $FEAT " in *" paca "*) HAS_PACA=1 ;; esac
case " $FEAT " in *" pacg "*) HAS_PACG=1 ;; esac
[ "$HAS_PACA" = 1 ] && ok "paca（地址认证 QARMA/实现定义算法）可用" \
                    || no "**没有 paca** —— 第 5 章的 PA CFI 在本机无法运行"
[ "$HAS_PACG" = 1 ] && ok "pacg（通用认证）可用" || warn "没有 pacg（本方案不依赖它）"

for c in ARM64_PTR_AUTH ARM64_PTR_AUTH_KERNEL; do
    v=$(zcat /proc/config.gz 2>/dev/null | grep "^CONFIG_$c=" \
        || grep "^CONFIG_$c=" "/boot/config-$(uname -r)" 2>/dev/null)
    [ -n "$v" ] && ok "$v" || warn "当前运行内核未开 CONFIG_$c（装 I-KASLR 内核时要开）"
done

# ---------------- ② 调试观察点 ----------------
echo
echo "② 硬件观察点"
HB=$(dmesg 2>/dev/null | grep -i 'hw-breakpoint' | head -3)
if [ -n "$HB" ]; then
    echo "$HB" | sed 's/^/   /'
    NW=$(echo "$HB" | grep -oE '[0-9]+ watchpoint' | grep -oE '[0-9]+' | head -1)
    [ -n "$NW" ] && [ "$NW" -gt 0 ] && ok "内核报告 $NW 个观察点寄存器" \
                 || warn "没解析出观察点个数，看上面原文"
else
    warn "dmesg 里没有 hw-breakpoint 行（可能已被 rate-limit 冲掉；试 dmesg | grep -i breakpoint）"
fi
v=$(zcat /proc/config.gz 2>/dev/null | grep '^CONFIG_HAVE_HW_BREAKPOINT=' \
    || grep '^CONFIG_HAVE_HW_BREAKPOINT=' "/boot/config-$(uname -r)" 2>/dev/null)
[ -n "$v" ] && ok "$v" || no "内核未开 CONFIG_HAVE_HW_BREAKPOINT"

echo
warn "以下两条**用户态确认不了**，必须由内核内探测给出："
echo "     (a) 观察点能否在**生产配置**下于 EL1 的数据访问上触发"
echo "         —— 有些实现只在 EL0 生效，或被固件/hyp 占用；"
echo "     (b) DBGWCR_EL1.MASK 的**实际位宽**（架构给 5 位，实现可少于此）。"
echo "     做法：装上 CONFIG_IKASLR + 第 4 章观察点路径的内核，读它的自测输出。"
echo "     两条都确认之前，**不要把 ARM 定为主平台**。"

# ---------------- ③ 其他会影响第 6 章的项 ----------------
echo
echo "③ 其他与第 6 章相关的平台项"
echo "   核数: $(nproc)    大小核: $(lscpu 2>/dev/null | grep -ci 'core.*cluster\|big.LITTLE' || echo 未检出)"
echo "   页大小: $(getconf PAGE_SIZE) B"
[ "$(getconf PAGE_SIZE)" = 4096 ] && ok "4 KB 页（与 x86 口径一致，便于对比）" \
    || warn "**不是 4 KB 页**。变体池容量、改映射代价、内存开销都会与 x86 不可直接比，正文要说明"
G=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 无)
echo "   调频策略: $G"
[ "$G" = performance ] || warn "跑性能实验前先 cpupower frequency-set -g performance"

echo
echo "=================================================================="
echo " 结论"
echo "=================================================================="
if [ "$HAS_PACA" = 1 ]; then
    echo "  PA 可用。观察点的**平台支持**见上；但 (a)(b) 两条尚未确认，"
    echo "  它们决定第 4 章的 ARM 检测路径能不能落地。"
else
    echo "  **paca 不可用 → 本机不能作为第 5 章的平台。** 换机器，或把 ARM 部分改为"
    echo "  '机制级验证 + 文献参数'，并在正文如实说明未在真实 PAuth 硬件上评测。"
fi
echo
echo "  结果已存: $OUT/precheck.txt"
