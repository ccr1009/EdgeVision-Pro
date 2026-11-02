#!/bin/bash
echo "===== A. 失败时刻的完整 dmesg 上下文 ====="
# 定位所有 dma map ... fail 及其前后 60 行
dmesg 2>/dev/null | grep -n "dma map .* fail" | tail -5
echo "--- 第一次失败的完整上下文（前后各 60 行）---"
dmesg 2>/dev/null | grep -n "rknpu_gem_get_pages" | head -1
LINE=$(dmesg 2>/dev/null | grep -n "rknpu_gem_get_pages" | head -1 | cut -d: -f1)
if [ -n "$LINE" ]; then
  START=$((LINE-70)); [ $START -lt 1 ] && START=1
  dmesg 2>/dev/null | sed -n "${START},$((LINE+6))p"
fi

echo
echo "===== B. rknpu 驱动的 IOMMU / 内存限制（从 dmesg 早期看）====="
dmesg 2>/dev/null | grep -i -E "rknpu.*(iova|iommu|memory|space|limit|dma)" | head -20

echo
echo "===== C. 当前 NPU IOMMU 已用（估算）====="
echo "--- 所有打开 renderD129 / rknpu 的 fd 数 ---"
for p in /proc/[0-9]*; do
  pid=${p#/proc/}
  n=$(ls -l $p/fd 2>/dev/null | grep -c "renderD129")
  [ "$n" -gt 0 ] && echo "PID $pid  fd_count=$n  $(tr '\0' ' ' < $p/cmdline 2>/dev/null | cut -c1-60)"
done

echo
echo "===== D. 直接对比：用户原始命令（不设环境变量）跑 3 次 ====="
cd /root/rkllm_deploy
for i in 1 2 3; do
  echo "---------- 第 $i 次 ----------"
  echo "  before: CmaFree=$(awk '/CmaFree/{print $2}' /proc/meminfo) MemFree=$(awk '/MemFree/{print $2}' /proc/meminfo) NPU=$(cat /sys/kernel/debug/rknpu/load | tr -d '\n')"
  OUT=$(echo "quit" | timeout 120 ./rkllm_embodied_agent 2>&1)
  echo "$OUT" | grep -E "failed to allocate|failed to malloc|load model file error|rkllm_init failed|Function tools set successfully|已就绪" | head -8
  echo "  after : CmaFree=$(awk '/CmaFree/{print $2}' /proc/meminfo) MemFree=$(awk '/MemFree/{print $2}' /proc/meminfo)"
done
echo "===== DONE ====="
