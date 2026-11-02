#!/bin/bash
# ============================================================================
#  test_utf8_guard.sh —— 验证 UTF-8 防护与空输出兜底
# ============================================================================
#  用例：
#    1  GBK 编码的「led改为常亮」→ 修复前：abort；修复后：提示 + 继续运行
#    2  UTF-8 的「led改为常亮」  → 正常执行
#    3  过短输入「温度」          → 修复前：屏幕无变化像卡死；修复后：给兜底提示
# ============================================================================

cd /root/rkllm_deploy || exit 1
rm -f /tmp/tt /tmp/tt.log
pkill -9 -f "rkllm_embodied_agent_p0$" 2>/dev/null
sleep 3

mkfifo /tmp/tt
( exec sleep 600 > /tmp/tt ) </dev/null >/dev/null 2>&1 &
( exec ./rkllm_embodied_agent < /tmp/tt > /tmp/tt.log 2>&1 ) </dev/null >/dev/null 2>&1 &
APID=$!

echo "=========== 等待加载 ==========="
for i in $(seq 1 90); do
  grep -q "已就绪" /tmp/tt.log 2>/dev/null && break
  sleep 1
done
if ! grep -q "已就绪" /tmp/tt.log 2>/dev/null; then
  echo "!! 加载失败"; tail -20 /tmp/tt.log; kill -9 $APID 2>/dev/null; exit 1
fi
echo "  就绪，pid=$APID"

run_case() {
  local title=$1 ; local send=$2 ; local wait=$3
  echo
  echo "=========== $title ==========="
  local mark
  mark=$(wc -l < /tmp/tt.log)
  printf "$send" > /tmp/tt
  local i
  for i in $(seq 1 $wait); do
    sleep 1
    kill -0 $APID 2>/dev/null || break
  done
  if kill -0 $APID 2>/dev/null; then
    echo "  进程存活 = YES"
  else
    echo "  进程存活 = NO  <<< 崩溃了"
  fi
  echo "  --- 本轮新增输出 ---"
  tail -n +$((mark+1)) /tmp/tt.log | sed 's/^/    /' | tail -25
}

# 用例 1：GBK 字节（改=B8C4 为=CEAA 常=B3A3 亮=C1C1）
run_case "用例1 GBK 编码输入（修复前会 abort）" 'led\xB8\xC4\xCE\xAA\xB3\xA3\xC1\xC1\n' 75

# 用例 2：正常 UTF-8
run_case "用例2 UTF-8 编码输入" 'led改为常亮\n' 75

# 用例3：过短输入
run_case "用例3 过短输入「温度」" '温度\n' 90

echo
echo "=========== 收尾 ==========="
kill -9 $APID 2>/dev/null
pkill -f "sleep 600" 2>/dev/null
rm -f /tmp/tt
echo "存活实例 = $(pgrep -c -f 'rkllm_embodied_agent_p0$' 2>/dev/null || echo 0)"
echo "===== DONE ====="
