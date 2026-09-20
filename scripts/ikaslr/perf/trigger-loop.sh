#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# 定频驱动随机化（+R 档用）：每 <间隔ms> 写一次 /proc/ikaslr/trigger。
#   trigger-loop.sh 20      # 每 20ms 触发一次（与 Adelie 同）
# Ctrl-C 或被 kill 停止。
set -eu
MS="${1:?usage: trigger-loop.sh <interval_ms>}"
[ -w /proc/ikaslr/trigger ] || { echo "no /proc/ikaslr/trigger (IKASLR on?)"; exit 1; }
SEC=$(awk "BEGIN{print $MS/1000}")
echo "driving randomization every ${MS}ms; Ctrl-C to stop"
while true; do echo 1 > /proc/ikaslr/trigger; sleep "$SEC"; done
