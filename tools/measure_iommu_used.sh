#!/bin/bash
# 关键测量：加载 1 个 rkllm 实例后，NPU IOMMU 到底占了多少？
# 手法：先跑一个实例常驻，再故意触发第二次分配失败，让驱动把 IOMMU 空间 dump 出来
# 注意：只抓「本次新增」的 dmesg 行，避免像上次那样误读历史日志
cd /root/rkllm_deploy
export LD_LIBRARY_PATH=/root/rkllm_deploy

cleanup() {
  pkill -9 -f rkllm_embodied_agent 2>/dev/null
  pkill -f edgevision_dashboard 2>/dev/null
  systemctl stop edgevision.service 2>/dev/null
  sleep 3
}

echo "############ 前置清理 ############"
cleanup
echo "  进程: $(pgrep -af 'rkllm|edgevision' | grep -v grep || echo 无)"
M0=$(dmesg 2>/dev/null | wc -l)
echo "  dmesg 基线行号 = $M0"

echo
echo "############ 步骤 A：先触发一次「空环境」失败，拿到基线 IOMMU 占用 ############"
echo "  （用一个超大的假分配参数不行，改直接跑第二个实例来逼驱动 dump）"
mkfifo /tmp/fA 2>/dev/null
( sleep 400 > /tmp/fA & )
./rkllm_embodied_agent < /tmp/fA > /tmp/A.log 2>&1 &
PID_A=$!
sleep 80
echo "  实例A PID=$PID_A 存活=$(ps -p $PID_A -o stat= 2>/dev/null) RSS=$(awk '/VmRSS/{print $2$3}' /proc/$PID_A/status 2>/dev/null)"
grep -q "已就绪" /tmp/A.log && echo "  实例A 加载成功 OK" || echo "  实例A 加载失败 FAIL"

MA=$(dmesg 2>/dev/null | wc -l)
echo "  现在再起第二个实例，逼驱动打印 IOMMU dump（新增 dmesg 从第 $MA 行开始）..."
echo "quit" | timeout 200 ./rkllm_embodied_agent > /tmp/B.log 2>&1
echo "  --- 第二个实例的输出 ---"
grep -E "failed to allocate|failed to malloc|rkllm_init failed|已就绪" /tmp/B.log | sed 's/^/     /'
echo "  --- 【新增】dmesg 里驱动打印的 IOMMU 空间（第一次加载后 + 第二次尝试时）---"
dmesg 2>/dev/null | tail -n +$((MA+1)) | grep -E "^\s*[0-9]+: \[|^\s*used:|dma map|dma_map" | tail -45

echo
echo "############ 步骤 B：杀掉实例A，再看 IOMMU 是否释放 ############"
kill -9 $PID_A 2>/dev/null
sleep 8
pkill -f "sleep 400" 2>/dev/null; rm -f /tmp/fA
echo "  实例A 状态: $(ps -p $PID_A -o stat= 2>/dev/null || echo 已死)"
MC=$(dmesg 2>/dev/null | wc -l)
echo "  再起实例，看能否成功（若成功=IOMMU 已释放）:"
echo "quit" | timeout 200 ./rkllm_embodied_agent > /tmp/C.log 2>&1
grep -q "已就绪" /tmp/C.log && echo "    ✅ 成功 —— kill -9 后 IOMMU 已释放" || echo "    ❌ 失败 —— 疑似泄漏"
grep -E "failed to|已就绪" /tmp/C.log | sed 's/^/       /'

echo
echo "############ 恢复 ############"
cleanup
systemctl start edgevision.service 2>/dev/null
sleep 3
echo "  gateway=$(systemctl is-active edgevision.service)"
echo "===== DONE ====="
