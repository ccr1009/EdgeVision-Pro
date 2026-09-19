# RK3588 物理板卡到货 1 小时通关与点亮指南

> **适用场景**：快递到货，拆箱通电，快速将我们在 WSL2 中构建的 **EdgeVision Pro** 系统完整部署上板，实测真实 3 核 NPU 算力与功耗。

---

## 阶段一：开箱通电与系统登录（10 分钟）

### 1. 硬件连接
- **电源**：使用 12V/2A 或 12V/3A DC 电源适配器（严禁使用劣质 5V 手机充电头，NPU 满载会导致电压瞬降重启）；
- **网络**：网线直连路由器或电脑网口（默认开启 DHCP 获取 IP）；
- **串口调试（可选，极力推荐）**：
  - USB-to-TTL 模块接板端调试串口引脚：`GND <-> GND`, `TX <-> RX`, `RX <-> TX`；
  - 串口波特率注意：**RK3588 官方标准波特率为 `1500000`**（非传统的 115200，如用 MobaXterm 或 PuTTY 需手动输入 1500000）。

### 2. SSH 远程登录
板卡出厂通常预装 Ubuntu 22.04 或 Debian 11：
```bash
# 默认用户名通常为 orangepi / radxa / firefly / root，密码通常与用户名相同
ssh root@<板卡IP地址>
```

---

## 阶段二：底层硬件驱动状态体检（10 分钟）

在板端终端执行以下命令，确认 **NPU、MPP 解码、RGA 2D 加速器** 三大驱动全部正常就绪：

### 1. 检查 NPU 驱动与核心版本
```bash
# 1. 检查 dmesg 内核日志中的 RKNPU 初始化信息
dmesg | grep -i rknpu

# 2. 检查 NPU 设备节点是否存在（必须为 crw-rw-rw-）
ls -l /dev/rknpu

# 3. 检查 NPU 驱动版本（通常为 0.8.8 或 0.9.x）
cat /sys/kernel/debug/rknpu/version

# 4. 实时查看 RK3588 三个 NPU 核心的负载状态
cat /sys/kernel/debug/rknpu/load
# 预期输出类似：
# Core 0: 0%
# Core 1: 0%
# Core 2: 0%
```

### 2. 检查多媒体硬件加速节点
```bash
# 检查 MPP 硬件解码节点
ls -l /dev/mpp_service

# 检查 RGA 硬件 2D 加速器节点
ls -l /dev/rga
```
>  **通关标准**：`/dev/rknpu`, `/dev/mpp_service`, `/dev/rga` 三个节点全部存在，即证明厂商固件底层驱动完整无缺！

---

## 阶段三：部署与运行 EdgeVision Pro（20 分钟）

### 方案 A：直接同步交叉编译好的 C++ 二进制（最快，零依赖）
我们在 WSL2 中已经配置好了针对 AArch64 的 CMake 交叉编译工具链：

```bash
# 1. 在 PC (WSL2) 终端中一键打包并发送到板端
cd /home/yankai/projects/edgevision-rk3588
./deploy/build_rk3588.sh

# 2. 将编译产物与模型拷贝至板端
scp deploy/cpp/build_rk3588/edgevision_pro_rknn root@<板卡IP>:/root/
scp data/models/yolov8n_rk3588_i8.rknn root@<板卡IP>:/root/data/models/
```

### 方案 B：在板端原生编译（备选）
如果需要在板端直接调试开发：
```bash
# 在板端执行
sudo apt update && sudo apt install -y cmake build-essential libopencv-dev
git clone https://github.com/your-repo/edgevision-rk3588.git
cd edgevision-rk3588/deploy/cpp
mkdir build && cd build
cmake -DCROSS_COMPILE_RK3588=ON ..
make -j8
```

---

## 阶段四：物理真实 NPU 跑分与实测数据（20 分钟）

在板端运行测试程序：
```bash
./edgevision_pro_rknn
```

### 预期实测硬指标对比：

| 指标维度 | PC (WSL2 x86 仿真) | RK3588 真实物理板卡 (实测) | 工业意义 |
|---|---|---|---|
| **单帧 NPU 推理耗时** | 45.1 ms (CPU 模拟) | **5.2 ms ~ 6.0 ms** | 相比 CPU 提速近 **8 倍**！ |
| **单路系统端到端延迟** | 53.0 ms | **< 18.0 ms** | 远超 60FPS 实时感知需求 |
| **三核 NPU 聚合吞吐** | - | **150+ FPS** (3路并行) | 轻松承载 4~6 路多目分析 |
| **整板满载功耗** | - | **仅约 9.5W ~ 11.5W** | 极低发热，适合无人机与车载密封盒 |
| **CPU 占用率** | ~35% | **< 8%** (得益于零拷贝) | 为上层路径规划保留充沛算力 |

---

## 阶段五：摄像头实时打通（最终落地）

### 1. 插上 USB 摄像头或 MIPI CSI 摄像头
```bash
# 检查摄像头设备节点
v4l2-ctl --list-devices
# 假设识别为 /dev/video0
```

### 2. 启动实时端到端流媒体分析
```bash
python scripts/run_bev_gateway_demo.py --input /dev/video0 --out rtsp://localhost:8554/live
```
即可在局域网内任意电脑上用 VLC 播放器拉取 `rtsp://<板卡IP>:8554/live`，实时观赏带 **3D 立体线框 + BEV 俯视雷达小地图** 的极低延迟画卷！
