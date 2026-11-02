#!/bin/bash
set -e
cd /root/rkllm_deploy
echo "=== 正在板端原生编译纯端侧具身智能体 rkllm_embodied_agent ==="
g++ -O2 rkllm_embodied_agent.cpp -L. -lrkllmrt -Wl,-rpath=. -o rkllm_embodied_agent
echo "=== 编译成功，启动端侧 Agent 闭环实测 ==="
./rkllm_embodied_agent "$@"
