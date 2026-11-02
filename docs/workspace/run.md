1、
ssh rk3588
root@ema-alip:~# cd /root/edgevision
root@ema-alip:~/edgevision# ./deploy/cpp/build_release/edgevision_dashboard --cam /dev/video83 --port 8090
跑在8090端口，车辆实时检测
2、全部重新编译
cd /root/edgevision/deploy/cpp/build_release
make -j4
3、agent
cd /root/rkllm_deploy
./rkllm_embodied_agent_p0
4、麦克风
bash /root/voice_start.sh                  # 默认启动（停 gateway，独占 NPU，响应快）
bash /root/voice_start.sh --keep-gateway   # 保留 gateway（NPU 争用，响应慢到 39 秒）
bash /root/voice_start.sh --status         # 看状态
bash /root/voice_start.sh --stop           # 停止，并恢复 gateway
tail -f /tmp/vgw.log
