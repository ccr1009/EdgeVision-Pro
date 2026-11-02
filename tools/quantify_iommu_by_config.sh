#!/bin/bash
# 定量实验：不同配置下 NPU IOMMU 的「已用量 used」到底是多少
# 手法：每个配置都持续起 rkllm 直到失败，读取失败时驱动打印的 used 值
cd /root/rkllm_deploy
export LD_LIBRARY_PATH=/root/rkllm_deploy
DSM() { dmesg 2>/dev/null | wc -l; }

cleanall() {
  pkill -9 -f rkllm_embodied_agent 2>/dev/null
  pkill -9 -f "tail -f /dev/null" 2>/dev/null
  pkill -f edgevision_dashboard 2>/dev/null
  systemctl stop edgevision.service 2>/dev/null
  sleep 4
}

# 起一个实例，等到 OK/FAIL，输出结果
tryone() {
  local tag=$1
  rm -f /tmp/q.log
  tail -f /dev/null | ./rkllm_embodied_agent > /tmp/q.log 2>&1 &
  sleep 1
  local pid=$(pgrep -f "rkllm_embodied_agent$" | tail -1)
  local r=""
  for i in $(seq 1 150); do
    grep -q "已就绪" /tmp/q.log 2>/dev/null && { r=OK; break; }
    grep -q "rkllm_init failed" /tmp/q.log 2>/dev/null && { r=FAIL; break; }
    ps -p $pid >/dev/null 2>&1 || { r=DEAD; break; }
    sleep 1
  done
  echo "$r"
}

# 反复起实例直到失败，打印失败时 used 值
fill_until_fail() {
  local lbl=$1
  local mark=$(DSM)
  local n=0
  while [ $n -lt 5 ]; do
    n=$((n+1))
    local r=$(tryone $n)
    if [ "$r" = "OK" ]; then
      echo "    [$lbl] 第 $n 个: OK"
      sleep 2
    else
      local sz=$(grep 'failed to malloc npu memory' /tmp/q.log | sed 's/.*size: \([0-9]*\).*/\1/')
      local used=$(dmesg 2>/dev/null | tail -n +$((mark+1)) | grep 'used:' | tail -1 | sed 's/.*used: //')
      echo "    [$lbl] 第 $n 个: $r  失败请求=$sz 字节 (≈$(awk "BEGIN{printf \"%.1f\", ${sz:-0}/1048576}")MiB)  失败时 used=$used"
      return 0
    fi
  done
  echo "    [$lbl] 5 个都成功了（没触发失败，无法读 used）"
}

echo "════════════════════════════════════════════════════════"
echo " 实验 1：完全干净（无 gateway / 无 dashboard）"
echo "════════════════════════════════════════════════════════"
cleanall
fill_until_fail "干净"
cleanall

echo
echo "════════════════════════════════════════════════════════"
echo " 实验 2：只开 gateway（systemd，yolov8n 3 context）"
echo "════════════════════════════════════════════════════════"
cleanall
systemctl start edgevision.service 2>/dev/null
sleep 12
echo "    gateway=$(systemctl is-active edgevision.service) RSS=$(pgrep -f w4_edgevision_gateway | head -1 | xargs -I{} awk '/VmRSS/{print $2$3}' /proc/{}/status 2>/dev/null)"
fill_until_fail "gateway"
cleanall

echo
echo "════════════════════════════════════════════════════════"
echo " 实验 3：gateway + dashboard（= 用户 21:07 的配置）"
echo "════════════════════════════════════════════════════════"
cleanall
systemctl start edgevision.service 2>/dev/null
sleep 8
cd /root/edgevision
nohup ./deploy/cpp/build_release/edgevision_dashboard --port 8090 >/tmp/qdash.log 2>&1 &
DPID=$!
cd /root/rkllm_deploy
sleep 40
echo "    gateway=$(systemctl is-active edgevision.service) dashboard RSS=$(awk '/VmRSS/{print $2$3}' /proc/$DPID/status 2>/dev/null)"
fill_until_fail "gw+dash"
cleanall

echo
echo "════════════════════════════════════════════════════════"
echo " 实验 4：板上的 .cpp 源码是不是「带 fork/exec 修复」的新版？"
echo "════════════════════════════════════════════════════════"
echo "  --- 板上源码是否含 fork() ---"
grep -n "fork()" /root/rkllm_deploy/rkllm_embodied_agent.cpp 2>/dev/null | head -5 || echo "    （无 fork —— 是旧版 timer 实现）"
echo "  --- 板上源码 LED 相关行 ---"
grep -n -E "delay_on|delay_off|trigger|timer" /root/rkllm_deploy/rkllm_embodied_agent.cpp 2>/dev/null | head -12
echo "  --- 二进制大小与时间 ---"
ls -l --time-style=long-iso /root/rkllm_deploy/rkllm_embodied_agent 2>/dev/null | sed 's/^/    /'
echo "  --- 二进制里是否有 fork 爆闪特征（while true / sleep 0.1）---"
strings /root/rkllm_deploy/rkllm_embodied_agent 2>/dev/null | grep -E "while true|sleep 0\.1|edgevision_led_alarm" | head -5 || echo "    （无 → 旧版）"

echo
echo "════════════════════════════════════════════════════════"
echo " 恢复现场"
echo "════════════════════════════════════════════════════════"
cleanall
systemctl start edgevision.service 2>/dev/null
sleep 3
echo "  gateway=$(systemctl is-active edgevision.service)"
echo "  累计 dma map 失败: $(dmesg 2>/dev/null | grep -c 'dma map')"
echo "===== DONE ====="
