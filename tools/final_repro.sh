#!/bin/bash
# 最终验证：复现用户真实场景 + 测"减小 context"是否规避
cd /root/rkllm_deploy
M=/root/rkllm_deploy/qwen3.5-2b_w8a8_rk3588.rkllm

echo "############ 场景：gateway + dashboard 同时跑（= 用户 21:07 的真实状态）############"
pkill -f edgevision_dashboard 2>/dev/null
systemctl stop edgevision.service 2>/dev/null
sleep 3
systemctl start edgevision.service 2>/dev/null
cd /root/edgevision && nohup ./deploy/cpp/build_release/edgevision_dashboard --port 8090 >/tmp/dash.log 2>&1 &
sleep 15
cd /root/rkllm_deploy
echo "  gateway=$(systemctl is-active edgevision.service) dashboard=$(pgrep -f edgevision_dashboard >/dev/null && echo YES || echo NO)"
echo "  NPU load: $(cat /sys/kernel/debug/rknpu/load | tr -d '\n')"
echo "  MemAvailable=$(awk '/MemAvailable/{print $2}' /proc/meminfo)kB"

M1=$(dmesg | wc -l)
echo
echo "---- 尝试 1：完整 agent (3核 + 16384 context) ----"
OUT=$(echo "quit" | timeout 180 ./rkllm_embodied_agent 2>&1)
echo "$OUT" | grep -q "已就绪" && echo "  ✅ 成功" || echo "  ❌ 失败"
echo "$OUT" | grep -E "failed to allocate|failed to malloc|rkllm_init" | sed 's/^/     /'
echo "  --- 失败后驱动打印的 IOMMU 占用（关键数字）---"
dmesg | sed -n "$((M1+1)),\$p" | grep -E "used:|fail" | tail -3 | sed 's/^/     /'

M2=$(dmesg | wc -l)
echo
echo "---- 尝试 2：减小 context 到 2048 再试 ----"
OUT2=$(echo "quit" | timeout 300 ./llm_demo "$M" 64 2048 2>&1)
echo "$OUT2" | tail -6 | sed 's/^/     /'
echo "  --- 驱动新增 ---"
dmesg | sed -n "$((M2+1)),\$p" | grep -E "used:|fail" | tail -3 | sed 's/^/     /'

echo
echo "############ 清理：停掉 dashboard，只留 gateway ############"
pkill -f edgevision_dashboard 2>/dev/null
systemctl start edgevision.service 2>/dev/null
sleep 3
echo "  gateway=$(systemctl is-active edgevision.service) dashboard=$(pgrep -f edgevision_dashboard >/dev/null && echo YES || echo NO)"
echo "  NPU load: $(cat /sys/kernel/debug/rknpu/load | tr -d '\n')"
echo "===== DONE ====="
