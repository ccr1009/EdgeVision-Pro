#!/bin/bash
# 决定性实验 B：验证 NPU IOMMU 4GB 空间耗尽的机制
# 手法：跑一个常驻 rkllm 实例，再起第二个 —— 看第二个是否报同样的错，并拿到 "used:" 值
cd /root/rkllm_deploy

cleanup() {
  pkill -f "rkllm_embodied_agent" 2>/dev/null
  sleep 2
}
cleanup
pkill -f edgevision_dashboard 2>/dev/null
systemctl stop edgevision.service 2>/dev/null
sleep 3

echo "===== 前置：确认环境干净 ====="
echo "  rkllm 进程: $(pgrep -af rkllm_embodied_agent || echo 无)"
echo "  dashboard: $(pgrep -af edgevision_dashboard || echo 无)"
echo "  gateway: $(systemctl is-active edgevision.service)"
echo "  CmaFree=$(awk '/CmaFree/{print $2}' /proc/meminfo)kB  MemAvailable=$(awk '/MemAvailable/{print $2}' /proc/meminfo)kB"

echo
echo "===== 第 1 个实例：后台常驻（保持 stdin 打开让它一直活着）====="
MARK=$(dmesg 2>/dev/null | wc -l)
mkfifo /tmp/fifo1 2>/dev/null
( sleep 300 > /tmp/fifo1 & ) 
./rkllm_embodied_agent < /tmp/fifo1 > /tmp/agent1.log 2>&1 &
A1=$!
sleep 60
echo "  实例1 PID=$A1 存活=$(ps -p $A1 -o stat= 2>/dev/null || echo 死)"
grep -E "已就绪|failed" /tmp/agent1.log | head -3
echo "  实例1 的 RSS=$(awk '/VmRSS/{print $2$3}' /proc/$A1/status 2>/dev/null)"
echo "  实例1 持有的 renderD129 fd 数 = $(ls -l /proc/$A1/fd 2>/dev/null | grep -c renderD129)"

echo
echo "===== 第 2 个实例：同时再加载一个（预期：NPU 内存不够）====="
OUT2=$(echo "quit" | timeout 180 ./rkllm_embodied_agent 2>&1)
echo "$OUT2" | tail -10 | sed 's/^/    /'
if echo "$OUT2" | grep -q "已就绪"; then echo "  >>> 第2个实例居然成功了（说明 2 个 2.9GB 模型能塞进 4GB？不可能，请复查）"; else echo "  >>> 第2个实例失败（符合 NPU 内存耗尽预期）"; fi

echo
echo "===== 第 2 个实例失败时驱动打印的 IOMMU 占用 ====="
dmesg 2>/dev/null | sed -n "$((MARK+1)),\$p" | grep -E "^\[.*\] *(used:|[0-9]+: \[)" | tail -40

echo
echo "===== 清理 ====="
cleanup
rm -f /tmp/fifo1
systemctl start edgevision.service 2>/dev/null
sleep 2
echo "  gateway = $(systemctl is-active edgevision.service)"
echo "===== DONE ====="
