#!/bin/bash
# ============================================================================
# /root/bench_params.sh —— 参数扫描（无人闭环的第一层）
# ============================================================================
#  做什么：遍历「编译 flag × 上下文长度」，每组编译→起→跑固定问题→提取 tok/s→记录
#
#  用法：
#    bash bench_params.sh              # 完整扫描（4 组，约 8 分钟）
#    bash bench_params.sh quick        # 只测 2 组（-O2/-O3 @ ctx 8192）
#
#  自动化边界说明：
#    ★ 全程无人。判据是 decode tok/s 这个数字，不需要谁"觉得好"。
#    ★ 失败会跳过并记录，不会卡住整轮。
#    ★ 收尾一定把源码恢复成基线，不留脏状态。
#
#  已知限制：
#    - NPU 核数改不了（是模型自带属性，不在源码里）
#    - 如果某轮触发 IOVA 泄漏，脚本救不回来，需要人重启板子；
#      所以默认只跑 4 组，避免长时间占用
# ============================================================================
set -u

DIR=/root/rkllm_deploy
SRC="$DIR/rkllm_embodied_agent.cpp"
BASE=/tmp/agent_base.cpp
OUT=/tmp/param_scan.txt
MODE="${1:-full}"

cd "$DIR" || exit 1

# ---------- 前置：清场，确保 NPU 空闲 ----------
echo "[清场] 停掉所有占 NPU 的服务..."
systemctl stop edgevision-dashboard edgevision edgevision-patrol 2>/dev/null
bash /root/voice_start.sh --stop >/dev/null 2>&1
sleep 4

echo "[备份] $SRC -> $BASE"
cp "$SRC" "$BASE"

# 确保基线干净（改回 -O2 编译，排除上次残留）
printf "flag|ctx|prefill_ms|decode_toks|note\n" > "$OUT"

run_one() {
    local flag="$1" ctx="$2"
    cp "$BASE" "$SRC"
    sed -i "s/param\.max_context_len = [0-9]*/param.max_context_len = $ctx/" "$SRC"

    printf "[测试] %-4s ctx=%-6s 编译..." "$flag" "$ctx"
    if ! g++ $flag "$SRC" -L. -lrkllmrt -Wl,-rpath=. -o /tmp/agent_test 2>/tmp/cc.log; then
        echo " ✗ 编译失败"
        echo "$flag|$ctx|-|-|compile_fail" >> "$OUT"
        return
    fi
    echo " ok, 运行中..."

    # 固定问题："温度多少" —— 1 轮工具，输出稳定，便于横向比较
    ( echo "温度多少"; sleep 3; echo "exit" ) | timeout 120 /tmp/agent_test > /tmp/run.log 2>&1

    local pf dg
    pf=$(grep -oE "prefill [0-9]+ ms" /tmp/run.log | tail -1 | grep -oE "[0-9]+")
    dg=$(grep -oE "= [0-9]+ tok/s" /tmp/run.log | tail -1 | grep -oE "[0-9]+")

    if [ -z "${dg:-}" ]; then
        echo " ✗ 没抓到 perf（可能启动失败或超时）"
        echo "$flag|$ctx|${pf:-?}|-|no_perf" >> "$OUT"
    else
        echo " → prefill ${pf:-?}ms, decode ${dg} tok/s"
        echo "$flag|$ctx|${pf:-?}|$dg|ok" >> "$OUT"
    fi

    # 等 IOVA 释放（SIGTERM 优雅退出后大约 2~3 秒）
    sleep 8
}

if [ "$MODE" = "quick" ]; then
    run_one "-O2" 8192
    run_one "-O3" 8192
else
    run_one "-O2" 8192
    run_one "-O2" 4096
    run_one "-O3" 8192
    run_one "-O3" 4096
fi

# ---------- 收尾：恢复基线 ----------
cp "$BASE" "$SRC"
g++ -O2 "$SRC" -L. -lrkllmrt -Wl,-rpath=. -o "$DIR/rkllm_embodied_agent_p0" 2>/dev/null \
    && echo "[恢复] 已还原基线（-O2 / ctx=8192）并重新编译" \
    || echo "[警告] 基线恢复编译失败，请手动检查"

echo
echo "════════════════ 扫描结果 ════════════════"
column -t -s'|' "$OUT" 2>/dev/null || cat "$OUT"
echo "═════════════════════════════════════════"
echo "结论判据：看 decode_toks 列。prefill 只影响首轮，decode 影响每次生成。"
