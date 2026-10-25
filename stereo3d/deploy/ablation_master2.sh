#!/bin/bash
# =============================================================================
# ablation_master2.sh —— task-5 消融主控 v2（板上执行）
# 与 v1 的差别：先等「其它会话的 stereo3d_demo 退出」再动手，避免双摄被抢。
# 硬性协议：测前 systemctl stop edgevision；测完【无论如何】start 并确认 active。
# =============================================================================
set -u
LOG=/root/stereo3d/ablation_master.log

echo "==== [master2] 等待其它会话释放摄像头 $(date '+%F %T') ===="
for i in $(seq 1 60); do   # 最多等 10 分钟
  if pgrep -f 'stereo3d_demo' > /dev/null; then
    echo "  ..busy ($i), 等待 10s"; sleep 10
  else
    echo "  摄像头空闲"; break
  fi
done
if pgrep -f 'stereo3d_demo' > /dev/null; then
  echo "!!! 等待超时仍被占用，放弃（不恢复 edgevision——它本来就被别人管着）"
  exit 9
fi

echo "==== [master2] 停 edgevision $(date '+%F %T') ===="
systemctl stop edgevision
sleep 1

echo "==== [master2] 跑消融 ===="
bash /root/stereo3d/ablation_run.sh 2>&1
RC=$?

echo "==== [master2] 消融退出码 $RC，恢复 edgevision $(date '+%F %T') ===="
systemctl start edgevision
sleep 3
if systemctl is-active edgevision > /dev/null; then
  echo "edgevision ACTIVE - 摄像头已归还生产服务"
else
  echo "!!! edgevision NOT ACTIVE，重试一次"
  systemctl restart edgevision
  sleep 3
  systemctl is-active edgevision && echo "edgevision ACTIVE (第2次成功)" \
      || echo "!!! edgevision 仍未激活 - 需要人工介入 !!!"
fi
