#!/bin/bash
# npu_doctor.sh —— rkllm / rknn NPU 内存加载失败自查工具
# 用法：bash /root/npu_doctor.sh
echo "════════════════════════════════════════════════════════"
echo " RK3588 NPU 体检  $(date '+%F %T')"
echo "════════════════════════════════════════════════════════"

echo
echo "① 基础状态"
echo "   内核: $(uname -r)   运行时长: $(uptime -p 2>/dev/null)   开机于: $(uptime -s 2>/dev/null)"
echo "   NPU 驱动: $(cat /sys/kernel/debug/rknpu/version 2>/dev/null | sed 's/.*: //')   频率: $(( $(cat /sys/class/devfreq/fdab0000.npu/cur_freq 2>/dev/null || echo 0) / 1000000 )) MHz"
echo "   NPU 负载: $(cat /sys/kernel/debug/rknpu/load 2>/dev/null | sed 's/NPU load: //')"
echo "   内存: MemAvailable=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)MB  CmaFree=$(awk '/CmaFree/{print $2}' /proc/meminfo)kB"

echo
echo "② 谁在占 NPU（★ 这是 4GB 地址空间被吃光的元凶）"
FOUND=0
for p in /proc/[0-9]*; do
  pid=${p#/proc/}
  cmd=$(tr '\0' ' ' < $p/cmdline 2>/dev/null)
  case "$cmd" in
    *edgevision_dashboard*|*w4_edgevision_gateway*|*rkllm_embodied*|*llm_demo*|*edgevision_pro*|*stereo3d*)
      rss=$(awk '/VmRSS/{print int($2/1024)}' $p/status 2>/dev/null)
      et=$(awk '{print int($1)}' $p/stat 2>/dev/null)
      printf "   PID %-7s RSS %-5s MB  %s\n" "$pid" "${rss:-?}" "$(echo $cmd | cut -c1-64)"
      FOUND=1;;
  esac
done
[ $FOUND -eq 0 ] && echo "   （无 NPU 相关进程）"

echo
echo "③ 残留的 rkllm 常驻实例（★ 最常见的失败原因）"
RL=$(pgrep -af "rkllm_embodied_agent" 2>/dev/null)
if [ -n "$RL" ]; then
  echo "   ⚠️  发现 $(echo "$RL" | wc -l) 个 rkllm 实例常驻，每个约占 2GB NPU 地址空间："
  echo "$RL" | sed 's/^/     /'
  echo "   → 需要时清理：pkill -f rkllm_embodied_agent"
else
  echo "   ✅ 没有残留的 rkllm 实例"
fi
echo "   LED 爆闪残留: $(pgrep -f 'edgevision_led_alarm' >/dev/null && echo '有（pkill -f brightness 清掉）' || echo 无)"

echo
echo "④ systemd 自启的 NPU 服务"
for s in edgevision.service rkllm.service; do
  st=$(systemctl is-active $s 2>/dev/null)
  [ -n "$st" ] && echo "   $s = $st"
done

echo
echo "⑤ 历史上 NPU 内存分配失败记录"
N=$(dmesg 2>/dev/null | grep -c "rknpu_gem_get_pages: dma map")
echo "   累计失败 $N 次"
if [ "$N" -gt 0 ]; then
  echo "   最近 5 次（'used' = 失败那一刻 NPU 已占用的地址空间，上限 4096 MiB）："
  dmesg 2>/dev/null | grep -A0 -E "used:|dma map" | grep -E "used:|dma map" | tail -10 | sed 's/^/     /'
  SZ=$(dmesg 2>/dev/null | grep 'rknpu_gem_get_pages: dma map' | tail -1 | awk '{print $(NF-1)}')
  echo "   最近一次失败请求大小: ${SZ} 字节 ≈ $(awk "BEGIN{printf \"%.1f\", ${SZ:-0}/1048576}") MiB"
fi

echo
echo "⑥ 结论"
if [ "$N" -eq 0 ]; then
  echo "   ✅ 从未出现 NPU 内存分配失败，可以放心跑"
else
  echo "   ⚠️  出现过 NPU 地址空间耗尽（RK3588 NPU IOMMU 上限 4096 MiB / 4 GiB）"
  echo "   这不是驱动版本问题，也不是 CMA 问题。按顺序处理："
  echo "     1) pkill -f rkllm_embodied_agent        # 清掉残留实例（每个 ~2GB）"
  echo "     2) systemctl stop edgevision.service    # 停掉网关（让出 NPU）"
  echo "     3) pkill -f edgevision_dashboard        # 停掉看板"
  echo "     4) 再跑目标程序"
  echo "   仍失败 → reboot（冷重启后地址空间彻底复位）"
fi
echo "════════════════════════════════════════════════════════"
