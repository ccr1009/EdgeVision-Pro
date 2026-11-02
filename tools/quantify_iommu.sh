#!/bin/bash
# 量化实验：逐个加载 rkllm 实例，看每个吃多少 NPU IOMMU 空间、第几个开始失败
# 关键：只抓本次新增的 dmesg，避免误读历史日志
cd /root/rkllm_deploy
export LD_LIBRARY_PATH=/root/rkllm_deploy

DSM() { dmesg 2>/dev/null | wc -l; }

echo "############ 前置：彻底清场 ############"
pkill -9 -f rkllm_embodied_agent 2>/dev/null
pkill -9 -f llm_demo 2>/dev/null
pkill -f edgevision_dashboard 2>/dev/null
systemctl stop edgevision.service 2>/dev/null
sleep 4
echo "  残留进程: $(pgrep -af 'rkllm|llm_demo|edgevision_dashboard|w4_edgevision' || echo 无)"
echo "  CmaFree=$(awk '/CmaFree/{print $2}' /proc/meminfo)kB  MemAvailable=$(awk '/MemAvailable/{print $2}' /proc/meminfo)kB"

declare -a PIDS
for N in 1 2 3; do
  echo
  echo "════════ 第 $N 个实例 ════════"
  MARK=$(DSM)
  fifo=/tmp/fq$N
  mkfifo $fifo 2>/dev/null
  ( sleep 900 > $fifo & )
  ./rkllm_embodied_agent < $fifo > /tmp/inst$N.log 2>&1 &
  PID=$!
  PIDS+=($PID)

  # 轮询等待结果，最多 150 秒
  RESULT=""
  for i in $(seq 1 150); do
    if grep -q "已就绪" /tmp/inst$N.log 2>/dev/null; then RESULT=OK; break; fi
    if grep -q "rkllm_init failed" /tmp/inst$N.log 2>/dev/null; then RESULT=FAIL; break; fi
    if ! ps -p $PID > /dev/null 2>&1; then RESULT=DEAD; break; fi
    sleep 1
  done
  echo "  PID=$PID  结果=${RESULT:-超时}  等待了约 ${i}s"
  echo "  RSS=$(awk '/VmRSS/{print $2$3}' /proc/$PID/status 2>/dev/null || echo '-')"

  if [ "$RESULT" = "FAIL" ]; then
    echo "  ---- 失败输出（逐字）----"
    grep -E "failed to allocate|failed to malloc|load model file error|rkllm_init failed" /tmp/inst$N.log | sed 's/^/     /'
    echo "  ---- 【新增】dmesg 里驱动的 IOMMU dump ----"
    dmesg 2>/dev/null | tail -n +$((MARK+1)) | grep -E "^\s*[0-9]+: \[|^\s*used:|dma map|dma_map" | tail -45 | sed 's/^/     /'
    break
  fi

  if [ "$RESULT" = "OK" ]; then
    echo "  ---- 本次新增 dmesg（有 dump 说明期间发生过分配失败）----"
    NEW=$(dmesg 2>/dev/null | tail -n +$((MARK+1)) | grep -E "used:|dma map" | tail -3)
    [ -n "$NEW" ] && echo "$NEW" | sed 's/^/     /' || echo "     （无失败记录，说明第 $N 个也塞得下）"
  fi
  sleep 5
done

echo
echo "############ 汇总 ############"
echo "  当前存活实例数: $(pgrep -c -f rkllm_embodied_agent 2>/dev/null || echo 0)"
for f in /tmp/inst1.log /tmp/inst2.log /tmp/inst3.log; do
  [ -f $f ] && echo "  $f: $(grep -qE '已就绪' $f && echo 成功 || echo 失败)"
done
echo "  历史累计 dma map 失败次数: $(dmesg 2>/dev/null | grep -c 'rknpu_gem_get_pages: dma map')"

echo
echo "############ 清理并恢复 ############"
pkill -9 -f rkllm_embodied_agent 2>/dev/null
pkill -f "sleep 900" 2>/dev/null
rm -f /tmp/fq1 /tmp/fq2 /tmp/fq3
sleep 5
echo "  残留: $(pgrep -af rkllm_embodied_agent || echo 无)"
MARK2=$(DSM)
echo "  清理后再加载一次验证 IOMMU 已释放:"
echo "quit" | timeout 200 ./rkllm_embodied_agent > /tmp/verify.log 2>&1
grep -q "已就绪" /tmp/verify.log && echo "    ✅ 成功" || { echo "    ❌ 失败"; grep -E "failed" /tmp/verify.log | sed 's/^/       /'; }
systemctl start edgevision.service 2>/dev/null
sleep 3
echo "  gateway=$(systemctl is-active edgevision.service)"
echo "===== DONE ====="
