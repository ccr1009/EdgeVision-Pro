#!/bin/bash
cd /root/lab

echo "[1] 停止后台 edgevision 守护服务，释放摄像头 /dev/video81..."
systemctl stop edgevision 2>/dev/null || true

echo "[2] 清理可能占用 8080 端口或摄像头的旧预览进程..."
# 注意：用 ^ 锚定完整路径，否则 "preview" 会匹配到本脚本自己（start_preview_real.sh）把它杀掉
pkill -9 -f '^/root/lab/w4_edgevision_preview_real' 2>/dev/null || true
fuser -k 8080/tcp 2>/dev/null || true
sleep 1

echo "[3] 检查可执行程序..."
if [ ! -f /root/lab/w4_edgevision_preview_real ]; then
    echo "[*] 正在编译 w4_edgevision_preview_real..."
    g++ -O2 -DENABLE_RKNN /root/lab/w4_edgevision_preview_real.cpp \
        /root/edgevision/deploy/cpp/src/postprocess_dfl_free.cpp \
        -o /root/lab/w4_edgevision_preview_real \
        -I/usr/include/opencv4 \
        -I/root/edgevision/deploy/cpp/include \
        -I/root/edgevision/deploy/cpp/3rdparty/rknn_api/include \
        -lopencv_core -lopencv_videoio -lopencv_imgproc -lopencv_imgcodecs \
        -lrknnrt -lpthread
fi

echo "[4] 启动真实 AI 画框大屏预览服务 (8080 端口)..."
# setsid 让它脱离当前会话，SSH 断开也不会被 SIGHUP 带走
setsid nohup /root/lab/w4_edgevision_preview_real /dev/video81 0.25 \
    > /root/lab/preview_real.log 2>&1 < /dev/null &
sleep 3

ps aux | grep w4_edgevision_preview_real | grep -v grep || true
echo "[+] 启动完成！"
