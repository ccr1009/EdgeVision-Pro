#!/bin/bash
# 探针法精确测量：每个进程占用多少 NPU IOMMU 地址空间
# 原理：驱动只在分配失败时 dump IOMMU 已用总量，所以用"撑到失败"来读总量
cd /root/rkllm_deploy
export LD_LIBRARY_PATH=/root/rkllm_deploy
DSM() { dmesg 2>/dev/null | wc -l; }

spawn_agent() {
  local n=$1
  local fifo=/tmp/fs$n
  mkfifo $fifo 2>/dev/null
  ( sleep 900 > $fifo & )
  ./rkllm_embodied_agent < $fifo > /tmp/s$n.log 2>&1 &
  echo $! > /tmp/s$n.pid
}
wait_result() {
  local n=$1 pid=$(cat /tmp/s$n.pid 2>/dev/null)
  for i in $(seq 1 150); do
    grep -q "已就绪" /tmp/s$n.log 2>/dev/null && { echo OK; return; }
    grep -q "rkllm_init failed" /tmp/s$n.log 2>/dev/null && { echo FAIL; return; }
    ps -p $pid >/dev/null 2>&1 || { echo DEAD; return; }
    sleep 1
  done
  echo TIMEOUT
}
cleanall() {
  pkill -9 -f rkllm_embodied_agent 2>/dev/null
  pkill -f "sleep 900" 2>/dev/null
  pkill -f edgevision_dashboard 2>/dev/null
  systemctl stop edgevision.service 2>/dev/null
  rm -f /tmp/fs* 
  sleep 4
}
show_fail_dump() {
  local mark=$1
  echo "     【本次新增 dmesg 全部内容】"
  dmesg 2>/dev/null | tail -n +$((mark+1)) | grep -vE "^\[.*\] (usb|mmc|mali|rockchip-vop)" | tail -50 | sed 's/^/       /'
}

echo "════════════════════════════════════════════════════"
echo " 实验 A：干净环境，能塞下几个 rkllm？"
echo "════════════════════════════════════════════════════"
cleanall
echo "  清场后: $(pgrep -af 'rkllm_embodied_agent|edgevision_dashboard' || echo 无进程)"
CNT=0
for N in 1 2 3 4; do
  MARK=$(DSM)
  spawn_agent $N
  R=$(wait_result $N)
  echo "  第 $N 个: $R"
  if [ "$R" = "OK" ]; then
    CNT=$N
  else
    echo "   失败 size: $(grep 'failed to malloc npu memory' /tmp/s$N.log | sed 's/.*size: \([0-9]*\).*/\1/') bytes"
    grep -E "failed to allocate|failed to malloc" /tmp/s$N.log | sed 's/^/     /'
    show_fail_dump $MARK
    break
  fi
  sleep 3
done
echo "  >>> 干净环境最多能塞 $CNT 个 rkllm 实例"
echo "  （若第 N+1 个失败且有 dump，则总占用量接近 4096MiB）"

echo
echo "════════════════════════════════════════════════════"
echo " 实验 B：dashboard 在跑时，能塞下几个 rkllm？（测 dashboard 占多少）"
echo "════════════════════════════════════════════════════"
cleanall
cd /root/edgevision
nohup ./deploy/cpp/build_release/edgevision_dashboard --port 8090 >/tmp/dashA.log 2>&1 &
DPID=$!
cd /root/rkllm_deploy
sleep 45
echo "  dashboard PID=$DPID RSS=$(awk '/VmRSS/{print $2$3}' /proc/$DPID/status 2>/dev/null) 存活=$(ps -p $DPID -o stat= 2>/dev/null)"
grep -qE "已就绪|listening|8080|8090" /tmp/dashA.log && echo "  dashboard 已起动: $(grep -oE 'listening[^,]*|8080|8090' /tmp/dashA.log | head -1)"
echo "  NPU: $(cat /sys/kernel/debug/rknpu/load | tr -d '\n')"

CNT_B=0
for N in 1 2 3; do
  MARK=$(DSM)
  spawn_agent $N
  R=$(wait_result $N)
  echo "  [dashboard 在跑] 第 $N 个 rkllm: $R"
  if [ "$R" = "OK" ]; then
    CNT_B=$N
  else
    echo "   失败 size: $(grep 'failed to malloc npu memory' /tmp/s$N.log | sed 's/.*size: \([0-9]*\).*/\1/') bytes"
    grep -E "failed to allocate|failed to malloc" /tmp/s$N.log | sed 's/^/     /'
    show_fail_dump $MARK
    break
  fi
  sleep 3
done
echo "  >>> dashboard 在跑时最多能塞 $CNT_B 个 rkllm"

echo
echo "════════════════════════════════════════════════════"
echo " 恢复现场"
echo "════════════════════════════════════════════════════"
cleanall
systemctl start edgevision.service 2>/dev/null
sleep 3
echo "  gateway=$(systemctl is-active edgevision.service)  dashboard=$(pgrep -f edgevision_dashboard >/dev/null && echo YES || echo NO)"
echo "  CmaFree=$(awk '/CmaFree/{print $2}' /proc/meminfo)kB"
echo "  历史累计分配失败: $(dmesg 2>/dev/null | grep -c 'rknpu_gem_get_pages: dma map') 次"
echo "===== DONE ====="
