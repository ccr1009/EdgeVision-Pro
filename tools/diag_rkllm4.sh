#!/bin/bash
echo "===== A. 失败时刻 dmesg 的原始前几行（看是谁打印的 IOVA dump）====="
dmesg 2>/dev/null | sed -n '4265,4280p'
echo
echo "===== B. dashboard 源码里 RKNN 是初始化一次还是每帧都建 ====="
grep -n -E "rknn_init|init\(|release|RKNNEngineDflFree [a-z_]*;|new RKNN" /root/edgevision/deploy/cpp/src/edgevision_dashboard_main.cpp 2>/dev/null | head -20
echo
echo "===== C. rkllm 模型文件大小 vs 失败时申请的 196MB ====="
ls -l /root/rkllm_deploy/qwen3.5-2b_w8a8_rk3588.rkllm
echo "  205799424 bytes = $(awk 'BEGIN{printf "%.1f", 205799424/1048576}') MiB"
echo "  3915 MiB + 196 MiB = $(awk 'BEGIN{printf "%d", 3915+196.3}') MiB  (IOMMU 32-bit 上限 = 4096 MiB)"
echo
echo "===== D. 当前能否稳定加载：连跑 3 次 ====="
cd /root/rkllm_deploy
for i in 1 2 3; do
  out=$(echo "quit" | timeout 180 ./rkllm_embodied_agent 2>&1)
  echo "$out" | grep -q "已就绪" && echo "  第 $i 次: ✅ 成功" || { echo "  第 $i 次: ❌ 失败"; echo "$out" | grep -E "failed|error" | head -4; }
done
echo
echo "===== E. NPU IOMMU 相关内核配置 ====="
zcat /proc/config.gz 2>/dev/null | grep -E "CONFIG_RKNPU|CONFIG_ROCKCHIP_IOMMU|CONFIG_IOMMU_SUPPORT|CONFIG_DMA_API_DEBUG" || echo "(无 config.gz)"
echo "===== DONE ====="
