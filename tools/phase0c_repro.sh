#!/bin/bash
# ============================================================================
#  phase0c_repro.sh v2 —— 复现验证：不停 gateway/dashboard，能否稳定起 Agent
# ============================================================================
#  v1 的 bug：RES=$(spawn_agent ...) 中启动的后台进程继承了命令替换的管道，
#             只要 sleep 保活进程活着，$() 永远等不到 EOF → 脚本卡死。
#  v2 修正：① 不用命令替换，改用全局变量传结果
#           ② 所有后台进程显式重定向，绝不继承父 shell 的 stdout
#           ③ 全程 stdbuf -oL 行缓冲，日志可实时查看
#
#  待验证：
#    P1  gateway + dashboard 在跑时起 1 个 rkllm 能成功（run_agent.sh 的断言可能过保守）
#    P2  可重复
#    P3  再起第 2 个会失败（证明上限真实存在，只是位置在"第 2 个"）
# ============================================================================

RKDIR=/root/rkllm_deploy
AGENT=$RKDIR/rkllm_embodied_agent
DASH=/root/edgevision/deploy/cpp/build_release/edgevision_dashboard

ARES=""      # spawn_agent 结果：OK:<秒> / FAIL:<秒> / TIMEOUT
APID=""      # 本次 agent 的 pid

say() { echo; echo "▸ $*"; }

ensure_bg() {
  if ! systemctl is-active --quiet edgevision.service; then
    systemctl start edgevision.service; sleep 12
  fi
  if ! pgrep -f edgevision_dashboard >/dev/null; then
    ( cd /root/edgevision && exec nohup $DASH --port 8090 > /tmp/p0c_dash.log 2>&1 ) &
    sleep 18
  fi
}

kill_agents() {
  pkill -9 -f rkllm_embodied_agent 2>/dev/null
  pkill -f "sleep 900" 2>/dev/null
  rm -f /tmp/p0c_f* 2>/dev/null
  sleep 4
}

# 起一个 agent 并等到就绪/失败；结果写进全局 ARES，pid 写进 APID
spawn_agent() {
  local n=$1 limit=${2:-170}
  local fifo=/tmp/p0c_f$n
  local log=/tmp/p0c_a$n.log
  rm -f $fifo $log
  mkfifo $fifo
  # 保活写端：显式断开父 shell 的全部标准流
  ( exec sleep 900 > $fifo ) </dev/null >/dev/null 2>&1 &
  # agent：stdin 接 fifo，stdout/stderr 进日志；$! 即 exec 后的 agent pid
  ( cd $RKDIR && exec env LD_LIBRARY_PATH=$RKDIR $AGENT < $fifo > $log 2>&1 ) &
  APID=$!
  local t0
  t0=$(date +%s)
  ARES="TIMEOUT"
  local i
  for i in $(seq 1 $limit); do
    if grep -q "已就绪" $log 2>/dev/null; then
      ARES="OK:$(( $(date +%s) - t0 ))s"; break
    fi
    if grep -q "rkllm_init failed" $log 2>/dev/null; then
      ARES="FAIL:$(( $(date +%s) - t0 ))s"; break
    fi
    sleep 1
  done
}

echo "════════════════════════════════════════════════════════════"
echo " 复现验证 v2  $(date '+%H:%M:%S')"
echo "════════════════════════════════════════════════════════════"

kill_agents
ensure_bg
say "背景进程（全程不停）"
echo "    gateway   = $(systemctl is-active edgevision.service)  pid=$(pgrep -f w4_edgevision_gateway | head -1)"
echo "    dashboard = $(pgrep -f edgevision_dashboard >/dev/null && echo RUNNING || echo NONE)  pid=$(pgrep -f edgevision_dashboard | head -1)"
echo "    9000 推流 = $(timeout 2 cat < /dev/tcp/127.0.0.1/9000 2>/dev/null | wc -c) B/2s"

# ---------------------------------------------------------------- P1 / P2
R=0
while [ $R -lt 2 ]; do
  R=$((R+1))
  say "第 $R 轮：只清残留 rkllm，不停任何服务，直接起"
  kill_agents
  DM=$(dmesg 2>/dev/null | wc -l)
  spawn_agent $R 170
  echo "    init = $ARES   pid=${APID:-none}"
  if [ "${ARES:0:2}" = "OK" ]; then
    RSS=$(awk '/^VmRSS/{printf "%.0f", $2/1024}' /proc/$APID/status 2>/dev/null)
    echo "    RSS = ${RSS} MB"
    echo "查一下开发板当前温度" > /tmp/p0c_f$R
    RT=NO
    for i in $(seq 1 80); do
      grep -q "get_system_health" /tmp/p0c_a$R.log 2>/dev/null && { RT=YES; break; }
      sleep 1
    done
    echo "    真实推理（触发 get_system_health）= $RT"
    echo "    回答摘录:"
    grep -E "°C|℃|温度" /tmp/p0c_a$R.log 2>/dev/null | tail -3 | cut -c1-100 | sed 's/^/      /'
    STREAM=$(timeout 2 cat < /dev/tcp/127.0.0.1/9000 2>/dev/null | wc -c)
    echo "    共存期间 9000 推流 = $STREAM B/2s  (mod248=$((STREAM % 248)))"
    ERRS=$(dmesg 2>/dev/null | tail -n +$((DM+1)) | grep -cE "failed to malloc npu|failed to allocate handle")
    echo "    本轮 NPU 分配失败 = $ERRS 次"
  else
    echo "    ---- 失败摘要 ----"
    grep -E "failed to allocate|failed to malloc|rkllm_init failed" /tmp/p0c_a$R.log 2>/dev/null | sed 's/^/      /'
    dmesg 2>/dev/null | tail -n +$((DM+1)) | grep -E "used:|dma map" | tail -6 | sed 's/^/      /'
  fi
done

# ---------------------------------------------------------------- P3 边界
say "第 3 轮：保留第 2 轮的实例，再起第 2 个（验证上限）"
FIRST=$(pgrep -f "${AGENT}\$" | head -1)
echo "    已有实例 pid=${FIRST:-无}"
if [ -z "$FIRST" ]; then
  kill_agents
  spawn_agent 8 170
  echo "    补起一个：$ARES"
fi
DM=$(dmesg 2>/dev/null | wc -l)
spawn_agent 3 170
echo "    第 2 个 init = $ARES"
if [ "${ARES:0:4}" = "FAIL" ] || [ "$ARES" = "TIMEOUT" ]; then
  echo "    ✅ 第 2 个失败 → 上限真实存在，位置在「第 2 个」而非「第 1 个」"
  grep -E "failed to allocate|failed to malloc" /tmp/p0c_a3.log 2>/dev/null | sed 's/^/      /'
  dmesg 2>/dev/null | tail -n +$((DM+1)) | grep -E "used:|dma map" | tail -8 | sed 's/^/      /'
elif [ "${ARES:0:2}" = "OK" ]; then
  echo "    ⚠️ 第 2 个也成功 → 容量比文档记录的更大"
fi

# ---------------------------------------------------------------- 收尾
say "收尾"
kill_agents
echo "    gateway=$(systemctl is-active edgevision.service)  dashboard=$(pgrep -f edgevision_dashboard >/dev/null && echo YES || echo NO)"
echo "    9000 推流 = $(timeout 2 cat < /dev/tcp/127.0.0.1/9000 2>/dev/null | wc -c) B/2s"
echo "===== PHASE0C DONE ====="
