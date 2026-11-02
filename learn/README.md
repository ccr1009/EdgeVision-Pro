# learn/ 是什么

**学习线的代码归档。每条学习线一个目录，对应 `docs/LEARNING_PLAN_4WEEKS.md` 的周次。**

> ⚠️ **为什么有这个目录（2026-11-02）**
>
> 这 26 个文件原先只存在于板上 `/root/lab/`，**仓库里没有任何副本**。
> `AGENTS.md` 第 3 节记的「第 1~4 周全部通关」——`fork/waitpid`、`pthread_mutex` 竞态复现、
> epoll 网关、`daemon(0,0)`——**证据就是这些 .cpp**。板子一坏就全没了。
>
> 同批抢救回来的还有 `lab/tools/`（51 个攻坚工具）和 `lab/scripts/`（25 个一次性脚本）。
> 全部经 md5 逐文件校验，与板端**102/102 一致**。

---

## 一、目录与周次对应

| 目录 | 对应周次 | 主题 | 文件数 |
| :--- | :--- | :--- | :--- |
| `w1_系统编程/` | 第 1 周 | 文件 IO / 系统调用 / 进程 | 10 |
| `w2_多线程/` | 第 2 周 | 线程 / 互斥 / 条件变量 / 线程池 / 原子 | 8 |
| `w3_网络/` | 第 3 周 | Socket / epoll / 边缘网关推流 | 7 |
| `w4_服务化/` | 第 4 周 | 守护进程 + 信号（第 4 周其余已在 `lab/`） | 1 |

> **第 4 周不完整是有原因的**：`w4_d23_signal.cpp`、`w4_d24_epoll.cpp`、
> `w4_edgevision_gateway.cpp`、`w4_edgevision_preview*.cpp` 这几个当时就已经在
> `lab/` 里了（因为它们是项目主线的一部分，不是纯练习），所以没有重复放到这里。
> 找它们去 `lab/`。

---

## 二、逐个说明（按 `LEARNING_PLAN_4WEEKS.md` 的实验编号）

### w1_系统编程（第 1 周）

| 文件 | 练什么 | 关键点 |
| :--- | :--- | :--- |
| `w1_e1_temp.cpp` | `open`/`read`/`close` 读温度 | 读 `/sys/class/thermal/thermal_zone0/temp`，毫摄氏度 |
| `w1_e1_loop.cpp` | 同上 + 循环 10 次 + `sleep` | 文件描述符必须有 open/close 配对 |
| `w1_e2_filesize.cpp` | `stat` 与 `open + lseek(SEEK_END)` 两种测文件大小 | 两种方法对照 |
| `w1_e2_test_bug.cpp` | 故意写错，看 `perror` 报什么 | 排错训练：错误码不是拿来猜的 |
| `w1_e3_ioctl.cpp` | `ioctl` 读终端窗口尺寸 | `TIOCGWINSZ` |
| `w1_e3_led.cpp` | `write` 控制板载 LED | 亮度节点是字符串不是整数 |
| `w1_e4_process.cpp` | `fork` / `execvp` / `waitpid` | 父等子，拿退出码 |
| `w1_e4_zombie.cpp` | 制造僵尸进程然后回收 | `<defunct>` 长什么样 |
| `w1_e5_proc.cpp` | `opendir`/`readdir` 遍历 `/proc` | Mini-ps，扫 247 个进程 |
| `w1_e6_thermal_logger.cpp` | 遍历 7 大温区，格式化追加写日志 | 第 1 周综合练兵 |

### w2_多线程（第 2 周）

| 文件 | 练什么 | 关键点 |
| :--- | :--- | :--- |
| `w2_d8.cpp` | `pthread_create` / `join`，`void*` 传参 | 线程函数签名固定 |
| `w2_d9_race.cpp` | **复现竞态**：2 线程各加 100 万，结果少于 200 万 | 实测丢失，不是理论 |
| `w2_d9_mutex.cpp` | 用 `pthread_mutex_t` 修掉上一条 | 对比才有说服力 |
| `w2_d10_cond.cpp` | 条件变量 + 生产者消费者 | `while` 防虚假唤醒 |
| `w2_d11_pool.cpp` | 线程池，3 个 worker 抢任务 | 锁粒度最小化 |
| `w2_d12_atomic.cpp` | `std::atomic` 无锁自增 | 与 mutex 对照 |
| `w2_d13_queue_pool.cpp` | 队列 + 线程池，模拟摄像头帧 | 向项目结构靠拢 |
| `w2_step1.cpp` | 第 2 周阶段小结 | — |

> 这一周的代码直接长成了项目的 NPU 三核调度池——`w2_d11_pool.cpp` 就是它的原型。

### w3_网络（第 3 周）

| 文件 | 练什么 | 关键点 |
| :--- | :--- | :--- |
| `w3_d15_server.cpp` | TCP 服务端基础 | `socket`/`bind`/`listen`/`accept` |
| `w3_d16_bbox_stream.cpp` | 传检测框结构体 | 结构体直接发有对齐问题 |
| `w3_d18_udp_broadcast.cpp` | UDP 广播 | 无连接、可能丢 |
| `w3_d19_blocking_bad.cpp` | 阻塞 socket 的问题 | 一个客户端卡住整个服务 |
| `w3_d19_nonblocking.cpp` | 非阻塞 + 轮询 | 修上一条，但空转浪费 CPU |
| `w3_d20_edge_gateway.cpp` | **边缘网关雏形** | 多路接入 |
| `w3_d20_real_npu_gateway.cpp` | 网关 + 真 NPU 推理 | 从练习走向项目 |

> `w3_d20_real_npu_gateway.cpp` 是 **9000 推流网关的前身**。
> 主线版本后来演进成 `lab/vision/edgevision_gateway.cpp`（即 `edgevision.service` 跑的那个）。

### w4_服务化（第 4 周）

| 文件 | 练什么 | 关键点 |
| :--- | :--- | :--- |
| `w4_d22_daemon_gateway.cpp` | `daemon(0,0)` 把进程变成守护进程 | 只有 53 行，**不含** `sigaction` / `SIGPIPE` |

> ⚠️ **别被文档带偏**：`CURRENT_STATUS.md` 说第 4 周做到「`daemon(0,0)` + `sigaction` 收
> SIGINT/SIGTERM + 全链路屏蔽 SIGPIPE」。实测这个 53 行的文件里 **`sigaction` 出现 0 次、
> `SIGPIPE` 出现 0 次**，只有 `daemon(0,0)`。
> 信号处理那部分可能在 `lab/drafts/w4_d23_signal.cpp`（主线文件，不在这里），SIGPIPE 则是在
> `lab/vision/edgevision_gateway.cpp` 里。**讲学习线时以文件实际内容为准。**

---

## 三、怎么编译这些文件

板端原生编译（**别交叉编译**，glibc 版本对不上）：

```bash
# 单个
g++ -O2 -std=c++17 w1_e1_temp.cpp -o w1_e1_temp

# 多线程那几个要带 -pthread
g++ -O2 -std=c++17 w2_d9_race.cpp -pthread -o w2_d9_race

# 网络那几个
g++ -O2 -std=c++17 w3_d15_server.cpp -o w3_d15_server
```

`w3_d20_real_npu_gateway.cpp` 要额外链接 RKNN 和 OpenCV，参考 `lab/build/build_w4_gateway.sh` 的编译参数。

---

## 四、注意

- **这些是学习代码，不是交付物**。面试讲学习线时看这里，讲项目时看 `lab/` 和 `deploy/cpp/`。
- **板端 `/root/lab/` 里还有大量同名二进制产物**（编译出来的可执行文件），没有回收——
  二进制能从这些源码重新编出来，不需要入库。
- 板上 `/root/lab/` 还有 `.log`、`.jpg`、`.bin`、`.yuv` 等运行产物，同样没有回收。
- 本目录 26 个文件 + `lab/tools/` 51 个 + `lab/scripts/` 25 个，**总计 192 KB**。
