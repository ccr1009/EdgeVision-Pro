#!/bin/bash
# 精确定位：dashboard 运行时 rkllm 何时可用、何时失效
cd /root/rkllm_deploy

dmesg_mark() { dmesg 2>/dev/null | wc -l; }

try_load() {
  local tag="$1"
  local out
  out=$(echo "quit" | timeout 150 ./rkllm_embodied_agent 2>&1)
  if echo "$out" | grep -q "已就绪"; then
    echo "  [$tag] ✅ 成功"
  else
    echo "  [$tag] ❌ 失败 —— 完整错误输出:"
    echo "$out" | tail -12 | sed 's/^/       /'
  fi
}

# 只留 dashboard，清掉 gateway
pkill -f edgevision_dashboard 2>/dev/null
systemctl stop edgevision.service 2>/dev/null
sleep 4

echo "===== 启动 dashboard，观察 IOMMU 空间随时间的增长 ====="
cd /root/edgevision
nohup ./deploy/cpp/build_release/edgevision_dashboard --port 8090 >/tmp/dash.log 2>&1 &
DPID=$!
echo "dashboard PID=$DPID"
cd /root/rkllm_deploy

for t in 0 30 60 120 180; do
  if [ "$t" -gt 0 ]; then sleep $(( t - last )); fi
  last=$t
  echo
  echo "---------- dashboard 已运行 ${t}s ----------"
  echo "  RSS=$(awk '/VmRSS/{print $2$3}' /proc/$DPID/status 2>/dev/null)  stat=$(ps -p $DPID -o stat= 2>/dev/null)"
  echo "  renderD129 fd 数: $(ls -l /proc/$DPID/fd 2>/dev/null | grep -c renderD129)"
  try_load "t=${t}s"
done

echo
echo "===== 清理恢复 ====="
kill -9 $DPID 2>/dev/null
sleep 2
systemctl start edgevision.service 2>/dev/null
sleep 3
echo "edgevision.service = $(systemctl is-active edgevision.service)"
echo "===== DONE ====="
