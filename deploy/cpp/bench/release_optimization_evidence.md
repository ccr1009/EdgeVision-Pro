# Release 编译优化证据（task-7）

> 日期：2026-10-25 ｜ 板卡：EVM3588-B（RK3588，8 核 4×A76 + 4×A55，Debian 11，g++ 10.2.1，cmake 3.18.4）
> 采集：Lead 复核板上原始日志（`/root/optim_ab/`、`/root/lead_build_check/`）后整理。
> 交叉验证：`edgevision-rk3588/docs/BENCHMARKS.md` 2.8 节、`docs/TROUBLESHOOTING.md` 第 24 条。

---

## 1. 问题：项目跑了 20 多天，构建系统里从来没有过一个 `-O`

```
$ grep CMAKE_BUILD_TYPE /root/edgevision/deploy/cpp/build_native/CMakeCache.txt
CMAKE_BUILD_TYPE:STRING=                       ← 空

$ grep CXX_FLAGS /root/edgevision/deploy/cpp/build_native/CMakeFiles/edgevision_pro_rknn.dir/flags.make
CXX_FLAGS = -std=gnu++17                       ← 没有任何 -O 标志，等效 -O0
```

**后果**：此前记录的所有 CPU 侧耗时（MJPG 软解 7.76 ms、letterbox、DFL 解码 3.09 ms、CSC 51~65 ms）
全部是**无优化构建**下的悲观值。

**为什么一直没被发现**：CMake 在 build type 为空时**不报错、不警告**，直接按空 flags 编译。
`make` 全绿、程序功能完全正常 —— 功能测试永远发现不了这件事。

---

## 2. CMakeLists.txt 改动清单（`edgevision-rk3588/deploy/cpp/CMakeLists.txt`，206 → 222 行）

| 行号 | 改动 |
| :--- | :--- |
| 17-21 | `if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)` → 默认 `Release` |
| 24-29 | 兜底 `CMAKE_CXX_FLAGS_RELEASE = "-O3 -DNDEBUG"`（`CACHE STRING ... FORCE`，命令行仍可覆盖） |
| 33 | 新增 `option(BUILD_BENCH "Build bench tools with the same flags" OFF)` |
| 45-70 | `check_cxx_compiler_flag` 自动探测 `-mcpu=cortex-a76.cortex-a55` → 失败降级 `-mtune=cortex-a76` → 再降级通用；`-DEVISION_ARCH_FLAGS=none` 可关闭 |
| 102-108 | **[集成点]** `if(EXISTS src/mpp_jpeg_encoder.cpp)` → 加入 `COMMON_SOURCES` + `add_definitions(-DHAVE_MPP_JPEG_ENCODER)`；文件不存在只打一行 message 不报错 |
| 180-192 | `BUILD_BENCH=ON` 时用同一套标志编 `bench_dflfree` |
| 197-222 | 构建配置摘要，并在 flags 里没有 `-O` 时打 `CMake Warning`（防回归） |

关键片段：

```cmake
# 17-21：默认 Release
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# 24-29：兜底（注意变量名是全大写 + 值首字母大写）
string(TOUPPER "${CMAKE_BUILD_TYPE}" _BT_UPPER)
if(NOT CMAKE_CXX_FLAGS_${_BT_UPPER})
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)
endif()

# 102-108：MPP 硬编集成点（文件不存在时不报错）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/mpp_jpeg_encoder.cpp")
  list(APPEND COMMON_SOURCES src/mpp_jpeg_encoder.cpp)
  add_definitions(-DHAVE_MPP_JPEG_ENCODER)
else()
  message(STATUS "mpp_jpeg_encoder.cpp not found, skip MPP JPEG encoder")
endif()
```

### 2.1 实测生效标志（`build_release/CMakeFiles/edgevision_pro_rknn.dir/flags.make`）

```
CMAKE_BUILD_TYPE        = Release
CMAKE_CXX_FLAGS_RELEASE = -O3 -DNDEBUG
CXX_FLAGS               = -O3 -DNDEBUG -mcpu=cortex-a76.cortex-a55 -std=gnu++17
CXX_DEFINES             = -DENABLE_RKNN -DHAVE_MPP_JPEG_ENCODER -DHAVE_OPENCV
```

Lead 独立复核（全新目录、不传任何 `-DCMAKE_BUILD_TYPE`，脚本 `_test_results/lead_verify_integration.sh`）：

```
/root/lead_build_check/
  CMakeCache.txt: CMAKE_BUILD_TYPE:STRING=Release
  flags.make:     CXX_FLAGS = -O3 -DNDEBUG -mcpu=cortex-a76.cortex-a55 -std=gnu++17
  make -j8 → 0 error 0 warning
  edgevision_pro_rknn  109376 B（含 encoder 集成点）
```

---

## 3. A/B 实测（`taskset -c 6` 固定大核，6 轮交替 O0/O3，每轮 200 次迭代）

板端原始日志：`/root/optim_ab/final/logs/ab_pinned.log`、`/root/optim_ab/pro_ab/`

| 口径 | O0（无优化） | O3（`-O3 -DNDEBUG -mcpu=a76.a55`） | 变化 |
| :--- | ---: | ---: | :--- |
| ① 纯推理 `rknn_run` | 14.79 ~ 14.88 ms | 14.90 ~ 14.95 ms | **−0.3% ~ 0%（对照组，符合预期）** |
| ② IO 周期（set + run + get） | 15.47 ~ 15.56 ms | 15.58 ~ 15.64 ms | 无变化 |
| ③ **DFL 解码 + NMS** | **3.44 ~ 3.51 ms** | **0.62 ~ 0.63 ms** | **5.5× 加速** |
| ④ **全链路（②+③）** | **18.91 ~ 19.06 ms** | **16.21 ~ 16.26 ms** | **省 2.78 ms（−14.6%），52.6 → 61.7 FPS（+17.3%）** |

- 6 轮全部落在上面区间内，**组内波动 < 1%**，重复性很好。
- 6 轮检出框数均为 0（bench 输入是灰图，符合预期）。

---

## 4. 对照组的设计为什么是这次实验最重要的部分

第 ① 项（纯推理）在 O0 / O3 下差 **< 1%**，它同时证明了两件事：

1. **NPU 时间与主机编译优化无关** —— 符合物理预期，说明两边测的确实是同一个模型、同一条代码路径。
2. **测量本身没有漂移** —— 如果这一层也出现了 >5% 的变化，说明有负载 / 频率 / 热漂移干扰，
   那么第 ③④ 项的差异就不可信。

**方法论**：做 A/B 一定要先设一个"理论上不该变"的对照组，它稳了，其他项的变化才能归因。

---

## 5. 坑：不绑核时，O0 的 `mean` 会骗你（把 5.5× 压成 2×）

| 组 | DFL mean | DFL p50 | DFL p95 |
| :--- | ---: | ---: | ---: |
| **pinned** O0 | 3.44 ~ 3.51 ms | 3.44 ~ 3.48 | 3.48 ~ 3.71 |
| **pinned** O3 | 0.62 ~ 0.63 ms | 0.62 ~ 0.63 | 0.63 ~ 0.64 |
| **unpinned** O0 | **6.38 ~ 8.15 ms** | 3.44 ~ 3.47 | **15.78 ~ 17.20** |
| **unpinned** O3 | 2.71 ~ 2.93 ms | 2.71 ~ 2.89 | 2.73 ~ 3.16 |

unpinned 组 O0 的 **p50 只有 3.45 ms（与 pinned 完全一致），但 mean 被拉到 6.4~8.2 ms，p95 冲到 17 ms**。

**根因**：O0 代码体量大、单次执行时间长，被 CPU 调度器跨核（含大小核）迁移打断的次数显著更多；
O3 版执行短平快，受影响小。所以 **mean 反映的是调度噪声，不是代码差异**。

**结论：性能测量必须 `taskset` 绑核。**

---

## 6. 二进制体积

| 构建 | `edgevision_pro_rknn` | `edgevision_rknn` |
| :--- | ---: | ---: |
| 无优化（`build_native`，2026-10-24） | 206,256 B | 172,592 B |
| Release `-O3`（不含 MPP encoder） | 75,688 B | 65,792 B |
| Release `-O3`（含 MPP encoder 集成点） | 109,376 B | 99,672 B |

→ 纯优化带来的代码体积缩减 **−63%**（206,256 → 75,688 B）。

---

## 7. 生产二进制全链路 A/B（Lead 复核后补测，2026-10-25）

第 4 节测的是 `bench_dflfree`（不含 CSC / letterbox / 图像 IO）。为回答"**真实生产程序到底快多少**"，
用**同一张真实 JPEG、同一个模型**直接跑两个**生产二进制**各 5 次（`taskset -c 6` 绑核）：

```bash
IMG=/root/edgevision/data/calib_v2/real_000.jpg            # 48220 B, 640x640
export EV_MODEL=/root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn
export LD_LIBRARY_PATH=/root/edgevision/deploy/cpp/3rdparty/rknn_api/lib:/root/edgevision/third_party/mpp/lib
for i in 1 2 3 4 5; do
  taskset -c 6 /root/edgevision/deploy/cpp/build_native/edgevision_pro_rknn  $IMG
  taskset -c 6 /root/edgevision/deploy/cpp/build_release/edgevision_pro_rknn $IMG
done
```

| 层 | O0 `build_native`（5 次均值） | O3 `build_release`（5 次均值） | 变化 | 判读 |
| :--- | ---: | ---: | ---: | :--- |
| MPP 硬解 decode | 1.250 ms | 1.322 ms | +5.8% | **对照组①**：硬件解码，与主机编译优化无关 ✔ |
| **NV12→RGB888 CSC** | **52.016 ms** | **33.583 ms** | **1.549×** ⭐ | 自有定点循环（`mpp_jpeg_decoder.cpp:149-173`），优化直接命中 |
| letterbox（`cv::resize`） | 1.184 ms | 1.222 ms | +3.2% | **对照组②**：走 OpenCV 预编译 `.so`，我们动不了 ✔ |
| NPU + DFL 解码（`run_inference`） | 22.480 ms | 19.708 ms | −2.77 ms | 与第 4 节第 ③ 项的 −2.78 ms 吻合 |
| **全链路合计** | **76.928 ms** | **55.836 ms** | **−27.4%（1.378×）** | 13.0 → 17.9 FPS |

原始 5 次数据：

- O0：decode `1.108/1.279/1.279/1.302/1.280`，CSC `53.143/51.747/51.569/51.564/52.058`，
  letterbox `1.096/1.124/1.303/1.184/1.212`，run_inference `22.77/22.29/22.27/22.42/22.65`，
  合计 `78.11/76.44/76.42/76.47/77.20`
- O3：decode `1.291/1.293/1.313/1.276/1.438`，CSC `33.584/33.347/33.547/33.936/33.502`，
  letterbox `1.351/1.145/1.328/1.128/1.156`，run_inference `19.67/20.80/19.23/19.50/19.34`，
  合计 `55.90/56.59/55.41/55.84/55.44`
- 检出结果 10/10 完全一致：`person 0.813 → 5.67 / 1.61 / 5.89 m`（说明不是"优化改变了计算"）

**为什么这条比 bench 更有说服力 —— 两个"不该变"的对照组同时成立**：

| 对照组 | 为什么理论上不该变 | 实测 |
| :--- | :--- | ---: |
| MPP 硬解 | 跑在 `rkvdec` 硬件上，CPU 只下发/取帧 | +5.8%（无变化 ✔） |
| letterbox | `cv::resize` 在 OpenCV 的 `.so` 里，早已编译好 | +3.2%（无变化 ✔） |

两个走"硬件 / 预编译库"的层纹丝不动，而**唯一的自有 CPU 热点（CSC）恰好提速 1.55×** ——
说明加速不是测量漂移、不是负载波动，而是 `-O3 + -mcpu=cortex-a76.cortex-a55` 真的作用在我们的代码上。
（±5% 内视为无变化。）

> ⭐ **口径澄清**：历史文档里的"CMake 加 Release 白捡 1.55×"**是对的**，但成立的**不是全链路，而是 CSC 这一层**（1.549×）。
> 全链路实际收益是 **−27.4%（1.378×）**。对外引用请说清层次，别说"全链路提速 1.55×"。

---

## 8. 局限（引用时必须一起说）

1. **第 4 节 bench 未含 CSC / letterbox / 图像 IO；第 7 节已用生产二进制补测全链路**，
   两者结论方向一致（CPU 侧受益、NPU 侧不变）。
2. 第 7 节的 A/B 只在 **640x640 的 `real_000.jpg`** 上做（该图 letterbox `scale=1.000`，无填充），
   1280x720 输入下 CSC 耗时约翻 2.25 倍，**绝对秒数不能直接外推到 720p**，但倍率方向一致。
3. 测量时 `w4_edgevision_gateway` 常驻（占 CPU ~55%），两组同一负载，**相对差成立**，
   但绝对耗时比空载偏高（空载下两组都会更快）。
4. `bench_dflfree` 的 DFL 解码与生产 `postprocess_dfl_free.cpp` 是同一份源码，
   但 bench 输入灰图（检出 0 框），**NMS 部分负载偏低**，所以第 4 节第 ③ 项对真实场景是乐观值。
5. n=6（bench）/ n=5（生产 A/B）交替测量，未做统计显著性检验（但组内波动 <1%、差异量级远大于噪声）。
6. **CSC 仍是全链路第一瓶颈**（O3 下 33.583 ms 占 60%）：编译优化只削掉它 1/3 的时间，
   要真正打下去得换 **RGA 硬件转换**（RGA CSC 是下一步 CPU 侧第一优先级）。

---

## 9. 复现命令

```bash
# 全新目录验证默认 Release（不传任何 -DCMAKE_BUILD_TYPE）
rm -rf /root/lead_build_check && mkdir -p /root/lead_build_check
cd /root/lead_build_check
cmake /root/edgevision/deploy/cpp -DCROSS_COMPILE_RK3588=ON
make -j8
grep CMAKE_BUILD_TYPE CMakeCache.txt
grep -E 'CXX_FLAGS|CXX_DEFINES' CMakeFiles/edgevision_pro_rknn.dir/flags.make

# 绑核跑 bench（BUILD_BENCH=ON）
cmake /root/edgevision/deploy/cpp -DCROSS_COMPILE_RK3588=ON -DBUILD_BENCH=ON
make -j8 bench_dflfree
taskset -c 6 ./bench_dflfree 200

# 生产二进制全链路 O0 vs O3（第 7 节那张表）
export EV_MODEL=/root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn
export LD_LIBRARY_PATH=/root/edgevision/deploy/cpp/3rdparty/rknn_api/lib:/root/edgevision/third_party/mpp/lib
IMG=/root/edgevision/data/calib_v2/real_000.jpg
for i in 1 2 3 4 5; do
  taskset -c 6 /root/edgevision/deploy/cpp/build_native/edgevision_pro_rknn  $IMG | grep -E 'CSC|Letterbox|DONE'
  taskset -c 6 /root/edgevision/deploy/cpp/build_release/edgevision_pro_rknn $IMG | grep -E 'CSC|Letterbox|DONE'
done
```

---

## 10. 坑（写下来免得别人重踩）

1. **CMake 变量名大小写**：`CMAKE_BUILD_TYPE` 的**值**是 `Release`，但对应的标志变量是
   **全大写** `CMAKE_CXX_FLAGS_RELEASE`。拼之前必须 `string(TOUPPER)`，否则
   `CMAKE_CXX_FLAGS_Release` 查不到 → 兜底逻辑静默失效。
2. **别在副本里做 A/B**：早期在 `/root/optim_ab/fakeroot/` 建了一份 `deploy/cpp` 副本，
   那份是**旧版 CMakeLists（缺兜底逻辑）**，cfg 日志全显示 `CXX flags : ' '` +
   `CMake Warning: 构建类型 'Release' 的 flags 里没有 -O`。最终必须在 `build_release/`
   或 `final/b_o0` / `final/b_o3` 这种干净目录重做。
3. **CMake 缓存粘性**：同一个 build 目录改过 CMakeLists 后重新 `cmake ..`，
   旧缓存值（尤其 `CMAKE_BUILD_TYPE`）不会自动更新，**必须 `rm -rf` 重来**。
