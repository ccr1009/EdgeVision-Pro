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

## 2. 进度(2026-10-17)
- 学习线:第 1 周(文件IO/进程)、第 2 周(多线程)、第 3 周 D15~D19 **全通关**。
- **D20 第三周大综合:🟡 进行中** —— 板上 `/root/lab/w3_d20_edge_gateway.cpp` 已有
  「TCP:9000 + 3 worker + 推流线程 + 条件变量队列」骨架,电脑侧客户端**已联调成功**;
  **但 NPU 部分是 `usleep` 模拟,未接真实 `rknn_run`**。
- 板端实测已回填 **`docs/BENCHMARKS.md` 2.2 节**:单核 37.19ms/26.9FPS、三核真并行 59.3FPS(2.21x)、
  逐层剖析 NPU 19.23ms vs CPU 回退 18.44ms。

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
