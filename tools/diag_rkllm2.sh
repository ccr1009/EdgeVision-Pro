#!/bin/bash
echo "===== A. dma_buf 占用明细（谁在吃 CMA/DMA）====="
if [ -r /sys/kernel/debug/dma_buf/bufinfo ]; then
  echo "--- 按 size 排序的 buffer（前 25）---"
  grep -E "^(size|Name|Count|pid)" /sys/kernel/debug/dma_buf/bufinfo | head -5
  echo "--- 总 buffer 数 ---"
  grep -c "^size" /sys/kernel/debug/dma_buf/bufinfo
  echo "--- 各 size 汇总 ---"
  grep "^size" /sys/kernel/debug/dma_buf/bufinfo | awk '{s+=$2; n++} END {printf "total_buffers=%d total_bytes=%d (%.1f MB)\n", n, s, s/1048576}'
  echo "--- 前 20 个最大的 buffer 及归属 ---"
  awk '/^size/{sz=$2; getline; nm=$0; print sz" "nm}' /sys/kernel/debug/dma_buf/bufinfo | sort -rn | head -20
else
  echo "(读不到 dma_buf/bufinfo)"
fi

echo
echo "===== B. 各进程 dma_buf / CMA 占用（按 RSS + 是否开着 renderD129）====="
for p in /proc/[0-9]*; do
  pid=${p#/proc/}
  [ -d "$p" ] || continue
  if ls -l $p/fd 2>/dev/null | grep -qE "renderD129|renderD128|/dev/dma_heap"; then
    echo "PID $pid  $(tr '\0' ' ' < $p/cmdline 2>/dev/null | cut -c1-70)"
  fi
done

echo
echo "===== C. MPP/VPU 相关 debugfs ====="
ls /sys/kernel/debug/mpp_service/ 2>/dev/null | head
for f in /sys/kernel/debug/mpp_service/session_summary /sys/kernel/debug/mpp_service/sessions; do
  [ -r "$f" ] && { echo "--- $f ---"; head -40 "$f"; }
done
echo "--- VPU 设备节点占用 ---"
for p in /proc/[0-9]*; do
  pid=${p#/proc/}
  if ls -l $p/fd 2>/dev/null | grep -q "mpp_service"; then
    echo "PID $pid  $(tr '\0' ' ' < $p/cmdline 2>/dev/null | cut -c1-70)"
  fi
done

echo
echo "===== D. 动态库加载路径（rkllm 怎么找到 librkllmrt.so 的）====="
cat /etc/ld.so.conf.d/*.conf 2>/dev/null
echo "--- ld.so.cache 里有没有 rkllm ---"
ldconfig -p 2>/dev/null | grep -i rkllm || echo "(cache 里没有 librkllmrt.so)"
echo "--- ELF 头 rpath/runpath ---"
readelf -d /root/rkllm_deploy/rkllm_embodied_agent 2>/dev/null | grep -E "RPATH|RUNPATH|NEEDED"
readelf -d /root/rkllm_deploy/llm_demo 2>/dev/null | grep -E "RPATH|RUNPATH|NEEDED"

echo
echo "===== E. 历史启动方式 ====="
cat /root/rkllm_deploy/build_and_run_agent.sh 2>/dev/null
echo "--- systemd 里有没有 rkllm 服务 ---"
systemctl list-units --type=service --all 2>/dev/null | grep -iE "edgevision|rkllm|llm"

echo
echo "===== F. CMA 分区参数（bootargs / dts）====="
cat /proc/cmdline
echo "--- reserved-memory 里的 cma 节点 ---"
ls /sys/firmware/devicetree/base/reserved-memory/ 2>/dev/null | head -20
for d in /sys/firmware/devicetree/base/reserved-memory/*cma*; do
  [ -e "$d" ] || continue
  echo "== $d"
  od -An -tx1 "$d/reg" 2>/dev/null | head -2
done
echo "===== DONE ====="
