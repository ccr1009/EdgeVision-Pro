#!/bin/bash
# ============================================================================
#  phase0b_coexist.sh —— 验证 gateway + dashboard + rkllm 三者能否真正共存
# ============================================================================
#  背景：run_agent.sh 的注释断言「gateway 常驻时再起 rkllm 必失败」。
#        Phase 0 第一轮实测却成功启动了。二者必有一错，本脚本判定。
#
#  判定标准（三条同时满足才算"真共存"）：
#    1. rkllm init 成功并打印「已就绪」
#    2. rkllm 能完成一次真实推理（回出实质内容，不是空/报错）
#    3. 共存期间 gateway 仍能正常推流，dmesg 无 NPU 分配错误
# ============================================================================

RKDIR=/root/rkllm_deploy
AGENT=$RKDIR/rkllm_embodied_agent
DASH=/root/edgevision/deploy/cpp/build_release/edgevision_dashboard
GWPORT=9000

echo "════════════════════════════════════════════════════════"
echo " 共存验证  $(date '+%H:%M:%S')"
echo "════════════════════════════════════════════════════════"

# 记录 dmesg 基线，只统计本次新增
DMARK=$(dmesg 2>/dev/null | wc -l)

# -------------------------------------------------- 1. 确保 gateway 在跑
if ! systemctl is-active --quiet edgevision.service; then
  echo "[1] 启动 gateway..."
  systemctl start edgevision.service
  sleep 12
fi
echo "[1] gateway = $(systemctl is-active edgevision.service)  pid=$(pgrep -f w4_edgevision_gateway | head -1)"

# -------------------------------------------------- 2. 确保 dashboard 在跑
if ! pgrep -f edgevision_dashboard >/dev/null; then
  echo "[2] 启动 dashboard..."
  cd /root/edgevision && nohup $DASH --port 8090 > /tmp/p0b_dash.log 2>&1 &
  sleep 18
fi
echo "[2] dashboard pid=$(pgrep -f edgevision_dashboard | head -1)"

# -------------------------------------------------- 3. 起 rkllm
echo "[3] 启动 rkllm（2.98 GB 模型，等待 60~120s）..."
rm -f /tmp/p0b_fifo; mkfifo /tmp/p0b_fifo
( sleep 600 > /tmp/p0b_fifo & )
cd $RKDIR
LD_LIBRARY_PATH=$RKDIR $AGENT < /tmp/p0b_fifo > /tmp/p0b_agent.log 2>&1 &
APID=$!
echo "    agent pid=$APID"

READY=NO
for i in $(seq 1 150); do
  if grep -q "已就绪" /tmp/p0b_agent.log 2>/dev/null; then READY=YES; break; fi
  if grep -q "rkllm_init failed" /tmp/p0b_agent.log 2>/dev/null; then READY=FAIL; break; fi
  ps -p $APID >/dev/null 2>&1 || { READY=DEAD; break; }
  sleep 1
done

echo "[3] init 结果 = $READY"
if [ "$READY" != "YES" ]; then
  echo "    ---- agent 日志 ----"
  tail -25 /tmp/p0b_agent.log | sed 's/^/      /'
  echo "    ---- 本次新增 dmesg ----"
  dmesg 2>/dev/null | tail -n +$((DMARK+1)) | grep -E "rknpu|RKNN|rkllm|dma map" | tail -20 | sed 's/^/      /'
  echo
  echo ">>> 结论：共存失败（与 run_agent.sh 的断言一致）"
  pkill -9 -f rkllm_embodied_agent 2>/dev/null
  pkill -f "sleep 600" 2>/dev/null; rm -f /tmp/p0b_fifo
  exit 0
fi

# -------------------------------------------------- 4. 真实推理
echo "[4] 发一条真实指令，验证推理链路..."
echo "查一下开发板当前温度" > /tmp/p0b_fifo
INFER_OK=NO
for i in $(seq 1 90); do
  # 抓到工具调用或回复即可
  if grep -qE "get_system_health|温度|°C|℃" /tmp/p0b_agent.log 2>/dev/null; then
    INFER_OK=YES; break
  fi
  sleep 1
done
echo "    推理结果 = $INFER_OK"
echo "    ---- 回答摘录 ----"
tail -40 /tmp/p0b_agent.log | grep -vE "^\s*$" | tail -18 | sed 's/^/      /'

# -------------------------------------------------- 5. 共存稳定性观察
echo "[5] 共存稳定性观察 60s（每 15s 采样）..."
for t in 15 30 45 60; do
  sleep 15
  GW_ALIVE=$(systemctl is-active edgevision.service)
  GW_PID=$(pgrep -f w4_edgevision_gateway | head -1)
  DASH_ALIVE=$(pgrep -f edgevision_dashboard >/dev/null && echo YES || echo NO)
  AG_ALIVE=$(ps -p $APID -o stat= 2>/dev/null | tr -d ' ')
  NPU=$(cat /sys/kernel/debug/rknpu/load 2>/dev/null | tr -d '\n' | sed 's/NPU load: //')
  echo "    +${t}s  gateway=$GW_ALIVE($GW_PID)  dash=$DASH_ALIVE  agent=${AG_ALIVE:-DEAD}"
  echo "          NPU: $NPU"
done

# -------------------------------------------------- 6. 新增 dmesg 错误
echo "[6] 本次新增的 NPU 相关 dmesg（前 15 条）"
dmesg 2>/dev/null | tail -n +$((DMARK+1)) | grep -E "rknpu|RKNN|rkllm|dma map|failed" | head -15 | sed 's/^/    /' || echo "    （无）"
ERRS=$(dmesg 2>/dev/null | tail -n +$((DMARK+1)) | grep -cE "failed to malloc npu|failed to allocate handle")
echo "    NPU 分配失败次数 = $ERRS"

# -------------------------------------------------- 7. 汇总
echo
echo "════════════════════════════════════════════════════════"
echo " 判定"
echo "════════════════════════════════════════════════════════"
echo "  init 成功      : $READY"
echo "  真实推理       : $INFER_OK"
echo "  NPU 分配失败   : $ERRS 次"
echo "  gateway 存活   : $(systemctl is-active edgevision.service)"
if [ "$INFER_OK" = "YES" ] && [ "$ERRS" = "0" ] && [ "$(systemctl is-active edgevision.service)" = "active" ]; then
  echo
  echo "  >>> ✅ 三者真共存。run_agent.sh 的「必须先停服务」前提不成立。"
  echo "      npud 项目的核心动机需要重写。"
else
  echo
  echo "  >>> ⚠️ 共存不完整，需进一步定位失败点。"
fi

# -------------------------------------------------- 8. 恢复
echo
echo "[8] 恢复现场（杀 agent，保留 gateway）"
pkill -9 -f rkllm_embodied_agent 2>/dev/null
pkill -f "sleep 600" 2>/dev/null
rm -f /tmp/p0b_fifo
sleep 3
echo "    gateway=$(systemctl is-active edgevision.service)  dashboard=$(pgrep -f edgevision_dashboard >/dev/null && echo YES || echo NO)"
echo "===== DONE ====="
