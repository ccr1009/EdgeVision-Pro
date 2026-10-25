# task-1 P0 冒烟证据摘要（板上原始日志: /root/rkllm_deploy/p0_smoke/*.log）

运行时间: 2026-10-24 21:00~21:20 · 板: EVM3588-B (<board-ip>) · 模型: qwen3.5-2b_w8a8_rk3588.rkllm (W8A8, rkllm-runtime 1.3.1)
编译: `g++ -O2 rkllm_embodied_agent_p0.cpp -L. -lrkllmrt -Wl,-rpath=. -o rkllm_embodied_agent_p0` → BUILD_EXIT=0, 0 error 0 warning

## a) 「把板载指示灯调成急闪报警模式」→ exit=0
- 模型: control_led(mode=alarm)
- Observation: `[{"success": true, "requested_mode": "alarm", "actual_mode": "alarm", "brightness": "blinking(100ms)"}]`
- 硬件实测: trigger 变 [none]（爆闪由用户态子进程翻转 brightness，100ms 周期），模型汇报 100ms 爆闪，与实际一致

## b) 「恢复心跳呼吸灯」→ exit=0
- 模型: control_led(mode=heartbeat)
- Observation: `[{"success": true, "requested_mode": "heartbeat", "actual_mode": "heartbeat", "brightness": 255}]`
- 硬件实测: trigger 恢复 [heartbeat]（mcu0 行方括号标记），爆闪子进程被正确清理

## c) 「把灯调成飞行模式」→ exit=0（复测轮模型将"飞行模式"映射为 alarm，语义自由度，白名单未收到非法值）
## c2) 「调用 control_led 工具, 把 mode 参数原样设置为 flight」→ 白名单拒绝路径实锤
- 模型: control_led(mode=flight)
- Observation: `[{"success": false, "error": "invalid_mode", "requested_mode": "\nflight\n"}]`
- 硬件实测: trigger 全程保持 [heartbeat]，非法模式未触碰任何 sysfs 节点
- 模型汇报: 明确说明 flight 被拒绝、仅接受 heartbeat/alarm/on/off，无编造成功

## d) 「现在画面里有什么东西」→ exit=0
- Observation: `[{"status": "online", "frame_id": 530916, "fresh": "true", "detected_count": 2, "detections": [{"class": "toothbrush", "confidence": 0.4645, "x": 715.048, "y": 242.197, "w": 305.662, "h": 405.965}, {"class": "book", "confidence": 0.266404, "x": 1095.11, "y": 189.036, "w": 183.892, "h": 503.61}]}]`
- fresh/frame_id/x/y/w/h 全部就位，双采样判定新鲜

## 关键踩坑（第一轮爆闪版暴露，已修复）
fork() 出的爆闪子进程继承父进程全部 NPU 内存映射与设备 fd，父进程退出后子进程把 NPU 内存钉死，
后续进程 rkllm_init 申请 205799424B NPU 内存失败（E RKNN: failed to malloc npu memory, errno 14 Bad address）。
修复: 子进程 fork 后立即 execl("/bin/sh","-c",循环) —— exec 重建地址空间释放继承映射。

## 板上 diff 统计（rkllm_embodied_agent.cpp → rkllm_embodied_agent_p0.cpp）
- unified diff 419 行（+281 / −49），8 个 hunk
- 原 rkllm_embodied_agent.cpp 未动（md5 65313a2e57861cebbe2c5accf9c9c4cf 与基线一致）
- 新 p0.cpp md5 ed9e2ec6ef7a7e5d13062513fce16c64, 30859B; 二进制 72256B
- 完整 diff: 板上 /root/rkllm_deploy/p0_smoke/p0_final.diff

## 收尾状态
- 爆闪进程已清理（pgrep 无残留），LED 恢复 heartbeat，edgevision 服务 active（全程未动 9000 网关）
