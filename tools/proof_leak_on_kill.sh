#!/bin/bash
# 核心假设验证：rkllm 进程被强杀/异常退出后，NPU IOMMU 映射是否泄漏？
cd /root/rkllm_deploy
export LD_LIBRARY_PATH=/root/rkllm_deploy

load_ok() {
  local out
  out=$(echo "quit" | timeout 200 ./rkllm_embodied_agent 2>&1)
  if echo "$out" | grep -q "已就绪"; then echo OK; else echo FAIL; fi
}

echo "############ 前置：清干净 ############"
pkill -9 -f rkllm_embodied_agent 2>/dev/null
pkill -f edgevision_dashboard 2>/dev/null
systemctl stop edgevision.service 2>/dev/null
sleep 4
echo "  gateway=$(systemctl is-active edgevision.service) dashboard=$(pgrep -f edgevision_dashboard >/dev/null && echo YES || echo NO) rkllm=$(pgrep -f rkllm_embodied_agent || echo none)"
echo "  加载测试: $(load_ok)"

echo
echo "############ 实验 1：实例常驻后 kill -9（模拟崩溃/被强杀）############"
mkfifo /tmp/f1 2>/dev/null
( sleep 600 > /tmp/f1 & )
./rkllm_embodied_agent < /tmp/f1 > /tmp/a1.log 2>&1 &
A1=$!
sleep 75
echo "  实例 PID=$A1 状态=$(ps -p $A1 -o stat= 2>/dev/null) RSS=$(awk '/VmRSS/{print $2$3}' /proc/$A1/status 2>/dev/null)"
grep -q "已就绪" /tmp/a1.log && echo "  实例加载成功 OK" || echo "  实例加载失败 FAIL"
echo "  >>> kill -9 $A1"
kill -9 $A1
sleep 5
pkill -f "sleep 600" 2>/dev/null
rm -f /tmp/f1
echo "  强杀后 rkllm 进程: $(pgrep -af rkllm_embodied_agent || echo 无)"
echo "  >>> 强杀后再加载: $(load_ok)"

echo
echo "############ 实验 2：实例常驻后 SIGTERM（正常退出）############"
sleep 3
mkfifo /tmp/f2 2>/dev/null
( sleep 600 > /tmp/f2 & )
./rkllm_embodied_agent < /tmp/f2 > /tmp/a2.log 2>&1 &
A2=$!
sleep 75
echo "  实例2 PID=$A2 加载: $(grep -q 已就绪 /tmp/a2.log && echo 成功 || echo 失败)"
echo "  >>> SIGTERM $A2"
kill -TERM $A2 2>/dev/null
sleep 5
pkill -f "sleep 600" 2>/dev/null; rm -f /tmp/f2
echo "  退出后残留: $(pgrep -af rkllm_embodied_agent || echo 无)"
echo "  >>> SIGTERM 后再加载: $(load_ok)"

echo
echo "############ 恢复 ############"
pkill -9 -f rkllm_embodied_agent 2>/dev/null
systemctl start edgevision.service 2>/dev/null
sleep 3
echo "  gateway=$(systemctl is-active edgevision.service)"
echo "===== DONE ====="
