# lab/scripts/ 是什么

**25 个一次性调试脚本，从板端 `/root/lab/` 抢救回来（2026-11-02）。**

> ⚠️ **先看清楚：这批东西价值很低。**
> 它们不是工具，是当时"临时敲一行看一眼"留下的痕迹。
> 抢救回来是因为**它们记录了排查过程**（面试讲方法论时有用），
> 但**不要指望能直接跑**——绝大多数没有参数、没有注释、路径写死。

---

## 一、分三批

### 1. MPP 源码侦察（`s.sh` ~ `se.sh`，共 13 个）

当时为了搞清楚 MPP / RGA 的 API 签名，临时写的一堆 `grep` + `sed` 脚本：

| 脚本 | 当时想查什么 |
| :--- | :--- |
| `s.sh` / `s2.sh` / `s3.sh` | 进 `mpp-develop` 源码树看东西 |
| `s4.sh` | `mpp_buffer_group_get_internal` 的签名 |
| `s5.sh` | `wrapbuffer_handle` 在 `im2d_type.hpp` 里 |
| `s6.sh` | `/usr/include/rga/im2d.hpp` 第 30~90 行 |
| `s7.sh` | `RK_FORMAT_RGB_888` 在哪定义 |
| `s8.sh` | `improcess` 的签名们 |
| `s9.sh` | `IM_SYNC` 宏 |
| `sa.sh` | `im2d.h` 里所有 `wrapbuffer` / `importbuffer` 声明 |
| `sb.sh` | `/dev/rga` 设备节点 |
| `sc.sh` | `imcheck` 签名 |
| `sd.sh` | 色彩空间模式的枚举值 |
| `se.sh` | `im2d_type.h` 第 100~130 行附近 |

**共同点**：全是 `sed -n 'a,bp' 文件` 和 `grep 关键词 文件`。**换个板子就跑不了**
（依赖 `/usr/include/rga/`、`/tmp/mppsrc/mpp-develop/` 这些当时才有的路径）。

### 2. RGA 系列编译脚本（`b2.sh` ~ `b8.sh`，共 7 个）

每个对应 `lab/tools/` 里一个 `rga_*.cpp`，**编译 + 立即运行 + 限时 60 秒**：

| 脚本 | 编译哪个源文件 | 产物 |
| :--- | :--- | :--- |
| `b2.sh` | `dg.cpp` | `dg` |
| `b3.sh` | `rgatest.cpp` | `rgatest` |
| `b4.sh` | `rga_nv12.cpp` | `rga_nv12` |
| `b5.sh` | `rga_min.cpp` | `rga_min` |
| `b6.sh` | `rga_drm.cpp` | `rga_drm` |
| `b7.sh` | `rga_dump.cpp` | `rga_dump` |
| `b8.sh` | `rga_final.cpp` | `rga_final` |

统一的编译参数（以 `b2.sh` 为例）：

```bash
D=/root/edgevision/third_party/mpp
g++ -O2 -std=c++11 -o dg dg.cpp -I$D/include -L$D/lib \
    -lrockchip_mpp -lpthread -ldl -lrt -Wl,-rpath,$D/lib
[ -x ./dg ] && timeout 60 ./dg
```

> **这 7 个是 RGA 尝试里最有用的部分**——想重跑某个 `rga_*.cpp` 直接照抄对应脚本即可。
> 前提是板上 `/root/edgevision/third_party/mpp/lib/` 还在。

### 3. 其它

| 脚本 | 是什么 |
| :--- | :--- |
| `c.sh` | 进 `deploy/cpp`（编译尝试） |
| `t.sh` | **MPP 解码器独立编译验证**：先 `ls` 检查 `mpp_jpeg_decoder.h/.cpp` 在不在，再现场写一个最小 `main()` 编进去，确认 `MppJpegDecoder` 能独立链接——不用启动整个项目就能验证 MPP 库配置对不对 |
| `rg6.sh` / `rg7.sh` | 进 MPP 源码树，针对 RGA 的探查（**中文注释已损坏成 `??`**） |

---

## 二、为什么还要留着

**它们记录了一个真实的排查过程**：为了判断 RGA 能不能做硬件 CSC，
先把 RGA 和 MPP 的头文件翻了个遍，确认 API 存在、参数怎么传，然后才动手写
`lab/tools/rga_*.cpp`。这个过程本身是"遇到新库怎么开始"的示范。

如果你只想看结论：**RGA 硬件 CSC 没做成**，`docs/` 的「明确未实现」表里有记。

---

## 三、注意

- **不要试图运行这些脚本**，它们缺上下文（源码树路径、设备状态、前置文件）。
- `rg6.sh` / `rg7.sh` 的中文注释已损坏（板端原始文件就是 `??`，不是这次拉取造成的）。
- 全部 25 个共 11 KB。
- 板端 `/root/lab/` 里还有 `01_*.sh` ~ `38_*.sh` 那批编号脚本——那些是**有体系的**，
  放在 `lab/tools/`，不要在 `scripts/` 里找。
