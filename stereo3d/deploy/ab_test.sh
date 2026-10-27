#!/bin/bash
# 单组 SGBM 消融测试: stop_edgevision -> kill残留 -> 跑demo -> 记日志 -> 起回edgevision? (由调用方控制)
# 用法: ab_test.sh NUM_DISP BLOCK RUNSEC
ND=$1; BK=$2; SEC=$3
LOG=/root/stereo3d/ab_test/nd${ND}_bk${BK}.log

pkill -9 -f stereo3d_demo 2>/dev/null
systemctl stop edgevision 2>/dev/null
sleep 2
# 若摄像头仍被占,报错退出
if fuser /dev/video81 >/dev/null 2>&1 || fuser /dev/video83 >/dev/null 2>&1; then
    echo "CAM_BUSY"
    exit 1
fi

cd /root/stereo3d/build
STEREO_NUM_DISP=$ND STEREO_BLOCK=$BK timeout --signal=INT --kill-after=5 ${SEC} \
    ./stereo3d_demo --no-calib --no-face > $LOG 2>&1

# 确保残留进程死透、摄像头释放
pkill -9 -f stereo3d_demo 2>/dev/null
sleep 1
echo "=== nd=$ND bk=$BK sec=$SEC ==="
grep '^\[' $LOG | tail -5
