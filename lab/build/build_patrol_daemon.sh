#!/bin/bash
# build_patrol_daemon.sh — 板上原生编译巡检守护快层
# 为什么原生编译: 交叉编译有 glibc 风险(PC 端 2.35 vs 板子 Debian11 2.31), 一律板上 g++ 编译。
# 只依赖 pthread/std, 不链接 RKLLM —— 快层绝不加载大模型。
set -e
# 本脚本在 lab/build/，源码在 lab/agent/（2026-11-02 目录重整后）
# 反推 lab/ 根再按相对路径编译，无论从哪个目录调用都成立
cd "$(dirname "$0")/.."
SRC="agent/edgevision_patrol_daemon.cpp"

echo "[build] g++ -O2 -std=c++17 -Wall $SRC -lpthread -o edgevision_patrol_daemon"
g++ -O2 -std=c++17 -Wall "$SRC" -lpthread -o edgevision_patrol_daemon

echo "[build] OK -> $(pwd)/edgevision_patrol_daemon"
echo "[note] 板上编译时源码位于 /root/lab/agent/，本脚本也可直接在那里运行"
