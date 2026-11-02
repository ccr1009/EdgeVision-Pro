#!/bin/bash
# 启动/停止/查看板上的 D20 网关, 供电脑侧客户端连接
set -u
cd /root/lab || exit 1

case "${1:-start}" in
  start)
    pkill -f w3_d20_edge_gateway 2>/dev/null
    sleep 0.5
    nohup ./w3_d20_edge_gateway > /tmp/d20.log 2>&1 &
    echo $! > /tmp/d20.pid
    sleep 1
    echo "已启动 pid=$(cat /tmp/d20.pid)"
    ss -ltnp 2>/dev/null | grep 9000 || echo "(端口 9000 尚未监听)"
    ;;
  stop)
    pkill -f w3_d20_edge_gateway 2>/dev/null && echo "已停止" || echo "没在跑"
    ;;
  log)
    echo "===== 网关日志 ====="
    cat /tmp/d20.log
    ;;
esac
