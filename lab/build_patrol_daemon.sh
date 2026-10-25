#!/bin/bash
# build_patrol_daemon.sh — 板上原生编译巡检守护快层
# 为什么原生编译: 交叉编译有 glibc 风险(PC 端 2.35 vs 板子 Debian11 2.31), 一律板上 g++ 编译。
# 只依赖 pthread/std, 不链接 RKLLM —— 快层绝不加载大模型。
set -e
cd "$(dirname "$0")"

echo "[build] g++ -O2 -std=c++17 -Wall edgevision_patrol_daemon.cpp -lpthread -o edgevision_patrol_daemon"
g++ -O2 -std=c++17 -Wall edgevision_patrol_daemon.cpp -lpthread -o edgevision_patrol_daemon

echo "[build] OK -> $(pwd)/edgevision_patrol_daemon"
