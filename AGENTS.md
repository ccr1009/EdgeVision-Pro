# AGENTS.md — 仓库内协作须知(每次新对话请先读这个)

> **权威版本在工作区根目录**:`D:\a研究生\项目\rk3588项目\AGENTS.md`(含完整 12 周路线、
> 全部踩坑记录与协作偏好)。本文件是仓库内的精简副本,只保留开发时要立刻知道的事。
> 两者冲突时**以根目录那份为准**。

---

## 0. 一句话
研究生,手上 **EVM3588-B(RK3588)**,做**端侧 AI 部署 / 边缘计算**方向的校招项目,
同时用这块板子系统学 **Linux 应用层开发**。

## 1. 硬件与环境(实测)
| 项 | 值 |
|---|---|
| 开发板 | EVM3588-B(英码 EMA),SOM3588-B,8GB + 64GB eMMC |
| 系统 | **Debian 11**,kernel **5.10.110** |
| NPU | **驱动 v0.9.2 / 运行时 API 2.3.0**,3 核,主频 1.0 GHz |
| 网络 | 有线,板子 IP 见本地私密记录(校园内网);WiFi 已关(掉线) |
| SSH | `ssh <user>@<board-ip>`(**已配免密**,别名 `ssh rk3588`) |
| 已知正常 | 内存 7.5Gi、eMMC 57.6G、RGA/MPP 正常、librknnrt.so 已装 |

### 关键坑
- **`/dev/rknpu` 不存在是正常的**:新版驱动走 DRM `/dev/dri/renderD129`。
- **一律在板子上原生编译**:交叉编译有 glibc 风险(Ubuntu22 的 2.35 vs 板子 2.31)。
- **板子系统在 eMMC,不需要刷机**;`.img` 是 RKFW 整包,不能直接拷 TF 卡。
- **37ms/帧 ≠ NPU 慢**:实测 **49% 时间在 CPU**(YOLOv8 DFL 头 Transpose/Softmax/Reshape 回退),
  NPU 主体只占 19.23 ms,三核并行时每核仅 49% 负载 → 优化先改模型导出,**别先做零拷贝**。
- **Windows 侧跑远程命令**:复杂内联 `ssh 'a | b; c'` 的引号会被吃掉 → 写 `.sh` 脚本 scp 上去再 `bash`。

## 2. 进度(2026-10-25)

> **现状一句话**:视觉推理链路 + 端侧大模型 Agent + 巡检守护**都已真实闭环跑通**,
> 数字全部来自板端实测(口径见 `docs/BENCHMARKS.md`,局限一并写在里面)。

- **学习线**:第 1~4 周(Linux 系统编程 / 多线程并发 / 网络与网关 / 服务化)全通关。
- **推理链路(真实,无仿真)**:UVC 摄像头 MJPEG → **MPP 硬解**(p50 1.03ms,vs OpenCV 软解 7.76ms = **7.6×**)
  → CPU 侧 CSC + letterbox → **3 个独立 context 绑 3 核 NPU 真并行** → DFL-Free 后处理 → TCP:9000 定长帧推流。
  - 纯推理 **15.95 ms**(DFL 图手术前 37.19 ms,**−57%**);单帧全链路 20.58 ms / 48.6 FPS;
    三核聚合 **59.3 FPS / 2.21×**(三核各约 49% 负载,**不是线性 3×**,别对外吹线性)。
  - INT8 量化掉点用**真 mAP** 量化:COCO128 上 FP32 **0.6087** → INT8 **0.6002**(−1.4%)。
  - 编译优化:CMake 一个多月来 **`CMAKE_BUILD_TYPE` 是空的**(一个 `-O` 都没有)→ 改默认 Release 后
    CSC **1.549×**、全链路 **−27.4%**(含两组「不该变」的对照组,见 BENCHMARKS 2.8)。
- **端侧具身 Agent(纯板端离线)**:RKLLM + Qwen3.5-2B(W8A8) → ReAct 多步 → 5 个硬件工具
  (`get_system_health` / `get_led_status` / `control_led` / `get_vision_telemetry` / `get_patrol_events`)。
  两幕演示端到端 106.5~112.2s、4~5 个不同工具、Observation 全合法 JSON。
- **巡检守护(快慢双系统)**:独立 C++ 守护 5s 周期采温度/NPU/视觉帧,快层**不加载 LLM**(避免抢 NPU);
  状态翻转才写事件日志 + `llm_pending.jsonl` 给慢层。真实闪灯验收用**占空比**判据(20.0% → 49.7%)。
- **可靠性**:12 用例回归**全绿**(含故障注入停服务验视觉离线 + 自动恢复)。

⚠️ **明确未实现(不要对外宣传)**:RGA 硬件 CSC / DMA-BUF 零拷贝 / RTSP-WebRTC / H.264-H.265 硬解
/ ByteTrack 进 C++ 链路 / 整机功耗。完整清单见根目录 `README.md` 的「明确未实现」表。

## 3. 构建 / 运行
```bash
# 项目主程序(板端,先编译)
ssh root@<board-ip>          # 板子 IP 见本地私密记录(校园内网)
cd /root/edgevision/deploy/cpp && mkdir -p build_native && cd build_native
cmake .. -DCROSS_COMPILE_RK3588=ON && make
cd /root/edgevision && ./deploy/cpp/build_native/edgevision_pro_rknn

# 性能探针(本仓库 deploy/cpp/bench/)
B=/root/edgevision/deploy/cpp/3rdparty/rknn_api
g++ -O2 -std=c++17 bench_npu.cpp -I$B/include -L$B/lib -lrknnrt -o bench_npu
export LD_LIBRARY_PATH=$B/lib
./bench_npu <model> 200        # 单核/三核/延迟分位数
./bench_npu <model> 1 --perf   # 逐层耗时剖析
```

## 4. 文档
- 根目录 `AGENTS.md` —— **权威版**,含完整路线与坑
- `docs/CURRENT_STATUS.md` —— 状态快照
- `docs/BENCHMARKS.md` —— 板端实测数据与复现命令
- `docs/LEARNING_PLAN_4WEEKS.md` —— 12 周路线

## 5. 协作偏好
说中文;简洁直接可操作;用户是学生,**学习场景让他自己敲代码、AI 当私教陪同排错**;
校招项目要**真实、可量化、能讲清**,警惕吹嘘的数字;买硬件要给明确型号+价格+接口。
