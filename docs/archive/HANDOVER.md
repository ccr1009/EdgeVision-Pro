# EdgeVision-RK3588: 板子到货后部署与交接清单

> **面向研一同学的保姆级实机操作手册**  
> 当您的 RK3588 开发板（如友善 NanoPC-T6、Firefly ROC-RK3588S、香橙派 OrangePi 5 Plus 等）到货后，请按照本清单依次执行，即可无缝将 PC 仿真代码迁移到 NPU 真机上。

---

## 阶段 1: 系统烧录与基础配置 (30 分钟)

### 1.1 推荐固件选择
- **操作系统**: Ubuntu 22.04 LTS Desktop/Server (官方提供适配 Linux 5.10 / 6.1 内核镜像)。
- **烧录工具**: Windows 下使用 `RKDevTool` 或 `Etcher` 烧录至 NVMe SSD 或 eMMC（尽量避免 TF 卡，TF 卡 I/O 会成为多路视频瓶颈）。

### 1.2 检查 NPU 驱动状态
上电开机进入系统终端，执行：
```bash
# 查看内核日志中是否有 rknpu 驱动初始化成功
dmesg | grep -i rknpu

# 查看当前 NPU 驱动版本号
cat /sys/kernel/debug/rknpu/version
# 正常应输出类似: RKNPU driver: v0.9.x 或更高
```

### 1.3 安装必备系统工具
```bash
sudo apt-get update
sudo apt-get install -y git cmake g++ build-essential python3-pip python3-dev ffmpeg libopencv-dev
```

---

## 阶段 2: 板端 Python 运行时环境配置 (15 分钟)

### 2.1 获取代码仓库
在板端克隆本项目：
```bash
git clone <your-repo> edgevision-rk3588
cd edgevision-rk3588
```

### 2.2 安装板端轻量运行时 (rknn-toolkit-lite2)
> [!IMPORTANT]
> **切记**：PC 端使用的是模型转换工具包 `rknn-toolkit2`；而 RK3588 板端只需要安装超轻量的板端运行时 `rknn-toolkit-lite2`！

```bash
# 创建虚拟环境
python3 -m venv .venv
source .venv/bin/activate

# 安装基础依赖
pip install -r requirements.txt

# 下载并安装 RK3588 aarch64 专用 rknn_toolkit_lite2 wheel
# 官方路径: https://github.com/rockchip-linux/rknn-toolkit2/tree/master/rknn_toolkit_lite2/packages
wget https://raw.githubusercontent.com/rockchip-linux/rknn-toolkit2/master/rknn_toolkit_lite2/packages/rknn_toolkit_lite2-1.6.0-cp310-cp310-linux_aarch64.whl
pip install rknn_toolkit_lite2-1.6.0-cp310-cp310-linux_aarch64.whl

# 验证导入
python -c "from rknnlite.api import RKNNLite; print('RKNNLite Ready!')"
```

---

## 阶段 3: 切换 RKNNBackend 跑通端到端 Demo (10 分钟)

### 3.1 拷贝 PC 端量化好的模型文件
将 PC 端生成的 `data/models/yolov8n_rk3588_i8.rknn` 传输至板端对应目录。

### 3.2 一键启动板端 NPU 推理
```bash
python pipeline/demo_pipeline.py \
    --video data/videos/test_surveillance.mp4 \
    --backend rknn \
    --model data/models/yolov8n_rk3588_i8.rknn \
    --output outputs/board_demo.mp4
```
**观察点**：
- 观察终端打印的 `Inference` 耗时：由 x86 CPU 的 ~50ms 骤降至 NPU 的 **~6ms**！
- 帧率由 ~19 FPS 跃升至满帧 **30+ FPS**！

---

## 阶段 4: C++ 原生程序交叉编译与上板运行 (20 分钟)

### 4.1 在 PC 端一键交叉编译
在 WSL2 (PC 端) 执行：
```bash
cd deploy/cpp
# 如果没有 cross compiler: sudo apt-get install -y g++-aarch64-linux-gnu
bash build_rk3588.sh
```
编译产物位于 `deploy/cpp/build_aarch64/edgevision_rknn`。

### 4.2 推送至板端并链接运行
```bash
# PC 端推送
scp deploy/cpp/build_aarch64/edgevision_rknn rk3588@<板端IP>:~/
scp deploy/cpp/3rdparty/rknn_api/lib/librknnrt.so rk3588@<板端IP>:/usr/lib/

# 板端执行
ssh rk3588@<板端IP>
chmod +x edgevision_rknn
./edgevision_rknn yolov8n_rk3588_i8.rknn
```

---

## 阶段 5: 真实多路视频压测与 Demo 录制 (简历加分项)

1. **接入多路 RTSP 摄像头**：
   - 接入 2~4 路局域网网络摄像头，在 `PipelineEngine` 中实例化多个子进程/线程。
2. **监测板端 NPU 与 CPU 负载**：
   ```bash
   # 查看实时 NPU 核心利用率与主频
   watch -n 1 "cat /sys/kernel/debug/rknpu/load"
   
   # 查看 CPU 温度与各核心负荷
   htop
   cat /sys/class/thermal/thermal_zone0/temp
   ```
3. **记录性能数据并回填 `BENCHMARKS.md`**：
   - 填入实测单路延迟、4 路并发总 FPS 与 CPU/NPU 占用。
4. **录制实机运行视频 (Demo)**：
   - 录制屏幕上实时播放的多路带框视频、Track ID 轨迹与终端 `htop`/`rknpu/load`，作为研究生简历项目的有力证明！
