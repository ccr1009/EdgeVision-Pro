#!/bin/bash
# 摄像头 20fps 诊断 - 第 2 步：能力侦察（不干扰 service，先只查询）
DEV=/dev/video81

echo "===== A. 支持的格式与帧率 ====="
v4l2-ctl -d $DEV --list-formats-ext 2>&1

echo
echo "===== B. 当前生效的格式 ====="
v4l2-ctl -d $DEV --get-fmt-video 2>&1
v4l2-ctl -d $DEV --get-parm 2>&1

echo
echo "===== C. 全部可调控制项 ====="
v4l2-ctl -d $DEV --list-ctrls 2>&1

echo
echo "===== D. 当前控制项取值 ====="
v4l2-ctl -d $DEV --list-ctrls-menus 2>&1 | head -60
