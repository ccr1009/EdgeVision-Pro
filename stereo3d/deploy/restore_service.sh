#!/bin/bash
# 恢复生产服务：等其它会话的 stereo3d_demo 退出后，确保 edgevision active。
for i in $(seq 1 45); do
  if pgrep -f 'stereo3d_demo' > /dev/null; then
    echo "  ..demo 占用中 ($i)，等 10s"
    sleep 10
  else
    break
  fi
done
pgrep -f stereo3d_demo > /dev/null && { echo "!!! 等待超时仍有 demo 进程"; exit 9; }
systemctl start edgevision
sleep 3
systemctl is-active edgevision && echo "RESTORED-ACTIVE" || { systemctl restart edgevision; sleep 3; systemctl is-active edgevision && echo "RESTORED-ACTIVE-2ND" || echo "RESTORE-FAILED"; }
