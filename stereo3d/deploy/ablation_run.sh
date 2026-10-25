#!/bin/bash
# =============================================================================
# ablation_run.sh —— task-5 第二阶段：SGBM 参数消融实测（板上执行）
# 流程：停 edgevision -> 验证 STEREO_NUM_DISP/STEREO_BLOCK env 生效 ->
#       6 组实测(nd64/48/32 × block5/7, 各200帧, 每组存snapshot) ->
#       结果写 /root/stereo3d/ablation_results.txt
# 注意：本脚本【不】负责 systemctl start edgevision —— 主控脚本测完再恢复。
# =============================================================================
set -u
BOARD_DIR=/root/stereo3d
OUT=$BOARD_DIR/ablation_results.txt
SNAP=$BOARD_DIR/ablation_snaps
BIN=$BOARD_DIR/build/ablation_sgbm
FRAMES=200

mkdir -p "$SNAP"
: > "$OUT"

echo "==== env check: board $(uname -m) $(nproc) cores ====" | tee -a "$OUT"

echo "---- [0] 验证 env 覆盖入口是否生效 ----" | tee -a "$OUT"
# 跑 30 帧 nd=32(block=5) 两组：一组用 --num-disp 显式传，一组用 env 覆盖(命令行给64)。
# 若 env 生效，两者的 sgbm_ms 应明显不同(env 组应更快)。
$BIN --num-disp 64 --block-size 5 --frames 30 2>&1 | grep RESULT >> "$OUT"
STEREO_NUM_DISP=32 $BIN --num-disp 64 --block-size 5 --frames 30 2>&1 | grep RESULT >> "$OUT"
# 若上行 env 组的 sgbm_ms 与下行显式组接近、且明显低于上面 64 组 => env 入口有效。

echo "---- [1] 正式消融：6 组 x ${FRAMES} 帧 ----" | tee -a "$OUT"
run_case() {
    local nd=$1 bs=$2
    echo "" | tee -a "$OUT"
    echo ">>>> CASE num_disp=$nd block=$bs <<<<" | tee -a "$OUT"
    $BIN --num-disp $nd --block-size $bs --frames $FRAMES \
         --snapshot "$SNAP/disp_nd${nd}_b${bs}.png" 2>&1 | tee -a "$OUT"
}
run_case 64 5
run_case 48 5
run_case 32 5
run_case 64 7
run_case 48 7
run_case 32 7

echo "---- DONE ----" | tee -a "$OUT"
ls -la "$SNAP" >> "$OUT" 2>&1
echo "results -> $OUT"
