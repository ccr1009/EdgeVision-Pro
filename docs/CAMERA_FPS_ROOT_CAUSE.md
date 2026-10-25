# 摄像头帧率之谜：不是硬件上限，是代码没开口要（2026-10-24）

> **一句话**：`BENCHMARKS.md` 2.5.8 节写的「摄像头物理出帧率就是 20 fps」**是错的**。
> 真因是**代码从没调用 `cap.set(CAP_PROP_FPS, 30)`**，V4L2 默认协商到 20fps。
> 显式开口要 30fps 后，两台摄像头都能稳定跑 **33.33 ms/帧 = 30.0 fps**（400 帧无抖动）。
>
> **这条推翻旧结论，但也印证了 2.5.8 节末尾那条方法论** ——
> 「先测端到端上限」是对的，只是当时的「上限」测错了：把一个**没配好的默认值**当成了物理上限。

## 1. 现象

- 预览链路 `w4_edgevision_preview_real` HUD 稳定 ~20 FPS，单帧 ~50ms
- `bench_cam_fps`（`/root/lab/bench_cam_fps.cpp`）实测 50.02 ms/帧 → 20.0 fps
- `BENCHMARKS.md` 2.5.8 据此判定：「**摄像头物理出帧率就是 20 fps**，处理链路刚好跟得上，
  20 FPS 是上限不是下限」「优化任何一环都不会提高 FPS」

## 2. 排查（分层：先查硬件能力，再查代码请求）

### 第 1 层：硬件到底支不支持 30fps？—— `v4l2-ctl` 查能力

```bash
v4l2-ctl -d /dev/video81 --list-formats-ext
```

**1920x1080 / 1280x960 / 1280x720 全部列出 `Interval: Discrete 0.033s (30.000 fps)`**。
硬件明确支持 30fps。

### 第 2 层：为什么协商成了 20fps？—— 查当前生效值

```bash
v4l2-ctl -d /dev/video81 --get-parm
# 输出: Frames per second: 20.000 (20/1)
```

而源码里（`gateway.cpp:133`、`preview_real.cpp:259`、`bench_cam_fps.cpp:11`）
**只设了 `FOURCC` 和 `WIDTH/HEIGHT`，没有一处设 `CAP_PROP_FPS`**。

`bench_cam_fps.cpp:15` 那句 `printf("驱动报告的 FPS: %.1f")` 读的是**驱动当前协商值 20**，
不是能力上限 —— 这一行造成了「驱动说只有 20」的误读。真正的上限在 `--list-formats-ext`。

### 第 3 层：显式请求 30fps，看能不能跑起来

自写 `bench_cam_fps2.cpp`（多了 `cap.set(cv::CAP_PROP_FPS, req_fps)` 一行），对照实验：

| 请求 FPS | 回读 FPS | 实测 ms/帧 | 实测 FPS | 结论 |
| ---: | ---: | ---: | ---: | :--- |
| 不设置 | 20.00 | 50.03 | 20.0 | 复现旧结论 |
| **30** | 30.00 | **33.32** | **30.0** | ✅ 正常 |
| 20 | 20.00 | 50.00 | 20.0 | 正常 |
| 25 | 25.00 | 50.00 | 20.0 | ⚠️ 请求未被接受 |
| 15 | 15.00 | 33.33 | 30.0 | ⚠️ 请求未被接受 |
| 5 / 10 | 5.00 / 10.00 | — | 10 / 15 | ⚠️ 请求未被接受 |

> **注意「回读」会骗人**：请求 25fps 时 `cap.get(CAP_PROP_FPS)` 回读 `25.00`，
> 但实测仍是 20.0fps。**回读值是软件层记录的期望值，不等于硬件实际出帧节奏。**
> 这个坑差点让我得出「25fps 也能设」的错误结论。

## 3. 根因

**UVC 摄像头只在「支持帧率列表中的离散档位」上真正生效。**
这台 HDI Webcam 的 MJPG 支持档位是 `30 / 25 / 20 / 15 / 10 / 5 fps`，
但实际只有 **30** 能被正确兑现（25/15/10 都退回或跳档）。而默认协商值是 **20fps**。

代码**从未显式设置帧率** → 一直用默认 20fps → 全链路 50ms/帧是「等摄像头」而非「处理慢」。

## 4. 解法与验证

在 `cap.set(CAP_PROP_FOURCC)` 之后补一行：

```cpp
cap.set(cv::CAP_PROP_FPS, 30);   // ★ 必须显式设置，否则默认协商到 20fps
```

**长时验证（400 帧 ≈ 13s，两台摄像头都测）**：

| 摄像头 | 帧数 | 均值 | min | 中位 | p95 | max | 间隔分布 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| cam2 `/dev/video83` | 399 | **33.33 ms** | 32.56 | 33.32 | 33.50 | 34.09 | 399 帧全在 30~40ms 档 |
| cam1 `/dev/video81` | 399 | **33.33 ms** | 33.19 | 33.36 | 33.39 | 33.43 | 399 帧全在 30~40ms 档 |

**零抖动**（最大偏差 0.76ms），说明是稳的 30fps，不是均值凑出来的。

**用户眼前的验证**：`bench_cam_fps` 里 `printf("驱动报告的 FPS")` 这一行本身写得有歧义，
应改成打印「能力上限（从 list-formats-ext 或 `CAP_PROP_FPS` set 之后回读）+ 实测值」两个数。

## 5. 局限（引用时必须一起说）

1. **只测了「采集+解码」这一段**。接上 NPU（15.95ms）+ 解码（7ms）+ 编码（7.2ms）+ 画框，
   单帧处理仍需 ~34ms，30fps 输入（33.3ms 周期）下**刚好卡在临界**，
   实际预览 FPS 可能在 20~28 之间，需接进 preview 重新实测。
2. 两台摄像头同型号（HDI Webcam USB），结论**不能外推到其他 UVC 摄像头** ——
   不同固件的离散档位和默认值都不同，换摄像头必须重跑 `--list-formats-ext`。
3. 15/25/10/5 档位为何失效未深挖（可能固件 bug 或 USB 带宽协商），
   **只测了 20 和 30 两个可用档**。
4. 室内光照下测的，未在极暗/极亮环境复测 AE 对帧率的影响。不过已锁曝光测过，
   **锁曝光（exposure_auto=1, absolute=157）与自动曝光结果一致**，说明 AE 不是变量。

## 6. 复现命令

```bash
# 上传并编译
scp bench_cam_fps2.cpp bench_cam_ts.cpp root@<board-ip>:/root/lab/
ssh root@<board-ip>
cd /root/lab
g++ -O2 -std=c++17 bench_cam_fps2.cpp -o bench_cam_fps2 \
    -I/usr/include/opencv4 -lopencv_core -lopencv_videoio
g++ -O2 -std=c++17 bench_cam_ts.cpp -o bench_cam_ts \
    -I/usr/include/opencv4 -lopencv_core -lopencv_videoio

# 对照：不设 vs 设 30fps
./bench_cam_fps2 /dev/video83 1280 720 0    # 20.0 fps
./bench_cam_fps2 /dev/video83 1280 720 30   # 30.0 fps

# 长时稳定性 + 帧间隔分布
./bench_cam_ts /dev/video83 30 400

# 查硬件真实能力
v4l2-ctl -d /dev/video81 --list-formats-ext
```

## 7. 这次的方法论（比数字值钱）

> **「默认值」不是「上限」。** 2.5.8 节用 `bench_cam_fps` 测出 20fps 就下结论
> 「物理上限就是 20fps」，但那台摄像头的 `--list-formats-ext` 白纸黑字写着支持 30fps。
> 我少做了一步：**在测上限之前，先确认所有可调参数都已经调到了最大值。**
>
> 更隐蔽的是**回读值会骗人**：`cap.get(CAP_PROP_FPS)` 返回 25 时，
> 硬件实际跑的还是 20fps。软件层的「我以为」和硬件的「实际」之间，
> 差了一个实测打点。
>
> **教训**：测一个系统的「上限」时，先问自己一句——
> 「我现在测的，是真的顶到天花板了，还是撞在某个没调开的默认值上？」
