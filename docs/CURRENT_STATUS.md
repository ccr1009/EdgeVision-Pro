# 当前状态快照(CURRENT_STATUS)

> 完整背景看项目根目录的 **`AGENTS.md`**(新对话会自动加载)。

## 一句话
RK3588 EVM3588-B 边缘AI校招项目 + Linux 应用层学习。

> 最后一次更新:**2026-10-17**(补了板端实测数据、免密 SSH、D20 状态)

## 现在能干什么
- SSH 板子:`ssh root@<board-ip>`(**已配免密**;别名 `ssh rk3588`;有线)
- 跑 NPU:`cd /root/edgevision && ./deploy/cpp/build_native/edgevision_pro_rknn`
- 板端测速(板上 `/root/lab/`):`./bench_npu <model> 200` / `./bench_npu <model> 1 --perf` / `./bench_multicore <model> 100`
- 自检:`bash /root/check_board.sh`
- TF 卡已挂 `/mnt/tf`(开机自动挂载)

## 进度

### 学习线(第 1~3 周)
- 第 1 周(文件IO/进程)**通关**;第 2 周(多线程)**通关**;第 3 周 D15~D19 **通关**
- **D20 第三周大综合:🟡 进行中**
  - 已写:`/root/lab/w3_d20_edge_gateway.cpp` —— TCP:9000 收客 + 3 个 worker 线程 + 独立推流线程 + 条件变量队列
  - 已验证:电脑侧 Python 客户端连入并**正确解包 BBox**(帧 1/2 各 2 目标,坐标与源码一致)
  - **还差:NPU 部分是 `usleep` 模拟,没接真实 `rknn_run`** ← 下一步核心

### 项目线(板端实测,2026-10-17)
新增探针 `deploy/cpp/bench/`,实测数据已回填 `docs/BENCHMARKS.md` 2.2 节:

| 指标 | 实测值 |
| :--- | :--- |
| 单核推理(推理引擎边界) | **37.19 ms / 26.9 FPS**(p99 40.28 ms) |
| 三核 mask `CORE_0_1_2`(单 context) | 35.97 ms / 27.8 FPS(仅 −3.3%) |
| 3 独立 context 绑 3 核真并行 | **59.3 FPS,加速比 2.21x**(三核各仅 49% 负载) |
| 逐层剖析 | NPU 115 算子 19.23 ms(51%) / **CPU 回退 10 算子 18.44 ms(49%)** |
| 最慢算子 | `dfl/Transpose` 9.18 ms、`dfl/Softmax` 4.47 ms、`Reshape` 2.66 ms(全在 CPU) |

**结论**:37ms 不是 NPU 算力不够,是 **YOLOv8 的 DFL 检测头没被 NPU 融合、回退到 CPU**。
**下一步优化优先级**:① 改 ONNX 导出砍掉 DFL 头(预计省 15ms+)→ ② 重转模型提 MAC 利用率 → ③ 最后才做 RGA/DMA-BUF 零拷贝。

## 下一步(按优先级)
1. **D20 收尾**:把 `w3_d20_edge_gateway` 里的 `usleep` 换成真实 `rknn_run`(接 `/root/edgevision` 的 RKNNEngine),让「真 NPU + 3 核 + TCP 推流」闭环。
2. **改模型导出**:砍 DFL 头 → 端到端从 37ms 打到 ~21ms,再重测回填 BENCHMARKS。
3. 校招主攻:【端侧AI部署/边缘计算】+【嵌入式Linux/机器人系统】
4. 桌面信息屏(**屏未买**,软件可先做);板载外设:看门狗 / ADC按键 / GPIO

## 详细资料
- `AGENTS.md`(背景与协作须知)
- `docs/LEARNING_PLAN_4WEEKS.md`(12 周路线)
- `docs/BENCHMARKS.md`(性能数据与复现命令)
- `docs/CAREER_DIRECTIONS.md`(校招方向)
