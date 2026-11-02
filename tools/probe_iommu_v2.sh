#!/bin/bash
# 探针法 v2：改用 tail -f /dev/null 供 stdin（避开 FIFO 阻塞问题），并打印诊断
cd /root/rkllm_deploy
export LD_LIBRARY_PATH=/root/rkllm_deploy
DSM() { dmesg 2>/dev/null | wc -l; }
FAILS() { dmesg 2>/dev/null | grep -c 'rknpu_gem_get_pages: dma map'; }

cleanall() {
  pkill -9 -f rkllm_embodied_agent 2>/dev/null
  pkill -9 -f "tail -f /dev/null" 2>/dev/null
  pkill -f edgevision_dashboard 2>/dev/null
  systemctl stop edgevision.service 2>/dev/null
  sleep 4
}

spawn() {   # $1 = 序号；结果写到 /tmp/r_$1
  local n=$1
  tail -f /dev/null | ./rkllm_embodied_agent > /tmp/g$n.log 2>&1 &
  sleep 1
  # 找到真正的 agent 进程
  local pid=$(pgrep -f "rkllm_embodied_agent$" | tail -1)
  echo "$pid" > /tmp/g$n.pid
  rm -f /tmp/r_$n
  (
    for i in $(seq 1 180); do
      if grep -q "已就绪" /tmp/g$n.log 2>/dev/null; then echo OK > /tmp/r_$n; exit; fi
      if grep -q "rkllm_init failed" /tmp/g$n.log 2>/dev/null; then echo FAIL > /tmp/r_$n; exit; fi
      if ! ps -p "$pid" > /dev/null 2>&1; then echo DEAD > /tmp/r_$n; exit; fi
      sleep 1
    done
    echo TIMEOUT > /tmp/r_$n
  ) > /dev/null 2>&1 &
}

waitr() {  # $1 = 序号
  for i in $(seq 1 200); do
    [ -f /tmp/r_$1 ] && { cat /tmp/r_$1; return; }
    sleep 1
  done
  echo TIMEOUT
}

echo "══════════════ 实验 A：干净环境能塞几个 rkllm ══════════════"
cleanall
echo "  清场: $(pgrep -af 'rkllm_embodied_agent|edgevision_dashboard' || echo 无) | CmaFree=$(awk '/CmaFree/{print $2}' /proc/meminfo)kB"
F0=$(FAILS); echo "  基线失败次数=$F0"
CAP=0
for N in 1 2 3 4; do
  MARK=$(DSM); FB=$(FAILS)
  spawn $N
  R=$(waitr $N)
  PID=$(cat /tmp/g$N.pid 2>/dev/null)
  echo "  ── 第 $N 个: $R  PID=$PID  RSS=$(awk '/VmRSS/{print $2$3}' /proc/$PID/status 2>/dev/null || echo -)"
  if [ "$R" = "OK" ]; then
    CAP=$N
  elif [ "$R" = "DEAD" ]; then
    echo "     [诊断] 进程非正常退出，日志尾部："
    tail -15 /tmp/g$N.log | sed 's/^/       /'
    echo "     [诊断] dmesg 新增（找 OOM/segfault）："
    dmesg 2>/dev/null | tail -n +$((MARK+1)) | grep -iE "oom|kill|segfault|rkllm|rknpu" | tail -10 | sed 's/^/       /'
    break
  else
    SZ=$(grep 'failed to malloc npu memory' /tmp/g$N.log | sed 's/.*size: \([0-9]*\).*/\1/')
    echo "     失败请求 size = ${SZ} bytes ≈ $(awk "BEGIN{printf \"%.1f\", ${SZ:-0}/1048576}") MiB"
    grep -E "failed to allocate|failed to malloc|load model file error" /tmp/g$N.log | sed 's/^/       /'
    echo "     【本次新增 dmesg】"
    dmesg 2>/dev/null | tail -n +$((MARK+1)) | grep -E "^\s*[0-9]+: \[|^\s*used:|dma map" | tail -40 | sed 's/^/       /'
    break
  fi
  sleep 3
done
echo "  >>> 干净环境最多 $CAP 个 rkllm 并存（第 $((CAP+1)) 个失败）"
echo "  >>> 累计失败次数 = $(FAILS)"

echo
echo "══════════════ 实验 B：dashboard 在跑时能塞几个 ══════════════"
cleanall
cd /root/edgevision
nohup ./deploy/cpp/build_release/edgevision_dashboard --port 8090 >/tmp/gdash.log 2>&1 &
DPID=$!
cd /root/rkllm_deploy
sleep 40
echo "  dashboard PID=$DPID RSS=$(awk '/VmRSS/{print $2$3}' /proc/$DPID/status 2>/dev/null) NPU=$(cat /sys/kernel/debug/rknpu/load | tr -d '\n')"
CAPB=0
for N in 1 2 3; do
  MARK=$(DSM)
  spawn $N
  R=$(waitr $N)
  PID=$(cat /tmp/g$N.pid 2>/dev/null)
  echo "  ── [dashboard 在跑] 第 $N 个 rkllm: $R  RSS=$(awk '/VmRSS/{print $2$3}' /proc/$PID/status 2>/dev/null || echo -)"
  if [ "$R" = "OK" ]; then
    CAPB=$N
  else
    SZ=$(grep 'failed to malloc npu memory' /tmp/g$N.log | sed 's/.*size: \([0-9]*\).*/\1/')
    echo "     失败请求 size = ${SZ} bytes ≈ $(awk "BEGIN{printf \"%.1f\", ${SZ:-0}/1048576}") MiB"
    grep -E "failed to allocate|failed to malloc" /tmp/g$N.log | sed 's/^/       /'
    echo "     【本次新增 dmesg】"
    dmesg 2>/dev/null | tail -n +$((MARK+1)) | grep -E "^\s*[0-9]+: \[|^\s*used:|dma map" | tail -40 | sed 's/^/       /'
    break
  fi
  sleep 3
done
echo "  >>> dashboard 在跑时最多 $CAPB 个 rkllm"

echo
echo "══════════════ 恢复 ══════════════"
cleanall
systemctl start edgevision.service 2>/dev/null
sleep 3
echo "  gateway=$(systemctl is-active edgevision.service) dashboard=$(pgrep -f edgevision_dashboard >/dev/null && echo YES || echo NO)"
echo "  剩余 rkllm: $(pgrep -af rkllm_embodied_agent || echo 无)"
echo "  累计失败次数 = $(FAILS)"
echo "===== DONE ====="
