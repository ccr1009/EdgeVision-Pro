@echo off
chcp 65001 >nul
title EdgeVision-Pro: Live Webcam 3D & BEV Perception Demo
color 0B
echo =======================================================================
echo          EdgeVision-Pro: 电脑摄像头实时 3D 感知与鸟瞰雷达演示
echo                    作者: 陈创荣 (ccr1009)
echo =======================================================================
echo.
echo [*] 正在连接电脑摄像头 (Camera 0) 并加载 ONNX 3D BEV 推理网关...
echo [*] 提示: 弹窗出来后，按 [Q] 退出，按 [S] 截屏保存，按 [空格] 暂停。
echo.
py -3.12 "%~dp0webcam_demo.py"
if %errorlevel% neq 0 (
    python "%~dp0webcam_demo.py"
)
