# _tools/ —— 本地辅助脚本

> 39 个脚本，**全部是 Linux 侧跑在板子上的 shell / C++ / Python**，不是 Windows 工具。
> 用法一律：`scp _tools/xxx.sh root@<board-ip>:/root/ && ssh root@<board-ip> 'bash /root/xxx.sh'`
> （别在 PowerShell 里内联复杂引号的 ssh 命令，见根 `README.md` 第 5 节）
>
> ⚠️ 这些脚本**含板子路径假设**（`/root/edgevision`、`/root/lab`、`/root/rkllm_deploy`），
> 不保证换个目录结构还能跑。它们的价值是**证据链和排查方法**，不是通用工具。

---

## 一、现役 —— 日常会用到（5 个）

| 脚本 | 作用 | 什么时候用 |
|---|---|---|
| `npu_doctor.sh` | **NPU 体检六节**：驱动/频率/负载 → 谁在占 NPU → 残留 rkllm 实例 → systemd 自启 → 历史失败次数与 `used:` 值 → 结论与四步处理法 | 任何 `rkllm_init failed` 之前/之后，第一条命令 |
| `run_agent.sh` | **安全启动 Agent**：清残留实例 → 停 `edgevision.service` 与看板 → 起 Agent → `trap` 退出时自动恢复服务 | 每次跑 `rkllm_embodied_agent` 都用它，别手动起 |
| `quantify_iommu_by_config.sh` | IOMMU 容量定量实验的**三配置版**（干净 / 只开 gateway / gateway+看板），逐配置测「第几个实例开始失败」 | 验证任何「NPU 内存不够」类怀疑 |
| `setup_board_ssh.py` | 生成密钥并配 `~/.ssh/config` 别名 `rk3588`，写入板子 `authorized_keys` | 换新电脑时一次性 |
| `collect_calib.sh` | 从板端双摄实拍采集 INT8 校准集 | 重新量化模型时 |

---

## 二、`rkllm_init failed` 战役 —— 一次性排查链（20 个，最后全部固化进 TROUBLESHOOTING 第 27 条）

这 20 个脚本是同一次排障的**完整证据链**，按方法论分四组。留着是因为它们示范了「怎么把
一个'昨天还好好的今天坏了'的问题做成可复现的定量实验」。

### 假设检验（A/B 对照）
| 脚本 | 验证的假设 |
|---|---|
| `ab_test_rkllm.sh` | 根因是「驱动版本太老」还是「NPU/CMA 内存被抢占」——决定性 A/B，测完自动恢复服务 |
| `proof_two_instances.sh` | NPU IOMMU 4 GiB 空间被吃满的机制：跑一个常驻实例再起第二个，拿 `used:` 值 |
| `proof_gateway_conflict.sh` | 只跑 gateway（开机自启的那个）时 rkllm 还能不能加载 = 用户最可能的「日常状态」 |
| `proof_iommu_leak.sh` | 「`edgevision_dashboard` 泄漏 IOMMU」这个假设是否为真（**结论：假**） |
| `proof_iommu_timeline.sh` | dashboard 运行时 rkllm 何时可用、何时失败的精确定位 |
| `proof_leak_on_kill.sh` | rkllm 进程被强杀/异常退出后 IOMMU 映射是否泄漏（**结论：不泄漏**） |
| `repro_user_exact.sh` | 用用户原始配置精确复现 `205799424 / flags: 0x2 / errno: 14` |

### 定量测量
| 脚本 | 作用 |
|---|---|
| `quantify_iommu.sh` | 逐个加载 rkllm，测每个吃多少 IOMMU、第几个开始失败 |
| `measure_iommu_used.sh` | 常驻 1 个实例后，故意触发第二次分配失败，让驱动 dump 出 IOMMU 已用量 |
| `probe_iommu_capacity.sh` | 探针法精确测量：**驱动只在分配失败时 dump**，所以用「撑到失败」读总量 |
| `probe_iommu_v2.sh` | 同上 v2：改用 `tail -f /dev/null` 供 stdin，避开 FIFO 阻塞问题 |
| `iommu_map_inspect.sh` | 看 IOMMU debugfs 有没有可读的映射表 |

### 分层诊断（diag 四连）
| 脚本 | 看什么 |
|---|---|
| `diag_rkllm.sh` | 系统负载 + 内存总览 |
| `diag_rkllm2.sh` | `dma_buf/bufinfo` 明细：谁在吃 CMA/DMA |
| `diag_rkllm3.sh` | 失败时刻 dmesg 的完整上下文（`dma map ... fail` 前后 60 行） |
| `diag_rkllm4.sh` | 失败时刻 dmesg 的**原始**前几行（确认是谁打印的 IOVA dump） |

### 复现与追溯
| 脚本 | 作用 |
|---|---|
| `repro_rkllm.sh` | 用真正报错的 binary 复现 + 查 NPU 驱动内存池 |
| `final_repro.sh` | 最终验证：复现用户真实场景 + 试「减小 context」是否能规避 |
| `trace_history.sh` | 追溯：用户 21:07 失败前系统上还跑过什么（shell 历史 + 日志） |
| `find_orphan.sh` | 列出所有残留进程（含 LED 爆闪子进程 / `rkllm` / `brightness` / `trigger`） |

> 四个被**证伪**的假设（都在这里留下了否定证据）：驱动 0.9.2 太旧、CMA 不足、
> dashboard 单独泄漏、gateway 冲突。真根因是 **32 位 IOMMU 地址空间 = 4096 MiB**。

---

## 三、性能测量 —— 早期逐层剖析与优化验证（5 个）

| 脚本 | 作用 | 结论落在哪 |
|---|---|---|
| `diag_npu.sh` | 单帧 37ms 到底是 NPU 在算还是偷偷回退到 CPU | BENCHMARKS 2.2 节（49% 耗在 DFL 头 CPU 回退） |
| `build_and_perf.sh` | 编译 + 逐层剖析（全走脚本文件，避开 Windows ssh 引号问题） | BENCHMARKS 2.2 节 |
| `run_bench.sh` | 板端 NPU 性能探针 + 后台采样三核负载 | BENCHMARKS 2.2 节 |
| `run_multicore.sh` | 三核并行吞吐测试 + 三核负载采样 | BENCHMARKS 2.2 节（2.21×，各核 49%） |
| `eval_models.sh` | 批量对比：新校准集模型 vs 旧合成校准集模型（同 runtime / 同 toolkit / 同 ONNX） | BENCHMARKS 2.4 节（cls 分支塌缩成 0.5） |

---

## 四、D20 / 学习期验证（9 个）

第 3 周「全链路闭环」与并发教学的验证代码。**已被 `edgevision-rk3588/lab/` 里的正式版取代**，
留作当时的学习痕迹。

| 脚本 | 对应学习节点 |
|---|---|
| `d20_step0.sh` | D20 第 0 步：亲眼看见 `if` 和 `while` 的区别 |
| `d20_step1_npu_threads.cpp` + `run_d20_step1.sh` | D20 第 1 步：3 线程真 NPU 并行 |
| `d20_gateway_npu.cpp` + `run_d20_gateway.sh` | D20 真 NPU 网关：编译 + 后台启动，供 PC 端连接 |
| `d20_ctl.sh` | 启动/停止/查看板上的 D20 网关 |
| `test_d20_from_pc.py` | PC 侧上位机连 9000 端口验证 |
| `q3_spurious.cpp` + `run_q3.sh` | 虚假唤醒演示（条件变量 `while` 循环的必要性） |

---

## 与 `_workbench/tools_residue/` 的区别

`_tools/` 只留**有明确复现价值的脚本**。清出去放 `_workbench/tools_residue/` 的是：
工具调用痕迹 `_*.json`、`perf.log`、`r1/r2.txt`、4 个已被 `docs/resumes/_build_resume_v12.py`
取代的旧简历生成器、硬编码路径已失效的 `organize_workspace.py`、ACL 报告、会话备份。
