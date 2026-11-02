#!/bin/bash
# 追溯：用户 21:07 失败前，系统上还跑过什么？（查 shell 历史 + 日志）
echo "══════════ A. 用户 21:07 前后的操作历史 ══════════"
echo "--- /root/.bash_history 最近 40 条 ---"
tail -40 /root/.bash_history 2>/dev/null | cat -n | sed 's/^/  /'

echo
echo "══════════ B. 失败时刻（开机 122356 秒 ≈ 34 小时）对应的时间 ══════════"
echo "  开机时间: $(uptime -s 2>/dev/null)"
echo "  失败时刻: 开机后 122356 秒 = $(date -d "@$(( $(date -d "$(uptime -s)" +%s) + 122356 ))" '+%F %T' 2>/dev/null)"
echo "  当前时间: $(date '+%F %T')"
echo "  当前运行时长: $(cut -d' ' -f1 /proc/uptime) 秒"

echo
echo "══════════ C. 现在系统上有哪些进程是「开机就一直在」的长命进程 ══════════"
ps -eo pid,etimes,rss,stat,args --sort=-etimes | head -20 | awk '{printf "  %-8s %-10s %-8s %-6s %s\n", $1, $2"s", int($3/1024)"MB", $4, substr($0, index($0,$5), 60)}'

echo
echo "══════════ D. NPU 相关进程（含已退出但可能有残留的）══════════"
echo "--- 当前所有持 renderD129 的进程 ---"
for p in /proc/[0-9]*; do
  pid=${p#/proc/}
  if ls -l $p/fd 2>/dev/null | grep -q renderD129; then
    echo "  PID $pid: $(tr '\0' ' ' < $p/cmdline 2>/dev/null | cut -c1-70)"
    ls -l $p/fd 2>/dev/null | grep renderD129 | awk '{print "     fd="$9" -> "$11}'
  fi
done
echo "  （空 = 当前无进程持有 NPU 设备节点）"

echo
echo "══════════ E. 当前 dmesg 全部 dma map 失败记录（含时间戳）══════════"
dmesg | grep "dma map" | sed 's/^/  /'
echo
echo "══════════ F. dmesg 里每次失败前是否带 used: 行 ══════════"
dmesg | grep -E "used:|dma map" | sed 's/^/  /'

echo
echo "══════════ G. 系统日志里 21:00~21:10 的活动 ══════════"
journalctl --since "2026-10-26 21:00" --until "2026-10-26 21:10" --no-pager 2>/dev/null | tail -30 | sed 's/^/  /' || echo "  (无 journal)"
echo "===== DONE ====="
