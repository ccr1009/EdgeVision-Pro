#!/bin/bash
# rkllm_init failed 诊断脚本
echo "===== 1. 系统 / 负载 ====="
uname -a
uptime
echo
echo "--- top 15 CPU 进程 ---"
ps -eo pid,ppid,pcpu,pmem,rss,etimes,comm --sort=-pcpu | head -16
echo
echo "--- 全进程数 ---"
ps -e --no-headers | wc -l

echo
echo "===== 2. NPU 驱动 ====="
echo "--- rknpu version ---"
cat /sys/kernel/debug/rknpu/version 2>/dev/null || echo "(读不到 /sys/kernel/debug/rknpu/version)"
echo "--- rknpu load ---"
cat /sys/kernel/debug/rknpu/load 2>/dev/null || echo "(读不到 load)"
echo "--- rknpu freq/volt ---"
cat /sys/class/devfreq/fdab0000.npu/cur_freq 2>/dev/null
echo "--- rknpu 相关内核模块 ---"
lsmod | grep -i -E "rknpu|rknn" || echo "(lsmod 无 rknpu —— 可能是内建 builtin)"
echo "--- dmesg 中 rknpu / RKNN 相关最后 30 行 ---"
dmesg 2>/dev/null | grep -i -E "rknpu|rknn|iommu|drm" | tail -30

echo
echo "===== 3. 内存 / CMA ====="
echo "--- free ---"
free -m
echo "--- MemAvailable / CmaTotal / CmaFree ---"
grep -E "MemTotal|MemFree|MemAvailable|CmaTotal|CmaFree|SUnreclaim" /proc/meminfo
echo "--- CMA 区域 ---"
grep -i cma /proc/misc 2>/dev/null
ls -l /dev/dma_heap/ 2>/dev/null || echo "(无 /dev/dma_heap)"
ls -l /dev/dri/ 2>/dev/null
echo "--- 内存碎片 ---"
cat /proc/buddyinfo
echo "--- dmesg 中 CMA / alloc 失败 ---"
dmesg 2>/dev/null | grep -i -E "cma|alloc.*fail|out of memory|oom" | tail -20

echo
echo "===== 4. 谁占着 NPU (renderD129) ====="
for p in /proc/[0-9]*; do
  pid=${p#/proc/}
  if ls -l $p/fd 2>/dev/null | grep -q renderD129; then
    echo "PID $pid : $(tr '\0' ' ' < $p/cmdline 2>/dev/null)"
  fi
done
echo "--- 有没有 rkllm / edgevision 残留进程 ---"
ps -eo pid,ppid,stat,rss,etimes,args | grep -E "rkllm|edgevision|llm_demo" | grep -v grep

echo
echo "===== 5. 运行时库版本 ====="
ls -l /usr/lib/librknnrt.so /usr/lib/librknn_api.so 2>/dev/null
ls -l /root/*.so 2>/dev/null
echo "--- rkllm 目录 ---"
ls -l /root/rkllm_deploy/ 2>/dev/null
echo "--- ldd 依赖检查 ---"
ldd /root/rkllm_deploy/rkllm_embodied_agent 2>/dev/null | grep -E "rknn|not found"

echo
echo "===== 6. 温度 / 降频 ====="
for z in /sys/class/thermal/thermal_zone*/temp; do
  printf "%s = %s\n" "$z" "$(cat $z 2>/dev/null)"
done
echo "--- 限频标志 ---"
dmesg 2>/dev/null | grep -i -E "throttl|thermal.*trip|cpufreq" | tail -10

echo
echo "===== 7. 页表 / 大页 / vmalloc ====="
echo "--- KSM / overcommit ---"
cat /proc/sys/vm/overcommit_memory
cat /proc/sys/vm/min_free_kbytes
echo "===== DONE ====="
