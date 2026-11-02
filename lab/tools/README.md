# lab/tools/ 是什么

**攻坚期留下的工具与探针源码，51 个，从板端 `/root/lab/` 抢救回来（2026-11-02）。**

> ⚠️ **为什么在这里**
> 这些文件原先只在板上有一份，仓库里没有。它们不是"垃圾"——
> `bench_npu.cpp` 是 BENCHMARKS 里所有 NPU 数字的量具，
> `03_fps_matrix.sh`~`08_odd_fps.sh` 是「摄像头只有 20 fps」这个坑的完整排查链，
> `rga_*.cpp` 是 RGA 硬件 CSC 的尝试记录（**结论：没做成，见下文**）。
> 全部经 md5 逐文件校验，与板端一致。

---

## 一、摄像头 20 fps 诊断链（01~08）—— 已结案

`docs/tech_notes/CAMERA_FPS_ROOT_CAUSE.md` 的完整排查过程就在这里。按顺序：

| 文件 | 第几步 | 干什么 |
| :--- | :--- | :--- |
| `01_probe.sh` | 1 | 环境侦察 |
| `02_caps.sh` | 2 | 能力侦察（不干扰 service，只查询） |
| `03_fps_matrix.sh` | 3 | 帧率矩阵（最终版） |
| `04_verify.sh` | 4 | 复核异常 + 在正主 cam1 上验证 |
| `05_clean_matrix.sh` | 5 | 干净状态三点对照，查明 25 fps 异常 |
| `06_fps_sweep.sh` | 6 | 帧率生效性扫描：请求值 vs 实际值 |
| `07_longrun.sh` | 7 | 30 fps 长时稳定性 + cam1 稳定性复核 |
| `08_odd_fps.sh` | — | 奇数帧率异常探针 |

**结论已固化**：根因是代码从没设 `CAP_PROP_FPS`，显式请求 30 fps 后 33.33 ms/帧、400 帧零抖动。

---

## 二、MPP / JPEG 硬解侦查（10~14）

| 文件 | 干什么 |
| :--- | :--- |
| `10_mpp_probe.sh` | MPP / JPEG 硬解能力侦察（只读，不碰摄像头） |
| `11_dec_test.sh` | 决定性实验：用官方 `mpi_dec_test` 验证 jpegd 能否硬解 MJPEG |
| `12_demo_usage.sh` | 官方 demo 用法 |
| `13_headers.sh` | rockchip 头文件清单 |
| `14_cmd_h.sh` | `rk_mpi_cmd.h` 的 DEC 控制命令定义 |

---

## 三、真实链路验证（30~38）—— 0 检出排查链

| 文件 | 干什么 |
| :--- | :--- |
| `30_verify_pro.sh` | 上板验证 `edgevision_pro` 真实链路编译 + 跑真实 jpg |
| `31_rerun_pro.sh` | 复测真实链路 ×3（热身后稳态） |
| `32_csc_diag.sh` | 查 `build_native` 的编译优化等级 |
| `33_dump_check.sh` | 0 检出排查：把 `to_rgb888` 输出 dump 成 PPM，人眼验证 |
| `34_cmp_mpp_cv2.sh` | 数值对比：MPP CSC 输出 vs `cv2.imdecode` 参考 |
| `35_eval_dump_a.sh` | 降 conf 到 0.01 复测，排除「图里真没目标」和「阈值偏高」 |
| `36_eval_cam1.sh` | 用 conf=0.01 口径验证 cam1_focused.jpg |
| `37_eval_control.sh` | 决定性对照：已知高分校准图（当年 9/9 检出 avg 0.8072） |
| `38_final_verify.sh` | 终验：真实目标图走 MPP 全链路 |

> 这一串是**测量方法论**的样本：先排除"图里真没目标"，再排除"阈值偏高"，
> 最后用已知答案的对照组定性。面试讲"怎么确认不是模型的问题"用这个。

---

## 四、性能探针（量具）

| 文件 | 量什么 | 对应文档 |
| :--- | :--- | :--- |
| `bench_npu.cpp` | 单核/三核/延迟分位数、逐层耗时（`--perf`） | `BENCHMARKS.md` 2.2 |
| `bench_multicore.cpp` | 三核并行度 | `BENCHMARKS.md` 三核章节 |
| `bench_dflfree.cpp` | DFL-Free 前后对比 | `BENCHMARKS.md` DFL 章节 |
| `bench_cam_ts.cpp` | OpenCV 逐帧打点，看帧间隔分布 | `BENCHMARKS.md` 帧率章节 |
| `probe_cv_threads.cpp` | OpenCV 线程数对解码的影响 | — |
| `eval_dump.cpp` | 精度评测 dump | `BENCHMARKS.md` 量化章节 |

---

## 五、RGA 硬件 CSC 尝试（**结论：未做成**）

`rga_drm.cpp` / `rga_dump.cpp` / `rga_final.cpp` / `rga_min.cpp` / `rga_nv12.cpp` / `rgatest.cpp`

这些是尝试用 RGA 做硬件 CSC（替代 CPU 侧色彩空间转换）的过程记录。
**最终没有做进主线**——`README.md` 与 `AGENTS.md` 的「明确未实现」表里都列着
「RGA 硬件 CSC」「DMA-BUF 零拷贝」。**这些文件是失败的尝试，不是成果，面试别提成"做了"。**

> ⚠️ **这 6 个文件的中文注释已损坏**：板端原始文件里中文全部变成了 `??`
> （`file` 显示 ASCII text，说明写入时编码就丢了，**不是这次拉取造成的**）。
> 代码本身完好能编译，只是注释读不懂了。同类还有 `dg.cpp`、`test_dec.cpp`、
> `lab/scripts/rg6.sh`、`rg7.sh`，共 10 个。

---

## 六、其它

| 文件 | 是什么 |
| :--- | :--- |
| `postprocess.cpp` / `postprocess_dfl_free.cpp` | 后处理源码副本（主线在 `deploy/cpp/src/`） |
| `rknn_engine_dfl_free.cpp` | RKNN 引擎副本（主线在 `deploy/cpp/src/`） |
| `rknn_convert_v2.py` | ONNX→RKNN 转换脚本（v2） |
| `preview_detect.cpp` | 8080 预览的早期版本 |
| `start_preview_real.sh` | 启动 8080 预览 |
| `dg.cpp` | DRM buffer 试验（中文注释已损坏） |
| `test_cap.cpp` / `test_dec.cpp` / `test_accuracy.cpp` / `test_dfl_free.cpp` | 各项单点验证 |
| `step1.cpp` / `step2.cpp` / `step3.cpp` | 分步验证 |
| `q3_while.cpp` | 第三阶段探针 |
| `_diag_sgbm.sh` | SGBM 并行诊断（只读，不碰摄像头） |
| `verify_task5.sh` | task-5 板上验证（全程不 stop `edgevision.service`） |

---

## 七、注意

- **这些是草稿，不是交付物。** 结论已固化进 `docs/` 的才算数。
- 板端 `/root/lab/` 里同目录还有**编译出来的二进制**（如 `bench_npu`、`rga_drm`），
  没有回收——二进制能从这些源码重新编出来。
- 有 14 个文件与工作区 `_tools/` 里的**完全一致**（md5 校验过），没有重复搬运。
- 全部 51 个文件共 135 KB。
