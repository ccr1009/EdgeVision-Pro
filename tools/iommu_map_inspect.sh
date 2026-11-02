#!/bin/bash
echo "══════════ A. IOMMU debugfs 有没有可读的映射表 ══════════"
ls -la /sys/kernel/debug/iommu/ 2>/dev/null | sed 's/^/  /'
find /sys/kernel/debug -maxdepth 2 -name "*iommu*" -o -maxdepth 2 -name "*rknpu*" 2>/dev/null | sed 's/^/  /'
echo "--- 各 iommu 设备目录 ---"
for d in /sys/kernel/debug/iommu/*/; do
  [ -d "$d" ] && echo "  $d: $(ls $d 2>/dev/null | tr '\n' ' ')"
done

echo
echo "══════════ B. dma_buf 总览（IOVA 的真正来源）══════════"
cat /sys/kernel/debug/dma_buf/bufinfo 2>/dev/null | head -60 | sed 's/^/  /'
echo "  --- buf 总数: $(grep -c '^size' /sys/kernel/debug/dma_buf/bufinfo 2>/dev/null) ---"

echo
echo "══════════ C. 手动撑爆一次，拿最新的 used 值作为「当前占用」基准 ══════════"
cd /root/rkllm_deploy
export LD_LIBRARY_PATH=/root/rkllm_deploy
pkill -9 -f rkllm_embodied_agent 2>/dev/null
pkill -f edgevision_dashboard 2>/dev/null
systemctl stop edgevision.service 2>/dev/null
sleep 5
echo "  清场后 NPU load: $(cat /sys/kernel/debug/rknpu/load | tr -d '\n')"
echo "  清场后 dma_buf 数: $(grep -c '^size' /sys/kernel/debug/dma_buf/bufinfo 2>/dev/null)"

echo "  起 1 个实例后，再起第 2 个逼它失败 → 读出 used ---"
tail -f /dev/null | ./rkllm_embodied_agent > /tmp/m1.log 2>&1 &
sleep 1; P1=$(pgrep -f "rkllm_embodied_agent$" | tail -1)
for i in $(seq 1 120); do grep -q 已就绪 /tmp/m1.log && break; sleep 1; done
echo "    实例1: $(grep -q 已就绪 /tmp/m1.log && echo OK || echo FAIL) PID=$P1 RSS=$(awk '/VmRSS/{print $2$3}' /proc/$P1/status 2>/dev/null)"
echo "    dma_buf 数: $(grep -c '^size' /sys/kernel/debug/dma_buf/bufinfo 2>/dev/null)"

M=$(dmesg | wc -l)
tail -f /dev/null | ./rkllm_embodied_agent > /tmp/m2.log 2>&1 &
sleep 1; P2=$(pgrep -f "rkllm_embodied_agent$" | tail -1)
for i in $(seq 1 120); do
  grep -q 已就绪 /tmp/m2.log 2>/dev/null && break
  grep -q "rkllm_init failed" /tmp/m2.log 2>/dev/null && break
  sleep 1
done
echo "    实例2: $(grep -q 已就绪 /tmp/m2.log && echo 'OK —— 1个实例占不满' || echo 'FAIL —— 说明1个实例已占掉大部分')"
grep -E "failed to malloc|已就绪" /tmp/m2.log | sed 's/^/      /'
echo "    【新增 dmesg 的 used 行】"
dmesg | tail -n +$((M+1)) | grep -E "used:|dma map" | sed 's/^/      /'
echo "    dma_buf 数: $(grep -c '^size' /sys/kernel/debug/dma_buf/bufinfo 2>/dev/null)"

echo
echo "══════════ D. 恢复 ══════════"
pkill -9 -f rkllm_embodied_agent 2>/dev/null
pkill -9 -f "tail -f /dev/null" 2>/dev/null
sleep 4
systemctl start edgevision.service 2>/dev/null
sleep 2
echo "  gateway=$(systemctl is-active edgevision.service)"
echo "  累计 dma map 失败: $(dmesg | grep -c 'dma map')"
echo "===== DONE ====="
