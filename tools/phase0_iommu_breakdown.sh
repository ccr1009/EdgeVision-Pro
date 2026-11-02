#!/bin/bash
# ============================================================================
#  phase0_iommu_breakdown.sh —— NPU IOMMU 4 GiB 占用分解
# ============================================================================
#  目的：回答"RKNN 到底吃了多少 IOVA"，为 npud 推理服务化提供决策依据。
#
#  方法：不撑爆、不猜。直接读 /sys/kernel/debug/dma_buf/bufinfo，
#        按「附着设备」统计 rknpu(fdab0000.npu) 持有的 buffer 数量与总字节。
#        dma_buf 的 size 即 IOVA 映射量的来源，比"读失败 dump 里的 used 值"更轻。
#
#  四个配置隔离变量：
#    A 空板          → 基线
#    B 只有 gateway  → B-A = gateway 占用
#    C 只有 dashboard→ C-A = dashboard 占用
#    D 两者同时      → 验证是否叠加
#    E D + rkllm     → 验证争用是否导致失败
# ============================================================================

MODEL=/root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn
DASH=/root/edgevision/deploy/cpp/build_release/edgevision_dashboard
AGENT=/root/rkllm_deploy/rkllm_embodied_agent
RKDIR=/root/rkllm_deploy

# ---------------------------------------------------------------- 快照函数
npu_snapshot() {
  awk '
    /^[0-9]/            { size=$1+0; inatt=0; next }
    /Attached Devices:/ { inatt=1; next }
    /^Total [0-9]+ devices attached/ { inatt=0; next }
    inatt==1            { d=$1; s[d]+=size; c[d]++ }
    END {
      n=0
      for (d in s) { names[++n]=d }
      # 只报与 NPU 相关的（fdab0000.npu），其余汇总
      for (i=1;i<=n;i++) {
        d=names[i]
        if (d ~ /npu/) printf "    %-20s bufs=%-4d total=%9.2f MiB\n", d, c[d], s[d]/1048576
      }
      other=0; ocnt=0
      for (i=1;i<=n;i++) { d=names[i]; if (d !~ /npu/) { other+=s[d]; ocnt+=c[d] } }
      if (ocnt>0) printf "    %-20s bufs=%-4d total=%9.2f MiB\n", "(其他设备合计)", ocnt, other/1048576
    }
  ' /sys/kernel/debug/dma_buf/bufinfo 2>/dev/null
}

# 单个进程的资源画像
proc_profile() {
  local name=$1
  local pid
  pid=$(pgrep -f "$name" | head -1)
  if [ -z "$pid" ]; then
    printf "    %-12s 未运行\n" "$name"
    return
  fi
  local rss fds thr
  rss=$(awk '/^VmRSS/{printf "%.1f", $2/1024}' /proc/$pid/status 2>/dev/null)
  fds=$(ls /proc/$pid/fd 2>/dev/null | wc -l)
  thr=$(awk '/^Threads/{print $2}' /proc/$pid/status 2>/dev/null)
  printf "    %-12s pid=%-6s RSS=%-8s MB  fd=%-5s 线程=%s\n" "$name" "$pid" "$rss" "$fds" "$thr"
}

# 当前 NPU 上的进程清单
who_npu() {
  local p
  p=$(pgrep -af 'rkllm_embodied_agent|edgevision_dashboard|w4_edgevision_gateway' 2>/dev/null | grep -v pgrep)
  if [ -z "$p" ]; then echo "    （无）"; else echo "$p" | sed 's/^/    /'; fi
}

section() { echo; echo "════════════════════════════════════════════════════════════"; echo " $*"; echo "════════════════════════════════════════════════════════════"; }

# ---------------------------------------------------------------- 清场
cleanall() {
  systemctl stop edgevision.service 2>/dev/null
  pkill -f edgevision_dashboard 2>/dev/null
  pkill -9 -f rkllm_embodied_agent 2>/dev/null
  pkill -f "sleep 900" 2>/dev/null
  rm -f /tmp/fp0* 2>/dev/null
  sleep 5
}

restore() {
  pkill -f edgevision_dashboard 2>/dev/null
  pkill -9 -f rkllm_embodied_agent 2>/dev/null
  pkill -f "sleep 900" 2>/dev/null
  rm -f /tmp/fp0* 2>/dev/null
  systemctl start edgevision.service 2>/dev/null
  sleep 3
  echo "  gateway=$(systemctl is-active edgevision.service)  dashboard=$(pgrep -f edgevision_dashboard >/dev/null && echo YES || echo NO)"
}
trap 'echo; echo "!! 中断，恢复现场"; restore; exit 130' INT TERM

echo "════════════════════════════════════════════════════════════"
echo " Phase 0 · NPU IOMMU 占用分解"
echo " $(date '+%Y-%m-%d %H:%M:%S')"
echo " 模型: $MODEL ($(stat -c%s $MODEL 2>/dev/null) B)"
echo "════════════════════════════════════════════════════════════"

# ============================================================ A 空板
section "A · 空板基线"
cleanall
echo "  进程:"; who_npu
echo "  资源:"; proc_profile edgevision_dashboard
echo "  dma_buf 中 NPU 附着:"
npu_snapshot
A_TOTAL=$(npu_snapshot | awk '/npu/{for(i=1;i<=NF;i++) if($i ~ /total=/){gsub(/total=|MiB/,"",$i); s+=$i}} END{printf "%.2f", s}')
echo "  >>> A 基线 NPU IOVA = ${A_TOTAL:-0} MiB"

# ============================================================ B 只有 gateway
section "B · 只开 gateway (:9000)"
systemctl start edgevision.service 2>/dev/null
echo "  等待 gateway 就绪（摄像头 + 3 worker 加载模型）..."
for i in $(seq 1 30); do
  pgrep -f w4_edgevision_gateway >/dev/null && break
  sleep 1
done
sleep 12
echo "  服务状态: $(systemctl is-active edgevision.service)"
echo "  进程:"; who_npu
echo "  资源:"; proc_profile w4_edgevision_gateway
echo "  dma_buf 中 NPU 附着:"
npu_snapshot
B_TOTAL=$(npu_snapshot | awk '/npu/{for(i=1;i<=NF;i++) if($i ~ /total=/){gsub(/total=|MiB/,"",$i); s+=$i}} END{printf "%.2f", s}')
echo "  >>> B 总占用 = ${B_TOTAL:-0} MiB  →  gateway 净占 $(awk -v b="${B_TOTAL:-0}" -v a="${A_TOTAL:-0}" 'BEGIN{printf "%.2f", b-a}') MiB"

# ============================================================ C 只有 dashboard
section "C · 只开 dashboard (:8090)"
cleanall
cd /root/edgevision 2>/dev/null
nohup $DASH --port 8090 > /tmp/fp0_dash.log 2>&1 &
sleep 18
echo "  进程:"; who_npu
echo "  资源:"; proc_profile edgevision_dashboard
echo "  dashboard 日志尾: $(tail -2 /tmp/fp0_dash.log 2>/dev/null | tr '\n' ' ')"
echo "  dma_buf 中 NPU 附着:"
npu_snapshot
C_TOTAL=$(npu_snapshot | awk '/npu/{for(i=1;i<=NF;i++) if($i ~ /total=/){gsub(/total=|MiB/,"",$i); s+=$i}} END{printf "%.2f", s}')
echo "  >>> C 总占用 = ${C_TOTAL:-0} MiB  →  dashboard 净占 $(awk -v c="${C_TOTAL:-0}" -v a="${A_TOTAL:-0}" 'BEGIN{printf "%.2f", c-a}') MiB"

# ============================================================ D 两者同时
section "D · gateway + dashboard 同时"
systemctl start edgevision.service 2>/dev/null
sleep 14
echo "  进程:"; who_npu
echo "  资源:"; proc_profile w4_edgevision_gateway; proc_profile edgevision_dashboard
echo "  dma_buf 中 NPU 附着:"
npu_snapshot
D_TOTAL=$(npu_snapshot | awk '/npu/{for(i=1;i<=NF;i++) if($i ~ /total=/){gsub(/total=|MiB/,"",$i); s+=$i}} END{printf "%.2f", s}')
echo "  >>> D 总占用 = ${D_TOTAL:-0} MiB  （若 ≈ B+C-A 则线性叠加）"

# ============================================================ E 再起 rkllm
section "E · 在 D 之上起 rkllm（验证争用）"
if [ ! -x "$AGENT" ]; then
  echo "  [!] 找不到 $AGENT，跳过 E"
else
  mkfifo /tmp/fp0fifo 2>/dev/null
  ( sleep 900 > /tmp/fp0fifo & )
  cd $RKDIR && LD_LIBRARY_PATH=$RKDIR $AGENT < /tmp/fp0fifo > /tmp/fp0_agent.log 2>&1 &
  echo "  等待 rkllm 加载（模型 2.98 GB，约 60~90s）..."
  for i in $(seq 1 120); do
    grep -q "已就绪" /tmp/fp0_agent.log 2>/dev/null && break
    grep -q "rkllm_init failed" /tmp/fp0_agent.log 2>/dev/null && break
    sleep 1
  done
  if grep -q "已就绪" /tmp/fp0_agent.log 2>/dev/null; then
    echo "  >>> ✅ rkllm 成功启动 —— 与 gateway+dashboard 共存可行"
  else
    echo "  >>> ❌ rkllm 启动失败 —— 争用成立"
    grep -E "failed to allocate|failed to malloc|rkllm_init failed" /tmp/fp0_agent.log | sed 's/^/      /'
  fi
  echo "  dma_buf 中 NPU 附着:"
  npu_snapshot
  proc_profile rkllm_embodied_agent
fi

# ============================================================ 汇总
section "汇总"
cat <<EOF
  配置                         NPU IOVA 总计
  A 空板                        ${A_TOTAL:-?} MiB
  B 只开 gateway                ${B_TOTAL:-?} MiB
  C 只开 dashboard              ${C_TOTAL:-?} MiB
  D gateway + dashboard         ${D_TOTAL:-?} MiB
  上限                          4096 MiB

  判定规则：
    gateway 净占 = B - A
    dashboard 净占 = C - A
    若 D ≈ B + C - A        → 线性叠加，收敛 context 必然省出空间
    若 D 明显 < B + C - A   → 存在共享/复用，npud 的收益主要在「不重复加载」
    若 gateway 净占 < 200 MiB → 瓶颈不在 RKNN，收益口径改为「消除服务互斥」
EOF

echo
restore
rm -f /tmp/fp0fifo
echo "===== PHASE0 DONE ====="
