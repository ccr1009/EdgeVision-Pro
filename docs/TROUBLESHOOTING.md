# 问题记录（TROUBLESHOOTING）

> 按「现象 → 排查 → 根因 → 解法」记录，**每一条都是真踩过的**，不是抄来的通用 FAQ。
> **主用途是校招面试素材**（顺带让自己别再踩）：只收「根因讲得出原理 + 排查有方法论 + 有量化证据」的坑，
> 纯环境琐事不展开。
>
> 相关文档：技术数据见 `BENCHMARKS.md`；项目总纲见根目录 `AGENTS.md`。

---

## 🎯 面试主打（时间紧只看这 6 条）

按「能扛住几轮追问」排序。面试前背这 6 条，其余都是备查。**第 23 条是唯一一条「非性能类」的**——
它考的不是算得多快，而是**验证方法论**：演示全绿之后，你怎么发现它其实错了。

| # | 条目 | 一句话讲点 | 会被追问什么 |
| :-- | :--- | :--- | :--- |
| **3** | 校准集换色块，分数全变 0.5 | 量化 scale 被窄分布锁死；余弦相似度 0.9965 却已失效 | 「相似度高为什么模型还废了？」→ 分支塌缩 |
| **5** | 解码 3.19 → 1.79 ms | 遍历顺序跨 2.7 MB，cache 全废 | 「同样的算法为什么 ARM 上差一倍？」→ cache line |
| **1** | DFL 解码整体差 8 倍 | 格子单位 ≠ 像素，必须乘 stride | 「你怎么发现量纲错了？」 |
| **11** | 两个口径给出相同数字 | 一定是口径定义错了，不是巧合 | 「你的计时点怎么定的？」 |
| **2** | 砍 DFL 头怎么证明没砍错 | `max\|Δ\| = 1.2e-4 px`，已是 float32 极限 | 「凭什么说等价？」→ 构造误差基准 |
| **23** | 爆闪的灯被读成「熄灭」 | 周期性物理量单点采样 = 混叠；模型忠实推理了错误输入 | 「你怎么确定不是模型幻觉？」→ 分层定位 |

**补充弹药**：第 **4** 条（张量布局先打 dims）、第 **10** 条（全 0 文件的分层排除）、第 **12** 条（mAP 口径与局限）。

> 第 **8、9** 条是纯环境琐事（Windows 路径映射、`pkill` 自杀），面试价值低，保留只为「自己别再踩」，不用背。

---

## 一、模型与算法

### 1. DFL 解码结果整体差 8 倍

**现象**：自己写的 DFL 解码，检出的框全部偏大，坐标刚好是正确值的 8 倍（stride=8 那一路）。

**排查**：把 numpy 解码结果和原模型的 84 通道输出逐元素比对，发现前 4 个通道（box）我已经按 stride 乘过，
但 DFL 的 16 bin 加权和本身就是错的量纲。

**根因**：DFL（Distribution Focal Loss）对 16 个 bin 做 softmax 再和 `[0..15]` 加权求和，得到的是
**格子单位**的距离（0~15），不是像素。必须再乘 stride 才是像素坐标：

```python
# 错：dist 已经是像素了
x1 = anchor_x - dist_lt
# 对：
x1 = (anchor_grid_x - dist_lt) * stride
```

**教训**：跨量纲的地方一定要打印中间值。我当时如果早点把 `dist` 的范围打出来（0~15 而不是 0~120），
一眼就能看出问题。

---

### 2. 砍掉 DFL 头之后，怎么证明「没砍错」

**现象**：把 ONNX 图切开、只保留两个原始输出后，没法确定自己的解码逻辑和原模型完全等价。

**做法**：不去看代码，而是**用数据证明等价**——拿原模型（84ch 输出）当基准，
和「新模型 2 输出 + 我的 numpy 解码」逐元素比：

| 对比项 | 结果 |
| :--- | ---: |
| box 坐标 | max\|Δ\| = **0.000122 px** |
| cls 分数 | max\|Δ\| = **0** |

0.000122 已经是 float32 在 640 量级上的精度极限，说明切图没有引入任何语义损失。

**教训**：重构类的改动，不要用「看起来对」来验收，要构造一个**能算出误差的基准**。

---

### 3. 校准集换成纯色块图，模型「还能检出」，但分数全是 0.5

**现象**：用合成色块图（纯色矩形 + 纯灰底）做校准集转 INT8，模型在真实画面上框还是对的，
但 9 张图的置信度**全部精确等于 0.5000**。

**排查**：`0.5 = sigmoid(0)`，分数全是同一个值说明 cls 分支输出被量化成了常数。
换成真实实拍 + 增强样本重转后，平均分数回到 0.8072（FP32 是 0.8125）。

**根因**：合成图里根本没有人/物体，cls 分支的激活值分布极窄，量化 scale 定在了一个很小的区间；
真实画面一进来就整体饱和。而 box 分支（回归）分布相对稳定，所以框没坏。

**关键反例**：这个坏掉的模型，**特征图余弦相似度还有 0.9965**。
所以「余弦相似度很高」不能证明模型可用，它只能说明特征图的整体形状接近，掩盖不了某个分支塌缩。

**教训**：
- 校准集必须和**真实推理分布**一致，不能拿合成数据凑数；
- 评估不能只看一个指标，「框对」和「分对」是两件事，要分开验证。

---

## 二、C++ 与部署

### 4. RKNN 输出识别失败：8400 在最后一维还是在中间维

**现象**：写自适应代码从输出张量里找「64 通道」和「80 通道」，运行时报「输出识别失败」。

**排查**：打出来的 dims 是 `[1,64,8400]`，说明布局是 `(1, C, N)`，通道在**倒数第二维**。
我第一版写的是 `nhwc ? dims[n-1] : dims[n-1]`，两个分支取错了位置。

```cpp
// 看 8400 落在哪一维：
//   [1, 64, 8400] -> 8400 在最后 -> 布局 (1,C,N)，通道取 dims[n-2]
//   [1, 8400, 64] -> 8400 在中间 -> 布局 (1,N,C)，通道取 dims[n-1]
nhwc = (d != a.n_dims - 1);
ch   = nhwc ? a.dims[a.n_dims - 1] : a.dims[a.n_dims - 2];
```

**教训**：跨平台/跨版本的张量布局，不要靠「一般是 NHWC」假设，**先打印 dims 再写分支**。

---

### 5. 解码优化：3.19 ms → 1.79 ms，靠的是访存顺序

**现象**：DFL 解码 + NMS 在板子上要 3.19 ms，比预期高。

**排查**：cls 输出是 `(80, 8400)` 布局。按「anchor 外循环、类别内循环」遍历时，
每次读 `cls[c*8400 + a]` 都跨 33.6 KB，80 个类别就是跨 2.7 MB —— **cache 全废**。

**解法**：改成「类别外循环、anchor 内循环」，内层地址连续；同时先按阈值筛 anchor，
只对通过的做 softmax（避免 8400 次 softmax）。降到 **1.79 ms**。

**教训**：同样的算法，**访存顺序**在 ARM 上能差一倍。看到「数据量不大但很慢」，先怀疑 cache。

---

### 6. 检测框是假的 —— 预览和网关各有一处

**现象**：网页预览画面上一直有个绿框标着 "Person 96%"，位置还会缓慢移动，看起来像在检测。

**排查（第一处，预览）**：翻源码发现是 `float offset_x = (frame_id % 120) * 5;` —— 框的位置由帧号算出来，
标签字符串是硬编码的。更糟的是模型跑完之后 `outputs[0].want_float = 0`，
`rknn_outputs_get` 拿完立刻 `release`，**检测结果从头到尾没被使用过**。

**排查（第二处，9000 网关）**：修完预览、准备接新模型时，发现
`lab/vision/edgevision_gateway.cpp` 215~222 行是**同一个毛病的另一份拷贝**：

```cpp
// 构造检测结果（注入动态实测坐标）
res.header.bbox_count = 2;
float offset = (float)((frame.frame_id % 50) * 4);   // 靠帧号微移
res.boxes[0] = {0, 0.95f, 100.0f + offset, 150.0f, 80.0f, 200.0f};
res.boxes[1] = {1, 0.88f, 320.0f, 220.0f, 150.0f, 100.0f};
```

注释还写着「模拟真实跟踪」。这意味着 systemd 里那个「工业级网关」推给上位机的 800+ 帧，
**一个真实检测结果都没有**。

**解法**：预览侧重写为 `w4_edgevision_preview_real.cpp`（新模型 + `dflfree::decode` 画真框，
HUD 换成真实 `NPU ms + Decode ms + FPS + obj 数`）；网关侧把 `RKNNEngineDflFree` 接进
`npu_worker_thread`，用真 `Detection` 填 `BBox`（`CURRENT_STATUS.md` 待办 #4，正在做）。

**教训**：
- **「看起来在工作」和「在工作」是两件事**。凡演示代码，必须检查「输出有没有被消费」。
- 同一个假象会**复制粘贴到多处**。修完一处要全文搜同类模式（`frame_id %`、硬编码置信度）。
- 这类问题最伤简历：面试官只问一句「这个 0.95 是怎么算的」，前面讲的所有架构就全塌了。

---

## 三、工具链与环境

### 7. 在板子上装 rknn-toolkit2 的 8 个坑

原本以为转换必须在 x86 PC 上做（官方 wheel 是 `linux_x86_64`），后来发现 **2.x 版本有 aarch64 wheel**，
可以直接在板子上转。过程踩了 8 个坑，完整表格在 [`BENCHMARKS.md` 2.3.4 节](BENCHMARKS.md#234-转换环境的意外解法直接在板子上转不需要-wsl--pc)，摘要：

| # | 坑 | 解法 |
| :-- | :--- | :--- |
| 1 | 板子没有 pip（Debian 剥离了 `ensurepip`） | 手动下 pip 的 wheel，用 zipfile 解压到 site-packages |
| 2 | 清华 pypi 把板子 IP 限流（返回封禁页） | 换阿里云镜像 |
| 3 | `rknn.api` 顶层 `import torch`，而 aarch64 torch 有 1GB+ | 用 numpy 支撑的最小 torch 占位模块（`--stub-torch`） |
| 4 | 依赖版本必须精确，装默认最新版必崩 | numpy 1.26.4 / onnx 1.14.1 / protobuf 3.20.3 / onnxruntime 1.16.0 |
| 5 | opencv 5.x 与 numpy 1.26 冲突，pip 反复回退下载 | 直接钉死 `opencv-python-headless==4.9.0.80` |
| 6 | `quantized_iteration` 是 1.x 参数 | 2.x 直接删掉 |
| 7 | `dataset.txt` 里的相对路径被拼成 `calib/calib/xxx.jpg` | 写绝对路径（相对路径按清单所在目录解析） |
| 8 | 2.3.0 转的模型喂给 1.5.2 runtime | 板端 `librknnrt.so` 一起升到 2.3.0 |

**教训**：报错信息里出现 `ModuleNotFoundError` 时，先判断这个依赖**是不是真的需要**（比如 torch 只用来生成索引常量），
不要条件反射去装 1GB 的包。

---

### 8. Windows 上跑 Linux 路径的坑

**现象**：`curl` 把数据集下到 Git Bash 的 `/tmp`，Python 脚本读 `/tmp/coco128` 却是空的。

**根因**：Git Bash 的 `/tmp` 映射到 `C:\Users\<me>\AppData\Local\Temp`，
而 Windows 原生 Python 把 `/tmp` 当成 `C:\tmp`。

**解法**：涉及 Python 读写的路径一律用**绝对 Windows 路径**，数据集解压进项目目录。

---

### 9. `pkill -f <名字>` 把自己杀了

**现象**：`ssh root@board "pkill -f preview_real; ./start.sh"` 执行后 SSH 直接断开，脚本没跑。

**根因**：`pkill -f` 匹配的是**完整命令行**，而这条 ssh 命令本身也包含 `preview_real` 这个字符串，
于是它把自己所在的 shell 杀了。

**解法**：用 `pgrep -f '^./程序名'` 拿到 PID 再 `kill`（`^` 锚定命令行开头，排除自身）。

**教训**：任何「按名字杀进程」的命令，都要考虑它会不会匹配到调用者自己。

---

### 10. VSCode Remote-SSH 连不上：下载「成功」的文件其实全是 0

**现象**：VSCode 升到 **1.143.0** 后连不上板子，Remote-SSH 报
`Resolver error: Got bad result from install script`，日志停在 `> Downloading with wget`，
卡 49 秒（21:40:29 → 21:41:17）。

**排查（分层排除，别一上来就怀疑网线松了）**：

| 层 | 检查 | 结果 |
| :-- | :--- | :--- |
| 链路 | `ping <board-ip>` | 7ms 通 |
| 登录 | `ssh -o BatchMode=yes rk3588` | 免密登录成功 |
| 板端外网 | 板端 `curl -sI update.code.visualstudio.com` | HTTP/2 **200** |
| 磁盘 | `df -h /` | **41G 可用** |
| 写盘 | 板端 `dd if=/dev/urandom` 写 20MB 再统计非零字节 | 210MB/s 正常，**排除 overlay 写盘问题** |
| 下载物 | `gzip -t` 那个 tar.gz | **not in gzip format** |

再看文件真身：

```bash
head -c 64 xxx.tar.gz | od -c
# 0000000  \0  \0  \0  \0  \0  \0  \0  \0 ...
```

**11.7MB 的文件，内容全是 `\0`** —— 文件名对、大小也对，但数据一个字节都没下来。

**根因**：VSCode 1.143.0 对应 commit `2d0b7506`，而板子上只有 10/15 装的旧版 `6d62b1d5`。
新版连接时先下 **CLI 包（11.54MB）**、再下 server 包（208MB）。板子（Debian 11 + overlayroot，
走校园网/IPv6 出口）下 CLI 包时**断流**，wget 退出后留下一个「预分配了大小但全 0」的残包。
安装脚本解压失败 → 返回坏结果 → Remote-SSH 直接判死。

**解法（PC 下载 → scp → 手工布局）**：令 `C=2d0b7506a36c1d6db9a466440131a8e01eab2289`

```bash
# 1) PC 侧下载：实测 5 MB/s，41 秒拿完 208MB；板子却连 11MB 都下不来
curl -L -o server.tar.gz "https://update.code.visualstudio.com/commit:$C/server-linux-arm64/stable"
curl -L -o cli.tar.gz    "https://update.code.visualstudio.com/commit:$C/cli-linux-arm64/stable"

# 2) 推上去（208MB / 34 秒）
scp server.tar.gz cli.tar.gz rk3588:/root/.vscode-server/

# 3) server：解压后顶层目录是 vscode-server-linux-arm64/，必须改名成 server/
mkdir -p ~/.vscode-server/cli/servers/Stable-$C
tar -xzf server.tar.gz -C ~/.vscode-server/cli/servers/Stable-$C
mv ~/.vscode-server/cli/servers/Stable-$C/vscode-server-linux-arm64 \
   ~/.vscode-server/cli/servers/Stable-$C/server

# 4) CLI：包内是单文件 code，改名成 code-<commit>
tar -xzf cli.tar.gz && mv code ~/.vscode-server/code-$C && chmod +x ~/.vscode-server/code-$C
```

**验证（不要只看「文件在不在」，要真起一次）**：

```bash
cd ~/.vscode-server/cli/servers/Stable-$C/server
./bin/code-server --start-server --host 127.0.0.1 --port 18099 \
    --without-connection-token --accept-server-license-terms
# -> Server bound to 127.0.0.1:18099 (IPv4)
# -> Extension host agent listening on 18099
```

顺带证实：**node v24.21.0 能在 Debian 11 的 glibc 2.31 上跑**，不用为兼容性降级 VSCode。

**长期解**：PC 的 `settings.json` 加一条，以后升级让电脑下好再通过 SSH 传过去：

```json
"remote.SSH.localServerDownload": "always"
```

**教训**：
- **文件大小对 ≠ 下载成功**。判完整性用 `gzip -t` / `md5sum`，别用 `ls -l` 看大小。
- 排障要**分层**（链路 → 登录 → 外网 → 磁盘 → 写盘 → 下载物），一层层排除比瞎猜快得多。
- 板子下不了大文件是**环境固有属性**（校园网 + IPv6 出口），不是偶发故障，要从架构上绕开。
- 坑中坑：收尾时 `pkill -f "port 18099"` 又把执行它的那条 ssh 命令行杀了（见第 9 条），
  改用 `ss -lntp | grep 端口` 确认端口释放更稳。

---

## 四、测量与结论

### 11. 性能口径：把解码算进了「全周期」

**现象**：第一版测速表里，「全周期 IO」和「全链路」两栏数字一模一样，无法解释。

**排查**：计时点是 `t0`(开始) → `t1`(输入set后) → `t2`(run后) → `t3`(取输出后) → `t4`(解码后) → `t5`(release后)。
我把「全周期」写成了 `t5 - t0`，但 release 在 decode 之后，所以这一栏其实**包含了解码**。

**解法**：重定义 —— 纯推理 `t2-t1`、IO 周期 `t3-t0`、解码 `t4-t3`、全链路 `t4-t0`。
修正后数字才自洽：`15.95 + 1.53 + 3.09 = 20.58 ms`。

**教训**：性能表格里如果两个不同的口径给出相同数字，**一定是口径定义错了**，不是巧合。

---

### 12. 「9 张图 top-1 一致性」不能当 mAP 用

**现象**：一开始用 9 张真实图做量化前后对比，得出「平均置信度只掉 0.65%」。

**问题**：
- 样本量太小（9 张），说明不了统计意义；
- top-1 置信度是**分类口径**，而任务是检测，检测的核心指标是 mAP（要考虑框的位置和漏检）。

**解法**：用公开带标注的 **COCO128**（128 张 / 929 个 GT / 71 类）重做，两侧口径完全一致
（letterbox 640、conf 0.01、NMS IoU 0.45、COCO 101 点插值），得到
**FP32 mAP@0.5 = 0.6087 → INT8 = 0.6002，掉点 0.85 个百分点**。

**教训**：
- 指标要和任务对齐（检测就用 mAP，别拿分类指标代替）；
- 引用结果时必须带**局限**：COCO128 是 YOLOv8n 的训练子集，绝对 mAP 偏乐观，
  但因为 FP32/INT8 用同一批图，相对掉点仍然成立。

---

## 五、编译与链接

### 13. 头文件明明改了，编译器却报「未定义」——它吃的是旁边那份旧副本

**现象**：给 `RKNNEngineDflFree` 加了 `NpuCore` 枚举和 `init_from_memory`，
板端工程（`make`）编译通过、测试程序也跑通了。但编网关时报：

```
error: 'NpuCore' does not name a type
error: 'class RKNNEngineDflFree' has no member named 'init_from_memory'
```

而 `-I` 指向的 `/root/edgevision/deploy/cpp/include/rknn_engine_dfl_free.h` 里明明有这两样东西。

**排查**：不去怀疑缓存，先找同名文件：

```bash
find / -name "rknn_engine_dfl_free.h" 2>/dev/null
# /root/lab/rknn_engine_dfl_free.h                            ← 1062 字节，10/21 旧版
# /root/edgevision/deploy/cpp/include/rknn_engine_dfl_free.h   ← 新版，含 NpuCore
```

**根因**：`#include "xxx.h"` 用**双引号**时，搜索顺序是

> **源文件所在目录** → `-I` 路径 → 系统路径

`w4_edgevision_gateway.cpp` 就在 `/root/lab/`，那里躺着一份 10/21 的旧头文件，
于是它先被吃掉，`-I` 指向的新版**从头到尾没被打开过**。

**解法**：把源文件目录里的旧副本挪走即可：

```bash
cd /root/lab && mv rknn_engine_dfl_free.h rknn_engine_dfl_free.h.old_1021
bash build_w4_gateway.sh    # 立刻通过
```

**教训**：
- **`"xxx.h"` 和 `<xxx.h>` 不是一回事**：双引号优先搜源文件目录，尖括号只搜 `-I` 和系统路径。
  自己的头文件用双引号方便，但**代价是源文件旁边的同名文件会悄悄劫持**。
- 遇到「文件明明改了却报旧错误」，第一反应应该是 `find`/`ls` 找同名副本，
  而不是怀疑编译缓存、怀疑没保存、怀疑工具链。
- `-I` 加得再全，也压不过「源文件目录」的优先级。

---

## 六、并发与推流架构

### 14. 结果队列缺少背压控制，上位机重连导致瞬时 10000+ FPS 历史积压爆发与延迟雪崩

**现象**：开发板推流网关（端口 9000）在后台长期运行，上位机断开后隔几十秒重新连接，连接瞬间上位机终端疯狂刷屏，测算帧率飙到 **10800+ FPS**，且打印的帧号和检测框是几十秒前的历史陈旧数据，端到端延迟累积长达数十秒。

**排查**：
1. 为什么会有 10000+ FPS？网关单帧硬件推理稳态耗时 20ms（上限 50 FPS），物理上不可能凭空产生 10000 FPS 的新数据，这必然是网络层在疯狂倾泻**内存中积攒的存量旧数据**。
2. 检查多线程架构中的两个缓冲队列：
   - 图像输入队列 `g_raw_queue`：有 `while (g_raw_queue.size() >= 2) g_raw_queue.pop();`，当 NPU 消费不及时会自动丢弃旧帧；
   - 推流结果队列 `g_stream_queue`：代码里是无条件 `g_stream_queue.push(res);`！
3. 上位机断开时，NPU 3 个核心依然在全力推理，每秒产生约 30~50 帧结果塞进 `g_stream_queue`。上位机断开 1 分钟，队列就无界堆积了 2000~3000 个结构体。
4. 客户端一连上，TCP 发送线程从队列里把这几千帧存量数据一口气全部通过 Socket 吐给上位机，造成严重积压雪崩。

**根因**：
多线程异步流水线中，**生产者（NPU 持续硬件推理）与消费者（上位机网络发送）速率完全解耦**。当消费者通道中断（无客户端连入）时，若推流端没有**背压机制（Backpressure）或丢帧丢弃策略**，队列将发生无界内存膨胀；当通道恢复时，客户端接收到的全是严重过期的“死帧”，彻底违背实时流媒体系统的低延迟定义。

**解法**：双重防积压拦截：
1. **入队防积压**：入队前施加容量上限（保留最新 2 帧）：
   ```cpp
   pthread_mutex_lock(&g_stream_lock);
   while (g_stream_queue.size() >= 2) {
       g_stream_queue.pop(); // 保持队列永远不超过 2 帧
   }
   g_stream_queue.push(res);
   pthread_cond_signal(&g_stream_cond);
   pthread_mutex_unlock(&g_stream_lock);
   ```
2. **建联时清空存量**：在 `accept()` 客户端连入瞬间，直接将之前可能残留的旧数据清空，确保上位机收到的第一帧就是当前毫秒最新出炉的活帧。

**验证**：
修复后上位机断开 1 分钟再重连：上位机连入第一帧即为最新帧号，稳态聚合帧率稳定在 **19.2 FPS**，端到端延迟恢复到稳态 20~30ms。

---

## 七、摄像头与 V4L2

### 15. 摄像头「只有 20fps」——把默认协商值当成了物理上限

**现象**：预览链路 HUD 稳定 ~20 FPS，单帧 ~50ms。用 `bench_cam_fps` 单测纯采集也是
`50.02 ms/帧 => 20.0 fps`，驱动 `--get-parm` 也报 `Frames per second: 20.000`。
于是判定「**摄像头物理出帧率就是 20 fps**，处理链路刚好跟得上，20 FPS 是上限不是下限，
优化任何一环都不会提高 FPS」，并据此否掉了 MPP 硬解/硬编的优化路线。

**排查**（分层，从「硬件有没有能力」问起，而不是从「现在跑多少」问起）：

1. **先查硬件能力**——`v4l2-ctl -d /dev/video81 --list-formats-ext`：
   1280x720 MJPG 下明确列出 `Interval: Discrete 0.033s (30.000 fps)`。
   **硬件本来就支持 30fps，「物理上限 20fps」这个前提当场就塌了。**
2. **再查为什么协商成 20**——定位源码：
   `lab/vision/edgevision_gateway.cpp:133-134`、`lab/vision/edgevision_preview_real.cpp:259-260`、
   `lab/drafts/bench_cam_fps.cpp:11-13`，三处**都只设了 `FOURCC` + `WIDTH/HEIGHT`，没有一处设 `CAP_PROP_FPS`**。
   V4L2 拿不到请求就沿用设备默认档（20fps）。
3. **显式请求验证**——补一行 `cap.set(cv::CAP_PROP_FPS, 30)` 写对照工具 `bench_cam_fps2.cpp`，
   请求值 vs 回读值 vs 实测值三方对照。

**根因**：
UVC 摄像头的输出帧率是**离散档位**（这台设备 MJPG 支持 30/25/20/15/10/5），
由 V4L2 的 `VIDIOC_S_PARM` 协商决定。**OpenCV 的 `VideoCapture` 不会自动请求最高帧率**，
不调 `CAP_PROP_FPS` 就用设备默认值。默认值 **不是**上限。

更隐蔽的第二层：**`cap.get(CAP_PROP_FPS)` 的回读值会骗人**。
请求 25fps 时回读 `25.00`，硬件实际仍跑 20.0fps（实测 50.00 ms/帧）。
回读的是**软件层记录的期望值**，不是硬件真实节奏。

**对照数据（1280x720 MJPG，100 帧）**：

| 请求 FPS | 回读 FPS | 实测 ms/帧 | 实测 FPS | 结论 |
| ---: | ---: | ---: | ---: | :--- |
| 不设置 | 20.00 | 50.03 | 20.0 | 设备默认 |
| **30** | 30.00 | **33.32** | **30.0** | ✅ 兑现 |
| 20 | 20.00 | 50.00 | 20.0 | 兑现 |
| 25 | 25.00 | 50.00 | 20.0 | ❌ 回读骗人，实际未生效 |
| 15 | 15.00 | 33.33 | 30.0 | ❌ 跳到 30 |
| 10 / 5 | 10.00 / 5.00 | — | 15 / 10 | ❌ 跳档 |

**解法**：

```cpp
cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);
cap.set(cv::CAP_PROP_FPS, 30);   // ★ 必须显式设置
```

**验证（400 帧 ≈ 13s，两台摄像头）**：

| 设备 | 帧数 | 均值 | min | 中位 | p95 | max | 帧间隔分布 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| cam2 `/dev/video83` | 399 | 33.33 ms | 32.56 | 33.32 | 33.50 | 34.09 | 399 帧全在 30~40ms 档 |
| cam1 `/dev/video81` | 399 | 33.33 ms | 33.19 | 33.36 | 33.39 | 33.43 | 399 帧全在 30~40ms 档 |

**零抖动**（最大偏差 0.76ms），是稳定的 30fps，非均值凑数。

**局限**：只测「采集+解码」段；两台同型号（HDI Webcam USB），换摄像头必须重跑
`--list-formats-ext`；15/25/10/5 档为何失效未深挖；未在极暗/极亮环境复测
（已锁曝光 `exposure_auto=1, absolute=157` 与自动曝光结果一致，排除 AE 干扰）。

**面试讲法**：这题的价值在方法论，不在那个 30。
> 「我在项目上量出摄像头只有 20fps，就写成『物理上限 20fps，优化没意义』。
> 后来一查 `--list-formats-ext`，硬件写着支持 30——我测的是**默认值**，不是**上限**。
> 补上 `CAP_PROP_FPS` 之后实测 33.33ms/帧、400 帧零抖动。
> 这件事让我总结出两条：一，宣称上限前必须先确认所有可调参数已经调到最大；
> 二，**API 的回读值不等于硬件实际状态**，中间要隔一次实测打点。」

**连带影响（容易被忽略但很重要）**：
这个错误结论**把整条优化路线判错了方向**。在 20fps 的真实上限下，
MPP 硬解（7→1.5ms）、硬编（7.2→2ms）对 FPS 完全无感，所以被判「不值得做」；
修正后处理链路 34ms vs 出帧周期 33.3ms **刚好卡在临界**，
这 11ms 的优化才第一次能兑现成帧率。**一个测量前提错了，整个优先级排序都是错的。**

---

## 八、OpenCV 与图像处理

### 16. `initUndistortRectifyMap` 的 map2 类型由 m1type 决定——文档没写的隐式耦合

**现象**：stereo3d 项目矫正模块用 `initUndistortRectifyMap(..., CV_16SC2, ...)` 时，
误以为 map2 会是 `CV_32FC1`（小数部分），按这个假设写下游访存与类型判断。
实际跑起来后 `remap` 报类型相关错误/矫正输出异常。

**排查**：
1. 打断言/打印 `map1.type()`、`map2.type()`，实测 map2 是 **`CV_16UC1`**，不是 32FC1；
2. 翻 OpenCV 4.5.1 源码 `initUndistortRectifyMap` 实现：当 `m1type == CV_16SC2` 时，
   内部走定点化分支，**固定产出 `(CV_16SC2, CV_16UC1)` 组合**——不存在「16SC2 + 32FC1」；
3. 对照实验：`m1type = CV_32FC1` 时才得到 `(CV_32FC1, CV_32FC1)` 浮点对。

**根因**：`remap` 的双 map 有两种等价编码——
- **定点编码**：map1(CV_16SC2) 存整数坐标+索引，map2(CV_16UC1) 存插值小数（固定点 0~32），
  查表时纯整数运算，`CV_16SC2` 的 m1type 就是隐式选了这条路；
- **浮点编码**：map1(CV_32FC1/2) 直接存浮点坐标，map2 留空或同为浮点。
m1type 不是「map1 的格式偏好」，而是**整条查表方案的开关**，map2 类型被它连带锁死，
文档只在 remap 一侧写了合法组合，init 一侧没写——这是隐式耦合。

**解法**：直接接受 `(CV_16SC2, CV_16UC1)` 组合并按它写下游：

```cpp
cv::initUndistortRectifyMap(M1, D1, R, P1, size, CV_16SC2, map1, map2);
// map1.type() == CV_16SC2, map2.type() == CV_16UC1 —— 固定组合，别按 32F 假设
```

**证据（1280x720 双目矫正）**：定点方案两 map 共 6 字节/像素，
float 方案 8 字节/像素，**省 25% 访存**；remap 内部定点查表还少一次浮点乘加，
矫正段实测 ~5.4ms/帧 无回退。

**局限**：只验了 4.5.1 版本源码；插值精度差（定点 1/32 像素小数）对亚像素级
应用（如光流、精密测量）是否有感未测——双目视差 1px 粒度下无影响。

**面试讲法**：
> 「我用 OpenCV 做双目矫正时按文档直觉假设 map2 是浮点，跑起来才发现
> `initUndistortRectifyMap` 的 m1type 传 CV_16SC2 会把整条 remap 查表切到定点编码，
> map2 固定是 CV_16UC1 存 1/32 像素插值小数——这是文档没写的隐式耦合。
> 顺带因祸得福：6 字节/像素比全浮点省 25% 访存，矫正 5.4ms。」

---

## 九、MPP 硬件解码

### 17. MPP 解码 `decode_put_packet` 返回 -1012：系统库与内核驱动版本倒挂

**现象**：照官方 demo（mpi_dec_test.c advanced 模式）逐行抄的 MJPEG 解码器，
`mpp_init` 成功、`decode_put_packet` 返回 **-1012**，`mpi_decode_get_frame` 永远
NULL。换遍所有参数组合（split_parse、MPP_DEC_SET_CFG、预分配 frame buffer、
KEY_OUTPUT_FRAME meta）全部无效。

**排查**（分层排除）：
1. 先怀疑用法：读完 `/tmp/mppsrc/mpp-develop/test/mpi_dec_test.c` 全部关键路径，
   确认「split 参数在 init 前设 + advanced 模式 put_packet 前挂 KEY_OUTPUT_FRAME
   meta + 预分配 frame buffer group」三要素都齐了 —— 还是不行；
2. 读库源码 `mpp/mpp.c` 的 `mpp_put_packet`：-1012 = `MPP_ERR_BUFFER_FULL`，
   是 `mpp_poll` 输入端口返回负值被拒 —— 与上层参数无关；
3. `strings librockchip_mpp.so.0 | grep rk3588` 显示库认识 RK3588，
   `dmesg` 里 jpegd 节点 status=okay —— 排除「不支持该芯片/解码器」；
4. **决定性对照**：用 MPP 源码树重新编译出的 librockchip_mpp.a
   （静态链进自己的程序），**同一份代码不改一行，直接出帧**。

**根因**：系统自带的 `/usr/lib/aarch64-linux-gnu/librockchip_mpp.so.0`
（2021-05-20 编译）与板上 `mpp_service` 内核驱动的 ioctl 语义不匹配（版本倒挂）。
上层 API 怎么写都对不了——**这类「参数怎么调都返回同一个错误码」的问题，
优先怀疑层间版本，而不是继续翻参数**。

**解法**：`/tmp/mppsrc/mpp-develop` 编译出的静态库链接（输出帧 p50 **1.03ms**，
vs OpenCV imdecode 7.76ms，**7.6×**）。板上要长期用就 `make install` 或放
`third_party/mpp` 固化。

**证据**：bench_mpp N=200：硬件 put p50=1.015ms / total p50=1.027ms，200/200
有效帧；9 张真实图 PSNR 49.9~77.2dB。

**面试讲法**：
> 「MPP 解码器怎么写都返回 BUFFER_FULL，我按官方 demo 逐行对照确认用法没错后，
> 没有继续调参数，而是去读 mpp_put_packet 源码定位错误传播路径，再用
> 源码编译库做 A/B 对照——同一份代码换库直接通过，证明是系统库和内核驱动
> 版本倒挂。学会『参数调不动就去读错误码的传播链』。」

### 18. 「全链路 0 检出」逐层验证后，发现是测试图本身没有目标

**现象**：MPP 硬解→CSC→letterbox→NPU 全链路重写后，a.jpg 与 cam1_focused.jpg
都是 **0 检出**（conf=0.25）。而模型加载正常（64ch/80ch 输出都在）、
预处理各环节耗时正常。

**排查**（逐层验证，不跳步）：
1. **CSC 图像对不对**：MPP 输出 NV12→RGB888 dump 成 PPM，与 cv2.imdecode
   参考做逐通道线性回归——R 斜率 0.997、灰度相关性 **1.0000**，图像完美，
   排除花屏/通道序（corr(out_B, ref_R)=0.99 < corr(out_R, ref_R) 也证明 RGB 顺序对）；
2. **letterbox 对不对**：letterbox 后 buffer 拉回 PC 对比参考实现，灰度相关性 0.9995；
3. **喂入口径对不对**：与 eval_dump.cpp（当年 mAP 0.60 的验证工具）逐字段对照，
   UINT8/NHWC/RGB 完全一致；
4. **决定性对照**：用 eval_dump（conf=0.01）跑同一张 a.jpg——最高分只有
   **0.044**；再跑校准图 real_000 检出 person **0.82**、COCO 图 0.87。
   → 板端推理环境完全正常，**a.jpg 本身就没有可信目标**（它是 benchmark
   复制用的测试图）。另外三张"不同的图" md5 相同——测试素材管理混乱。

**根因**：测试图没有目标。0 检出是正确行为，不是 bug。

**解法**：全链路换真实目标图验证——real_000.jpg 检出
`person 0.813 → BEV dist 5.89m`，COCO 图 6 检出（0.871/0.803…），
与历史分数一致。**教训：验证检测链路必须用「已知含目标、且知道大概分数」
的锚点图（校准图/COCO 图），不能随手拿一张图当测试输入**。

**面试讲法**：
> 「全链路 0 检出时，我没有先怀疑模型，而是逐层验证：CSC 输出和 OpenCV
> 参考做线性回归（相关性 1.0）、letterbox buffer 拉回 PC 对比（0.9995）、
> 喂入口径逐字段对照——全部正确后，用 conf=0.01 的老工具跑同一张图，
> 发现最高分只有 0.044：是测试图本身没目标。再用已知分数的锚点图验证
> 全链路，person 0.813 与历史 0.8072 一致。『逐层排除 + 锚点对照』
> 比『到处改代码碰运气』快得多。」

---

### 19. RKLLM 端侧具身 Tool Calling 闭环：必须为严谨 JSON 数组 `[{...}]` 且 tool_response_str 不能为空

**现象**：在 RK3588 原生 C++ 运行 `librkllmrt.so` (v1.3.1) + Qwen3.5-2B 时，模型成功自主输出 `<tool_call><function=get_system_health></tool_call>`。但当 C++ 读取 `/sys/class/thermal/` 读出真实温度并以 `role="tool"` 喂回 `rkllm_run` 时，大模型不输出后续总结，运行时爆出错误：
`E rkllm: Please provide a valid JSON format string. example: [{"temperature": 26.1}], do not include extra newlines, trailing commas, or any characters outside the JSON array.`

**排查**：
1. **API 参数审查**：初次调用 `rkllm_set_function_tools` 时，若第 4 参数 `tool_response_str` 传空字符串 `""`，API 直接返回 `-1`，报错 `E rkllm: The tool_response_str cannot be empty`。必须传入明确的标记字符串（如 `"<tool_response>"`）；
2. **回填 Payload 格式对比**：抓取程序回传的字符串，发现原代码组装的是标准 JSON 对象 `{"soc_temp_c": 34.2, ...}`；
3. **对照实验**：将其严格用方括号包装成 **JSON 数组** `[{"soc_temp_c": 34.2, ...}]`，再次喂入 `rkllm_run`，报错立刻消失，大模型直接进入第二轮生成，准确提炼出 SOC 34.23°C、NPU 35.15°C 并用人话汇报。

**根因**：
`rkllm-runtime` 内部对 `role="tool"` 的输入做了模板语法解析，要求遵循 OpenAI Function Calling 规范的多结果数组格式。内部 JSON Parser 预期根节点为 Array，若为 Object 则解析器直接拒绝推进上下文。

**解法**：
1. `rkllm_set_function_tools(handle, sys_prompt, tools_json, "<tool_response>")` 第4参数传入有效非空字符串；
2. 工具函数返回值统一用 `[{ ... }]` 数组包裹；
3. Qwen3.5 采用原生 XML 标签输出 `<tool_call><function=name><parameter=key>val</parameter></function></tool_call>`，C++ 拦截器据此解析参数并派发执行。

**证据**：
- 板端实测闭环：自然语言询问「开发板当前实际温度」，大模型在 3核 NPU 离线环境下自主调起工具，读回 `[{"soc_temp_c": 34.23, "npu_temp_c": 35.153}]`，最终输出流式人话总结；
- 扩展执行器控制：自然语言「请把板载指示灯调成急闪报警模式」，模型自主解析 `mode="alarm"` 并写入 `/sys/class/leds/work/`，硬件指示灯即刻以 100ms 爆闪。

**面试讲法**：
> 「在 RK3588 上部署端侧 Agent 闭环时，我遇到模型输出 Tool Call 之后，C++ 回填硬件数据却被 runtime 拒绝的问题。排查发现 RKLLM 内部的 chat_template 解析器在处理 tool 角色时强制要求根节点是 JSON 数组 `[{...}]`，哪怕传合法的单个 JSON 对象也会被解析器拦截。同时搞清了端侧大模型从『只能纸上谈兵的 LLM』演进到『能感知底层硬件、控制外设执行器的具身 Agent』的核心纽带是运行时拦截与双向 Tool 回填。」

---

### 20. LED 写 `trigger=timer` 假成功：内核没编 CONFIG_LEDS_TRIGGER_TIMER，且写失败没人检查

**现象**：具身 Agent 的 `control_led(mode="alarm")` 一直「成功」——旧版代码往
`/sys/class/leds/work/trigger` 写 `timer` 后返回 `success:true`，但 LED 实际
**没有任何变化**（该闪不闪）。`heartbeat` 恰好是开机默认值，所以「恢复呼吸灯」
这类用例掩盖了问题。

**排查**（证据先行，不信返回值）：
1. 直接探针写 sysfs：C++ `ofstream` 写 `timer` 后检查 `fail()` —— **fail=1**；
   绕开 iostream 用裸 `write(2)` 拿 errno —— **errno=22 (EINVAL)**；
2. 追问「为什么 EINVAL」：查内核配置，板上 5.10.110 内核
   **没有编译 `CONFIG_LEDS_TRIGGER_TIMER`**（`/sys/class/leds/work/` 下根本
   不存在 `timer` 触发器，`cat trigger` 的可用列表里没有它）；
3. 对照实验：写 `heartbeat`（列表里存在的触发器）成功——证明不是权限/路径问题，
   是**这个触发器在内核里就不存在**。

**根因**：两层叠加。① 内核层：LED timer 触发器是编译期可裁剪模块（`CONFIG_LEDS_TRIGGER_TIMER`），
出货镜像没编它，`timer` 这个值对 sysfs 是非法输入，写入返回 EINVAL；
② 应用层：旧代码 `write_file_string()` **不检查写入返回值**，错误被静默吞掉，
上层拿到假 `success:true`。这是「假成功」类 bug 的标准形态：**底层失败 + 上层不验**。

**解法**：① 所有 sysfs 写入逐步检查返回值，失败报 `sysfs_write_failed+failed_node`；
写后**回读** `actual_mode`/`brightness` 与请求比对，不符报 `readback_mismatch`；
② alarm 改为**用户态爆闪**：fork 后立即 `execl("/bin/sh", "-c", "while true; echo 255 > brightness; sleep 0.1; echo 0; sleep 0.1; done")`，
pid 记录在 `/tmp/edgevision_led_alarm.pid`，切换其他模式前先 stop 旧进程——
不依赖内核触发器，任何内核配置下行为一致。

**面试讲法**：
> 「Agent 调 LED 报成功但灯不动。我没有加日志重试，而是直接探针写 sysfs：
> ofstream fail=1、裸 write errno=22，查内核配置发现 5.10.110 没编
> CONFIG_LEDS_TRIGGER_TIMER——`timer` 触发器根本不存在。真正的问题是两层：
> 内核裁剪掉了功能，应用层写失败又不检查，两层叠加出『假成功』。修法是
> 写入逐步校验+回读比对，alarm 改用户态 100ms 爆闪不依赖内核。」

---

### 21. fork 出的子进程「钉死」NPU：子进程继承 NPU 内存映射，父进程退出后 NPU 内存无法释放

**现象**：为做用户态 LED 爆闪加了 `fork()`，之后**重新启动 Agent 进程**时
`rkllm_init` 失败：`malloc npu memory 205799424B, errno 14 (ENOMEM)`——
明明系统还有 6GB 可用内存，却分不出 196MB 的 NPU 内存。

**排查**（相关性 → 对照实验）：
1. 时间相关性：报错只在「跑过带 fork 的旧版程序之后」出现；新开进程不 fork 直接
   init 一切正常——排除内存泄漏累积、模型文件损坏；
2. 检查子进程去向：旧版 fork 的爆闪子进程 `while true` 死循环**没有退出条件**，
   父进程退出后它变成孤儿进程继续跑，`ps` 可见；
3. 机理：Linux `fork()` **复制整个地址空间**（写时复制），包括 RKLLM runtime
   已经 `mmap` 的 NPU DMA 内存映射。子进程不 exec 就一直持有这份映射的引用，
   驱动侧认为 NPU 内存仍被占用，新进程 `rkllm_init` 申请同一块 196MB CMA
   内存自然 ENOMEM——** free 看似充足，实际被「看不见的子进程」钉死**；
4. 决定性对照：子进程 fork 后立即 `execl()` 重建地址空间（映射全部丢弃），
   同样场景再测，NPU 恢复正常。

**根因**：fork 的语义是「克隆」，不是「启动一个干净的新程序」。子进程继承父进程
包括设备内存映射在内的一切资源；持有 NPU 映射的进程（哪怕是孤儿）会阻止 NPU
内存释放。**要做「跑个外部命令」必须 fork+exec，不能只 fork**。

**解法**：爆闪子进程 fork 后立即 `execl("/bin/sh", "-c", ...)`；同时给爆闪循环
加退出条件（pid 文件 + 模式切换前 stop），SIGINT/SIGTERM 退出时恢复 heartbeat
并清理子进程。

**面试讲法**：
> 「加了 fork 做 LED 爆闪之后，下一次启动 LLM 就 ENOMEM——196MB 的 NPU
> CMA 内存分不出来，而 free 显示还有 6GB。排查发现 fork 的死循环子进程变成
> 孤儿还在跑，fork 是整地址空间克隆，它把父进程 mmap 的 NPU DMA 映射也继承
> 了，驱动认为内存仍被占用。fork 后立刻 execl 重建地址空间就好了。这个案例
> 讲透了 fork 语义：克隆≠启动新程序，设备映射也会被继承。」

---

### 22. 命令「卡死不返回」：后台子进程继承了 stdout 管道，上游永远等不到 EOF

**现象**：回归测试跑「开急闪报警」用例时，**命令卡死 270s 后被强杀**
（`rc=TIMEOUT`），但板上 `ps` 能看到 agent 早已跑完、LED 也确实在爆闪——
**功能是对的，进程就是不返回**。同一条命令在交互式 ssh 里手动跑却秒回。

**排查**（把「功能」和「IO 生命周期」分开看）：
1. 先确认功能：板上 `ps` 有爆闪进程、`/sys/class/leds/work/brightness` 在 255/0
   之间跳——**动作确实执行了**，排除「模型没调工具」；
2. 再盯「谁没退出」：`ps aux | grep 'while true'` 看到 `sh -c while true; ...`
   的父进程是 **PID 1**（已被 init 收养的孤儿）；agent 主进程**已经退出**；
3. 关键线索：**交互式 tty 正常、管道/重定向场景卡死** → 典型 fd 继承问题。
   回归脚本是 `subprocess` 抓 stdout，读端要等 **EOF**（所有写端关闭）才返回；
4. 机理：爆闪子进程 `fork()` 后没重定向 fd，继承了父进程的 stdout/stderr 管道写端。
   父进程退出后，**这个永不退出的 while 循环仍持有写端** → 读端永远拿不到 EOF →
   调用方/`ssh`/`bash -c` 全部一直等到超时被强杀。

**根因**：`fork()` 继承**文件描述符表**。任何长寿子进程（守护进程、后台循环）都必须
在 exec 前把 0/1/2 重定向到 `/dev/null`（或关掉），否则它会把父进程 stdout 的
生命周期无限拉长——这是 daemon 化的标准动作，漏了就会出现「功能正常但命令不返回」。

**解法**：子进程里 `open("/dev/null", O_RDWR)` 后 `dup2` 到 STDIN/STDOUT/STDERR，
再 `execl`。修复后同用例 **270s 超时 → 21s 正常返回**。

**面试讲法**：
> 「回归测试里『开爆闪』这条一直超时被杀，但灯确实在闪——功能对、进程不返回。
> 我按『先验功能、再查 IO 生命周期』分开看，发现爆闪子进程被 init 收养还在跑，
> 而交互式跑就正常、管道跑就卡死，这是 fd 继承的典型特征：fork 出的子进程继承了
> 父进程 stdout 管道写端，父进程退了它没退，读端永远等不到 EOF。修法就是标准
> daemon 化——exec 前把 0/1/2 重定向到 /dev/null。这条最能说明我理解『管道是
> 引用计数资源，不是父进程私有的』。」

---

### 23. 正在爆闪的报警灯被读成「熄灭」，Agent 据此编造了错误因果

**现象**：多步演示第一幕守护正在真实爆闪（`brightness` 实测占空比 **49.7%**），
第二幕 Agent 查灯状态却返回：

```json
{"led_name":"work","current_mode":"off","brightness":0,"description":"熄灭关闭"}
```

Agent 于是给出这段汇报：

> 「**LED灯始终熄灭（OFF）是异常的**……**核心原因**：板载 LED 指示灯（work）当前工作模式
> 为 "off"，**这导致硬件层面的过热保护逻辑被抑制**，无法向软件层面发送视觉故障报警」

——**灯其实一直在闪**。整条链路「工具调用正确、Observation 是合法 JSON、多步判定通过」，
演示脚本判 **PASS**，只有把 trace 逐条拉出来跟物理事实对齐才发现问题。

**排查**（先分层定位「模型幻觉 vs 感知层喂错数据」）：
1. 先看模型是否编造：Observation 里 C++ 工具**真的返回了** `off`，所以不是幻觉，
   是**忠实推理了错误输入** → 问题在感知层；
2. 对照物理事实：`echo heartbeat`/爆闪期间直接 `cat brightness` 连续采样得到
   `0 255 0 0 255 255 ...`，`cat trigger` 恒为 `[none]` —— 灯在闪，但 trigger 说"没有触发器"；
3. 读工具实现 `get_led_current_mode()`：**先读一次 trigger，见 `none` 就再读一次
   brightness，读到 0 直接判 `off`**；
4. 算概率：爆闪是 **100ms 亮 + 100ms 灭**的对称方波，单点采样落在灭相的概率约一半
   → 实测两次调用的 obs[1]、obs[16] **都是 `off`**，不是偶发。

**根因**：**周期性物理量不能用单点瞬时采样定性**。用户态爆闪不改 trigger（恒 `none`），
亮度在 0/255 之间周期翻转，而 `brightness=0` 这个编码**同时代表「关闭」和「爆闪的灭相」**
—— 两种语义撞在同一个值上，任何单点采样都会产生**混叠（aliasing）误判**。

**解法**（两层，主判据不看亮度）：
1. **判「谁在闪」而不是「这一瞬多亮」**：`led_user_blink_alive()` 读爆闪 pid 文件 →
   `kill(pid,0)` 确认存活 → 再读 `/proc/<pid>/cmdline` 确认含 `brightness` 关键字
   （排除僵尸进程和 pid 被内核复用两种情况）→ 命中则直接返回 `alarm_blink`，
   **与 trigger 和瞬时亮度都无关**；
2. **兜底多点采样**：12 次 × 15ms（覆盖 1.8 个爆闪周期），出现「既有 >0 又有 0」即判爆闪。

配套还修了两个连带问题：①陈旧 pid 文件（进程已死但文件残留）在假活判定时 `unlink` 自动清理；
②同一坑在**回归脚本收尾**也有一份——`restore_led_heartbeat()` 只 `echo heartbeat > trigger`
不清爆闪子进程，且判据 `"heartbeat" in cat_trigger` 对 `[none]` 也判真（trigger 候选列表里
**所有名字**都含 heartbeat 字样），必须匹配带方括号的生效标记 `[heartbeat]`。

**面试讲法**：
> 「演示脚本判 PASS，但我翻 trace 逐条对齐 Observation 和物理事实，发现 Agent 说『灯灭了
> 导致过热保护被抑制』——灯其实在爆闪。这不是模型幻觉，是工具把正在闪的灯读成了关闭：
> 爆闪是 100ms 方波，而 `brightness=0` 同时是『关闭』和『灭相』的编码，单点采样必然混叠。
> 我先分层确认是感知层还是模型层的问题，再把判据从『看瞬时亮度』改成『看爆闪进程是否存活』
> ——识别一个周期性状态，要看它的驱动源在不在，而不是赌一次采样的相位。」

**通用教训**：**Agent 给出离谱结论时，先对齐 Observation 与物理事实，别一上来就调 prompt。**
这次如果直接去改 prompt，会掩盖真正的感知层 bug——而模型「忠实推理错误输入」这个行为
本身是完全正确的。

**第二处（2026-10-28）：内核 `heartbeat` 触发器同样被单点采样误读，而且上次的判据在这里失效**

上一处的修法是「判驱动源在不在」（查爆闪 pid 文件）+ 「多点采样兜底」。
但 **heartbeat 是内核 LED trigger 在驱动，没有 pid 文件可查**，所以那套主判据在这里直接落空。
现象是 Agent 自相矛盾的一问一答：

```json
{"led_name":"work","current_mode":"heartbeat","brightness":0,"description":"正常呼吸闪烁"}
```
> 「模式：心跳 (heartbeat) / **亮度：0（关闭）** / 描述：正常呼吸闪烁 / 因此指示灯处于正常待机状态」

同一段回答里「亮度 0 关闭」和「正常呼吸闪烁」互相否定。**灯没灭，是采到了灭相。**

板上实测（`trigger=[heartbeat]`，40 次 × 30ms 连采）：**亮 9 次 / 共 40 次 = 占空比 22%**
（与历史记录的 heartbeat 基线 20.0%~25.3% 一致）→ **单点读到 0 的概率约 78%**，
比上一处爆闪对称方波的 50% 还要糟。原始采样串：
`0 0 0 255 255 0 255 0 0 0 0 0 0 0 0 255 0 0 255 0 ...`

**根因**：和上一处**同一个根因的另一半**——`brightness=0` 这个编码同时代表「关闭」和「周期波形的灭相」。
上次只修了「用户态爆闪」这一种驱动源，没修「内核触发器」这一种。
工具直接把裸 `brightness` 回填给模型，模型只能按最朴素的语义解释成「关闭」。

**解法**（改 `tool_get_led_status`）：

1. **按驱动方式分流**，而不是统一回填瞬时值：
   - **周期模式**（`heartbeat` / `alarm` / `alarm_blink`）→ 返回 `brightness_duty_pct`
     = 20 次 × 60ms（1.2s，覆盖 1.2 个 heartbeat 周期）内「亮」的采样点占比，
     并附 `brightness_note` 说明判读基准（心跳 20~25% / 爆闪约 50% / 常亮 100% / 关闭 0%）；
   - **静态模式**（`on` / `off`）→ 才返回瞬时 `brightness`（此时它确实是稳定值）。
2. **不依赖 pid 文件**：占空比是纯观测量，对「内核 trigger / 用户态爆闪 / 其它任何驱动源」都成立，
   这比「查谁在驱动」更有普适性。
3. **prompt 明确禁止**：「heartbeat 的瞬时读到 0 是采样假象，绝不等于关闭」，
   并要求「问警告灯/有没有告警时，`get_led_status` + `get_patrol_status` 一起调」。

**修复后实测**：

```
heartbeat 真值占空比 22%  →  Agent: "heartbeat 模式，亮度占用率约 25%，属于正常呼吸状态" ✅
爆闪    真值约 50%        →  Agent: "已触发温度告警，LED 进入报警闪烁模式" ✅
```

**方法论（比这一处修复更值钱）**：
- **修一个坑时要问「这个判据依赖的前提，在其它分支上还成立吗」**。
  上次的判据 `led_user_blink_alive()` 依赖「有 pid 文件」，这只对用户态爆闪成立；
  内核 trigger、硬件 PWM、外部驱动都没有 pid。**判据绑定了某一种实现，就会在换驱动方式时静默失效。**
- **优先选纯观测量，而不是溯源量**。占空比对任何驱动方式都成立；
  「谁在驱动」只对已知的几种驱动成立。观测优先于归因。
- **同一个语义值承载两种含义（`0` = 关闭 / 灭相）是设计层面的坑**，
  只要这个编码不变，任何「读一次就下结论」的消费方都会踩——修的时候要把消费方一并收口。

---

### 24. 项目跑了 20 多天，构建系统里从来没有过一个 `-O` 标志

**现象**：准备做 `CMAKE_BUILD_TYPE=Release` 优化（文档里写着「白捡 1.55×」），
上手先看板上现有的构建配置：

```bash
$ grep CMAKE_BUILD_TYPE /root/edgevision/deploy/cpp/build_native/CMakeCache.txt
CMAKE_BUILD_TYPE:STRING=                          ← 空

$ grep CXX_FLAGS .../build_native/CMakeFiles/edgevision_pro_rknn.dir/flags.make
CXX_FLAGS = -std=gnu++17                          ← 一个 -O 都没有，等效 -O0
```

**影响面**：项目此前记录的所有 CPU 侧耗时——MJPG 软解 7.76 ms、letterbox、DFL 解码 3.09 ms、
CSC 51~65 ms——**全是未优化代码跑出来的**。也就是说，这些数字不是"性能上限"，是"没开优化"。

**排查**（为什么一直没人发现）：
1. CMake 的 `CMAKE_BUILD_TYPE` **默认为空**，此时 CMake 不会追加任何优化标志，
   也不会报错或警告 —— 编译**成功**、程序**能跑**，只有性能悄悄变差；
2. 产物路径、日志、功能验证全部正常，**功能测试永远发现不了这类问题**；
3. 项目里 bench 工具是手工 `g++ -O2` 编的，跑出来的数字（如 DFL 解码 1.79 ms）看着正常，
   于是"整体应该没问题"的错觉就形成了——**两套构建口径混用，掩盖了正式构建的缺陷**。

**解法**：
1. CMakeLists 里 `if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)` 默认 `Release`；
2. **兜底** `if(NOT CMAKE_CXX_FLAGS_RELEASE)` 时设 `-O3 -DNDEBUG`（`CACHE STRING ... FORCE`），
   防止有人用外部 toolchain 覆盖掉；命令行仍可覆盖；
3. **自检而不是靠人记**：configure 结束时打印构建摘要，并在 flags 里匹配不到 `-O[0-9s]` 时
   `message(WARNING)`。这样"没开优化"会自己喊出来。

> 这里还踩了个 CMake 自己的坑：摘要第一版恒为空并误报"没有 -O" —— 因为
> `CMAKE_BUILD_TYPE` 的值是 **`Release`（大小写混合）**，而标志变量是全大写
> **`CMAKE_CXX_FLAGS_RELEASE`**。直接拼 `CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE}`
> 得到 `CMAKE_CXX_FLAGS_Release`，`DEFINED` 为假 → 读到空值。
> **CMake 变量名大小写敏感**，必须 `string(TOUPPER ...)` 后再拼。

**A/B 实测结果**（`taskset -c 6` 绑大核，6 轮交替，EVM3588-B）：

| 口径 | O0 | O3 | 变化 |
| :--- | ---: | ---: | :--- |
| 纯推理 `rknn_run`（对照组） | 14.79~14.88 ms | 14.90~14.95 ms | **无变化（证明测量没漂）** |
| DFL 解码 + NMS | 3.44~3.51 ms | 0.62~0.63 ms | **5.5×** |
| 全链路 | 18.91~19.06 ms | 16.21~16.26 ms | **−14.6%，52.6 → 61.7 FPS** |

**再用真实生产二进制验一遍**（同一张 `real_000.jpg` 同一模型，各 5 次，`taskset -c 6`）：

| 层 | O0 | O3 | 变化 | 意义 |
| :--- | ---: | ---: | ---: | :--- |
| MPP 硬解 | 1.250 ms | 1.322 ms | +5.8% | **对照组①**（硬件，不该变）✔ |
| **NV12→RGB888 CSC** | **52.016 ms** | **33.583 ms** | **1.549×** | 自己写的循环，优化直接命中 |
| letterbox（`cv::resize`） | 1.184 ms | 1.222 ms | +3.2% | **对照组②**（OpenCV 预编译库，动不了）✔ |
| NPU + DFL 解码 | 22.480 ms | 19.708 ms | −2.77 ms | 与 bench 的 −2.78 ms 吻合 |
| **全链路** | **76.928 ms** | **55.836 ms** | **−27.4%（1.378×）** | 13.0 → 17.9 FPS |

> **两个对照组同时成立**才是这条实测的价值：跑在硬件上（MPP）和跑在预编译库里（OpenCV）
> 的两层纹丝不动，唯一自有 CPU 热点 CSC 恰好提速 1.55× —— 排除测量漂移与负载波动的可能。
> 也修正了一句流传的表述：**"白捡 1.55×"是 CSC 单层的，全链路实际是 −27.4%**，别混着说。

**顺带一个更隐蔽的坑：不绑核时 `mean` 会骗你两倍**

| 组 | DFL mean | DFL p50 | DFL p95 |
| :--- | ---: | ---: | ---: |
| pinned O0 | 3.44~3.51 ms | 3.44~3.48 | 3.48~3.71 |
| pinned O3 | 0.62~0.63 ms | 0.62~0.63 | 0.63~0.64 |
| **unpinned O0** | **6.38~8.15 ms** | 3.44~3.47 | **15.78~17.20** |
| unpinned O3 | 2.71~2.93 ms | 2.71~2.89 | 2.73~3.16 |

unpinned 的 O0 组 **p50 与 pinned 完全一致（3.45 ms），但 mean 被拉到 6.4~8.2 ms、p95 冲到 17 ms**。
原因：O0 代码执行时间长，被调度器跨核/大小核迁移打断的次数显著更多；O3 短平快受影响小。
**mean 反映的是调度噪声，不是代码差异** —— 不绑核能把 5.5× 的真实差异压成 2×。

**面试讲法**：
> 「做编译优化时我先去看现有构建配置，发现 `CMAKE_BUILD_TYPE` 是空的、`flags.make` 里连一个
> `-O` 都没有——等于项目跑了至少 20 多天全是 -O0。CMake 在 build type 为空时**不报错不警告**，
> 编译成功程序能跑，功能测试永远发现不了。我加了默认 Release + flags 自检警告，
> 然后做 O0/O3 对照：**纯推理那层纹丝不动（NPU 时间和主机优化无关，正好当对照组证明测量可信）**，
> CPU 的 DFL 解码快了 5.5 倍、全链路省 14.6%。另外我特意做了绑核和不绑核两组，
> 不绑核时 O0 的 p50 一样但 mean 翻倍、p95 冲到 17 ms——**调度迁移的长尾会污染均值**，
> 这也说明为什么性能测试必须 `taskset`。」

**通用教训**：
- **"编译成功"不等于"编译正确"**。构建配置里的优化级别、架构标志属于**不会报错的静默失效项**，
  必须靠自检打印 + 警告兜住。
- **性能测量前先设一个"理论上不该变"的对照组**（这里是 NPU 纯推理），它稳了别的变化才能归因。
- 手工编的 bench 与 CMake target 口径不一致时，两边的数字**不能横向比较**，
  而且会互相掩盖问题。

---

### 25. MPP 硬件 JPEG 编码：走通了、数据对了，但比软件还慢

**现象**：路线图上写着「MPP 硬编 JPEG 省 7.2 ms」（依据是软编 `cv::imencode` 实测 7.1~7.2 ms）。
既然硬解能做到 7.6× 于软解，硬编理应也快。实测结果相反：

| 指标 | MPP 硬编 qf80 | OpenCV 软编 q80 | 硬/软 |
| :--- | ---: | ---: | ---: |
| 1280×720 mean（N=200，2 轮） | 7.24 / 7.20 ms | 6.65 / 6.70 ms | **1.07~1.09×（更慢）** |
| p95 | 9.62 / 10.82 ms | 6.72 / 6.71 ms | 1.43~1.61× |
| p99 | 11.09 / 11.46 ms | 6.74 / 6.72 ms | 1.65~1.71× |
| **CPU 占用** | **8.2~8.3%** | **99.7%** | **0.08×** |
| 4 路并行（NT=4） | 1.573 ms/帧 | **0.838 ms/帧** | **软编快 1.88×** |

分辨率从 64×64 扫到 960×540，**全部档位硬编更慢，没有一个赢的档位**。

**排查**（先证明"真的走了硬件"，再找慢在哪）：
1. **核对链接的库**：`ldd bench_jpeg_encoder` 确认链的是
   `/root/edgevision/third_party/mpp/lib/librockchip_mpp.so.1`（源码编译版），
   不是系统 `/usr/lib/aarch64-linux-gnu/librockchip_mpp.so.0`（2021-05-20 的旧库）；
2. **查中断计数**（这是"有没有真走硬件"的铁证）：
   ```
   跑 bench 前  fdba0000.jpege-core 42/44/46/48 中断 = 21485 / 2735 / 1566 / 820
   跑 bench 后                                      = 25038 / 3438 / 1953 / 987
   增量                                             = +3553 / +703 / +387 / +167
   其余 jpege / rkvenc / rkvdec 核全部 0 变化
   ```
   → 确实是硬件编码器在干活，**不是悄悄回退到软件路径**（一开始我误以为是软回退，是这个证据推翻的）；
3. **验证输出质量**：`file` 认证为合法 baseline JPEG（`FFD8 FFE0` … `FFD9`），
   9 张真实图 Y-PSNR 均值 **42.46 dB**、1280×720 **49.27 dB** —— 输出是对的；
4. **对分辨率做线性拟合**找慢在哪：
   ```
   硬编 t(ms) ≈ 0.35 + 7.0e-6 × 像素数
   软编 t(ms) ≈ 0    + 7.5e-6 × 像素数
   ```

**根因**：**每像素算力两者基本打平（硬编还略优），但硬编每帧要多付约 0.35 ms 的固定开销**
——`ioctl` 提交 + 等待中断往返。分辨率越小，这 0.35 ms 摊得越薄不利，所以 64×64 那档硬编输得最多；
1280×720 才把差距压到 1.07×。**4 路并行输得更惨**，因为软编能吃满 8 个 A76/A55 核，
硬编只有 4 个 jpege 硬件核。

**结论**：单路 30 fps 场景下一次编码 6.65 ms，只占一个核的 20%，**CPU 根本不是瓶颈**——
用 0.6 ms 的延迟损失去换 6 ms 的 CPU 空闲，是亏的。**从路线图划掉，CPU 侧优化改做 RGA CSC。**

**这条为什么值得记**：它不是"优化成功"的记录，是**用数据把写在路线图上的假设否掉**。
一个负结论 + 可复现的量化依据，比一个强行凑出来"快了 5%"的数字有价值得多。

**顺带踩的 6 个坑**（完整版在 `deploy/cpp/bench/mpp_jpeg_encoder_evidence.md`）：
1. **`encode_put_frame` 直接 Segfault**：`rc: Assertion cfg->fps_in_denom >= 1 failed at
   rc_frm_check_drop:142` —— FIXQP 模式下 fps 分母为 0 导致除零。设 `rc:fps_in_denorm=1` 解决；
2. **`MPP_ENC_SET_RC_CFG` / `SET_CODEC_CFG` 恒返回 -1**：runtime 报
   `deprecated ... use MPP_ENC_GET_CFG / MPP_ENC_SET_CFG instead` → 只能用短键名；
3. **PSNR 只有 35.4 dB、画面偏暗**：根因与第一直觉相反 —— `cv::cvtColor(BGR2YUV_I420)`
   输出的是 **limited-range**（16~235），MPP **原样**写进 JPEG（实测 `Y_jpeg = 1.0012·Y_in − 0.15`），
   而 JPEG 要求 full-range → 输入侧预展开 `(Y−16)×255/219`，PSNR 35.4 → 46.2 dB。
   **更坑的是 `prep:range` 和 `set_color_range` 12 组组合输出字节完全相同** —— 键存在但实现忽略，
   又是个"假成功"；
4. `prep:format=RGB888/BGR888` 想让 MPP 做 CSC → `imdecode` 返回空，死路；
5. 系统旧库 `librockchip_mpp.so.0`（2021-05-20）→ `-1012`（和 TROUBLESHOOTING 第 17 条同一个坑，
   只是这次出现在编码侧）；
6. 流水线深度 1→8 只从 3.383 降到 3.143 ms，单 context 没有并行余量。

**面试讲法**：
> 「我按路线图去做 MPP 硬编，代码写完了、正确性也过了（Y-PSNR 42~49 dB），
> 但一测发现比软编**慢 7%**。第一反应是"是不是回退到软件了"，我去看 `/proc/interrupts`
> 里 jpege-core 的中断数，跑一趟涨了三千多次——确实是硬件在干活。
> 然后我做分辨率扫描拟合延迟模型：硬编 = 0.35 ms 固定开销 + 7.0e-6×像素，
> 软编 = 0 + 7.5e-6×像素 —— **每像素算力打平，硬编白付一个 ioctl+中断往返**。
> 结论是这条路走不通，我从路线图划掉了，转去做 RGA CSC。
> **负结论也是结论**，而且它省掉的排期比我硬凑一个数字有价值。」

**通用教训**：
- **"硬件加速" ≠ "更快"**。硬件单元有固定的提交/中断开销，在小任务、低分辨率、
  或软件侧能多核并行时，硬件反而输。判断依据是**每个像素的固定成本 vs 边际成本**，
  不是"用了硬件就快"。
- **怀疑性能异常时，先证明"它真的走了你以为的那条路"**（`ldd` 查链的库 + `/proc/interrupts`
  查硬件中断增量），再谈优化。
- **一个"存在但被忽略"的配置键是最难查的**：`prep:range` 写进去不报错、返回值也正常、
  输出字节一模一样 —— 这类"假成功"只能靠**端到端效果指标**（PSNR）而不是调用来验证。

---

### 26. 验收脚本自己抛异常，差点让一个完全正常的服务端背锅

**现象**：给车载看板写了端到端验收脚本，第一次跑 **6 项只过 4 项**。
第 4 项「实时流 `GET /stream`」直接抛
`UnboundLocalError: cannot access local variable 'buffer' where it is not associated with a value`，
一帧都没解析出来，第 6 项「帧内容变化性」因为它拿不到帧而连带失败。
看报告的第一反应必然是：**服务端的 MJPEG 流有 bug。**

**排查（先隔离，再看代码）**：
1. **绕开脚本直接打服务端**：`timeout 6 curl -N http://127.0.0.1:8090/stream | grep -c -- '--frame'`
   → **163 帧 / 23579076 字节 ≈ 27 fps**，服务端完全正常。
   **这一步是整个排查的关键**——它把「服务端问题」还是「脚本问题」这个岔路口一次问死，
   而不是先去读 900 行脚本猜。
2. 缩小到抛异常的那个函数 `parse_multipart_stream`：`buffer` 在外层是 `bytearray(initial)`，
   内层闭包 `fill()` 里写的是 `buffer += chunk`。
3. 对照一个极小的复现脚本确认根因（不依赖项目代码）。

**根因（Python 静态作用域分析）**：
Python 的名字绑定规则是 **编译期决定、运行期使用**。编译器扫描整个 `fill()` 函数体，
一看到对 `buffer` 存在**赋值形态**（`+=` 也是赋值语句），就把 `buffer` 判定为**本函数的局部变量**
（`co_varnames`），此后读它时还没绑定 → `UnboundLocalError`。

关键点：`bytearray` 的 `+=` 在运行时其实是**就地 `__iadd__`、并不重新绑定对象**，
但**编译器不执行运行期语义**，它只做文本层面的赋值检测。所以「语义上没重新绑定」救不了它，
必须从**语法形态**上消除赋值。

**解法**：`fill()` 里改成 `buffer.extend(chunk)` —— 方法调用不是赋值语句，
`buffer` 恢复成自由变量（`co_freevars`），去向闭包单元格取值。

**为什么第一版自查没拦住（更值得记的部分）**：
第一版自测只跑了 `ast.parse(source)` 验证**语法**通过。
**`ast.parse` 对作用域错误完全无感** —— 这个 bug 的代码 100% 语法合法，
AST 里看不出任何异常，必须**真跑一遍调用链**才能暴露。
补上 mock 真跑后，**9 个夹具用例（6 个 PASS 分支 + 坏 JPEG / 静态图 / CL 写错 / 缺 CL / 低帧率 / 坏 stats / 阻塞式服务端 / 连不上）逐条命中预期失败原因**，才敢说脚本可信。

**探针自己也会说谎（同一批活里第二次踩到）**：
写前端的那一侧自写 DOM 垫片测「断流重连」，第一轮报告「重连没生效、`img.src` 没变」。
差一步就去改前端代码了 —— 实际是**垫片没代理 `img.src = ...` 这种属性赋值**，
垫片自己漏了，页面代码是对的。

**教训（三条都能直接进面试）**：
- **测试失败时，先证明「测试工具本身是对的」，再怀疑被测对象。**
  最便宜的做法是用一条**完全独立的路径**（curl / 浏览器 / 手写最小复现）打同一个目标做交叉验证。
  这次就是 `curl` 一次把方向掰正，省掉在 900 行脚本里瞎找的时间。
- **语法检查 ≠ 正确性检查。** `ast.parse` / `gcc -fsyntax-only` / `node --check` 只能证明「能被解析」，
  证明不了「跑起来是对的」。有调用链的东西，**必须 mock 真跑**。
- **阴性结果要归因到具体机制，不能停在「它坏了」。** 这次真正的收获不是那行 `extend`，
  而是搞清了「编译期名字绑定 vs 运行期就地修改」这层关系 —— 这是 Python 闭包的经典面试题，
  自己踩一次比背十遍记得牢。

**面试讲法**：
> 「我的验收脚本第一次只过了 4/6，报错在解析 MJPEG 流的地方，一帧都没收到。
> 我没有先去读脚本，而是先用 curl 直连服务端——6 秒 163 帧 27 fps，服务端完全正常。
> 方向一下就从『修服务端』变成『修我的测试工具』。
> 根因是 Python 的作用域规则：闭包里写了 `buffer += chunk`，
> 编译器看到赋值形态就把 `buffer` 判成本函数局部变量，读的时候还没绑定。
> 有意思的是 `bytearray` 的 `+=` 运行时是就地修改、根本不重新绑定对象，
> 但**编译器不做运行期语义分析**，它只看语法形态。改成 `buffer.extend(chunk)` 就好了。
> 我第一版自测只跑了 `ast.parse`——语法检查对这种错误完全无感，必须真跑调用链。
> **这件事让我记住：测试失败时先确认测试工具本身没坏。**」

---

### 27. `rkllm_init failed`（errno 14 / Bad address）：不是驱动老、不是 CMA 不够，是 NPU IOMMU 的 4 GiB 地址空间被其它跑 NPU 的进程吃满

**现象**：`./rkllm_embodied_agent`（Qwen3.5-2B W8A8，2.98 GB `.rkllm`）两天前还能正常加载推理，
突然加载失败，三行错误固定不变：

```
W rkllm: Warning: Your rknpu driver version is too low, please upgrade to 0.9.7
E RKNN: failed to allocate handle, ret: -1, errno: 14, errstr: Bad address
E RKNN: failed to malloc npu memory, size: 205799424, flags: 0x2
E rkllm: load model file error!
rkllm_init failed: -1
```

迷惑点：**同一个二进制、同一个模型文件、同一块板**，昨天不报错今天报错；
且报错的 `size: 205799424`（196.3 MiB）远小于可用内存（MemAvailable ≈ 6.9 GB），
Swap 为 0，所以第一反应必然是「内存不够吗？驱动太旧吗？」——**两个都是错的**。

**排查（四个假设逐一证伪，最后用算术钉死）**：

1. **假设 A：驱动 0.9.2 太旧（警告都提示升 0.9.7 了）** → **否**。
   dmesg 里 `used:` 值全部落在 **3873~4085 MiB**，而 `size` 只有 196.3 MiB；
   0.9.x 上跑通 2.98 GB 模型的案例大量存在，警告是**软警告不阻塞**
   （同款 issue #232 里 1.1.4+0.9.6 打同样警告仍 `init success`）。
   降级路线也不通：**Qwen3.5 支持是 rkllm v1.3.0 才加的**，旧 runtime 直接不认这个模型。

2. **假设 B：CMA 不足（CmaTotal 只 256 MB 且 CmaFree=0）** → **否**。
   决定性反证：A/B 里 **`CmaFree` 全程都是 0 kB 却仍能加载成功**；
   `/sys/kernel/debug/dma_buf/bufinfo` 只有 1 个 buffer（rockchipdrm/Xorg），
   说明 CMA 没被 dma-buf 占。且 `flags: 0x2` 按 `rknpu-ioctl.h` 枚举 =
   **CACHEABLE**（runtime 还会 OR 上 `IOMMU|ZEROING`）→ 走的是
   **dma-buf/系统内存 + IOMMU 映射，根本不占 CMA**。

3. **假设 C：某个常驻进程泄漏 NPU 内存** → **部分否，但方向对了**。
   板上当时有两个常驻 NPU 程序：`edgevision_dashboard --port 8090`、
   systemd 的 `w4_edgevision_gateway /dev/video81`。
   但**单独跑 dashboard 180 秒内反复加载全部成功**（RSS 稳定 171 MB，
   `fd` 里 `renderD129` 计数恒为 0）；**单独跑 gateway 8s/68s 也都成功**（RSS 131 MB）。
   → 不是「某个进程单独泄漏」，而是**加起来越界**。

4. **决定性实验：按配置数「最多能开几个 rkllm」**（`_tools/quantify_iommu_by_config.sh`）：

   | 配置 | 结果 | 失败那次的请求 size | 失败时驱动报的 `used` |
   |---|---|---|---|
   | 完全干净（无 gateway/无 dashboard） | **2 个 OK，第 3 个 FAIL** | 41 608 320 B（39.7 MiB） | **4075 MiB** |
   | 只开 gateway（systemd） | **1 个 OK，第 2 个 FAIL** | **205 799 424 B（196.3 MiB）** | — |
   | gateway + dashboard（= 用户当时的真实配置） | **1 个 OK，第 2 个 FAIL** | **205 799 424 B（196.3 MiB）** | — |

   **逐字复现了用户原始日志里的 `size: 205799424, flags: 0x2, errno: 14`**。

**根因**：**RK3588 NPU 的 IOMMU 只有 32 位（4 GiB = 4096 MiB）地址空间**，
`rknpu` 给每次 `rknn_create_mem` 在 IOVA 里要一段**连续**空间，
段不够 `dma_map_sg` 就返回 `EFAULT`（errno 14 = Bad address），
runtime 把这个 EFAULT 一路翻译成「failed to allocate handle」。
算术一眼看穿：**`used: 3915 MiB` + `196.3 MiB` = 4111 MiB > 4096 MiB**。

而一个 Qwen3.5-2B rkllm 实例常驻后 RSS ≈ **2.17 GB**，每个实例约占 **~1.95 GB IOVA**：
- 干净板：2 × 1.95 ≈ 3.9 GiB → **只能开 2 个，第 3 个必然失败**；
- 再叠加 gateway/dashboard 后 → **只能开 1 个**。

用户那条命令其实是「当前配置下的第 2 个 NPU 重负载程序」，所以报错。
**这解释了「昨天好好的今天坏了」——代码没变，机器上跑的东西变了。**

关键辨识点（一眼定性，不用猜）：
- errno **14 (EFAULT)** 是**地址映射失败**，不是 **12 (OOM)**、也不是 **23/24 (fd 耗尽)**；
- `dmesg` 里 `rknpu_gem_get_pages: dma map <bytes> fail` + 紧跟的 `used: N MiB`，
  **`used + 请求 size > 4096 MiB` 就是铁证**，且驱动每次失败前会 dump 全部 IOVA 区域列表。

**解法**：

1. **先体检**（`_tools/npu_doctor.sh`，板上 `/root/npu_doctor.sh`，10 秒出结论）：
   ① 驱动/频率/负载/内存 ② 谁在占 NPU ③ 残留 rkllm 实例 ④ systemd 自启 ⑤ 历史失败次数
   ⑥ 结论与处理顺序。

2. **立刻能用**（让出 NPU 地址空间，**不用重启板子**）：
   ```bash
   pkill -f rkllm_embodied_agent        # 清残留实例（每个 ~2 GB）
   systemctl stop edgevision.service    # 停网关
   pkill -f edgevision_dashboard        # 停看板
   cd /root/rkllm_deploy && ./rkllm_embodied_agent
   ```
   用完恢复：`systemctl start edgevision.service`。

3. **约束**（记住上限就不会再踩）：
   - **一块 RK3588 上，NPU 重负载程序总量 ≲ 4 GiB IOVA**；
   - `rkllm` 大模型实例：**干净板最多 2 个**，叠加 gateway/dashboard 后**最多 1 个**；
   - 想长期并存 → 用 systemd `Conflicts=`/`After=` 做**互斥**，别靠人记得关。

4. **收不了就冷重启**：`reboot`。IOMMU 地址空间在重启后彻底复位
   （`kill -9` 实测也能正常释放，不必重启；重启是最后手段）。

**证据**：
- 板上 `_tools/quantify_iommu_by_config.sh` 三配置实测（见上表），
  逐字复现 `205799424 / flags: 0x2 / errno: 14`；
- dmesg 全部 11 次失败记录的 `used` 值与时间戳，`used + size` 恒 > 4096 MiB；
- 每个 rkllm 实例 RSS 2273756~2274104 kB；
- `rknpu-ioctl.h` 枚举解出 `flags: 0x2 = CACHEABLE`，故**不占 CMA**（推翻假设 B 的理论依据）；
- 同款社区复现 [issue #335](https://github.com/airockchip/rknn-llm/issues/335)
  （qwen2.5-vl-3b + errno 14 + 3085697024 字节），提问者最终结论也是「重启开发板就行了」——
  他碰到的是同一个上限，只是没查出来是哪一层。

**方法论收获（比结论更值钱）**：
- **「昨天好好的今天坏了」≠ 代码回归**。先问「机器上还有什么在跑」，
  而不是先 `git diff` / 先怀疑驱动版本。**同一份二进制换环境结果不同 = 资源竞争，不是代码 bug。**
- **`size` 远小于可用内存却能 OOM，说明限制不在你以为的那一层**。
  `size: 196 MB` + `MemAvailable: 6.9 GB` 并存 → 立刻排除「内存不够」，
  转去查「是不是有别的**地址空间**（不是物理内存）不够」。
- **让驱动自己报数**：`used + size > 上限` 这种判定，比自己加日志猜准确得多。
  内核在失败点附近打印的上下文（`used:` / 区域列表）就是免费的可观测量。
- **别被 warning 带偏**：`driver version is too low` 是**软警告**，
  它和真正的失败点（`dma map fail`）在日志里相邻出现，极易被误判成因果。

**面试讲法**：
> 「端侧大模型突然加载失败，报 `errno 14 Bad address`，同时驱动提示『版本太旧请升 0.9.7』,
> 几乎所有人第一反应都是去升驱动。但算一下就不对：失败请求只有 196 MB，
> 机器可用内存 6.9 GB——真 OOM 应该是 errno 12，不是 14。
> 我去读 dmesg，发现驱动每次失败前都会打印 `used: 3915 MiB` 和完整的 IOVA 区域列表，
> 3915 + 196 = 4111 > 4096。RK3588 的 NPU 走 IOMMU，**IOMMU 地址空间是 32 位的**,
> 只有 4 GiB，跟物理内存是两回事。每个 2B 模型实例常驻要吃掉约 2 GB IOVA，
> 干净板最多开 2 个，再叠加视觉网关就只能开 1 个。
> 我做了个三配置对照实验：干净 / 只开网关 / 网关+看板，分别测『最多能起几个 rkllm』,
> 干净是 2 个、另两个都只有 1 个，而且**逐字复现了用户原始日志里的那个 205799424 字节**。
> 所以根因是地址空间耗尽，不是驱动版本——升驱动要重编内核刷 boot.img，
> 做了也解决不了这个问题。解法就是跑之前先让出 NPU，或者用 systemd 做互斥。
> 这件事让我记住两点：一是**『昨天好好的今天坏了』要先看环境变了什么，不是先怀疑代码**；
> 二是**日志里相邻出现的 warning 和 error 不一定有因果关系**,
> 差 6 倍的性能问题和这次的驱动警告，都是被相邻位置带偏的陷阱。」

---

### 28. Agent 同时说出「20°C 阈值」和「75°C 才报警」：阈值是进程内状态，没暴露给模型，模型就只能编

**现象**：问端侧 Agent「什么时候报警闪烁」，它这样答：

> - 巡检守护进程逻辑：设定了 `20°C` 的阈值作为演示测试
> - 当前温度 44.4°C → 正常工作范围
> **只有在温度超过 75°C 时才会真正报"过热"并触发 LED 闪烁报警**。当前温度远低于该阈值，LED 灯保持常亮状态。

两个阈值并排出现、互相否定，且最后给的结论是「不会闪」。

**迷惑点**：这句「LED 保持常亮」**物理上是对的**——但对的理由是错的。
上板实测：`trigger=[none]`、`brightness=255`、爆闪进程 0 个、`edgevision_patrol_daemon` 没在跑。
也就是说灯之所以常亮，是因为**巡检守护早就死了、退出时把灯留在半个僵尸态**，
不是因为"温度低所以不报警"。**结论对、过程错**，比结论错更危险：没人会去复查它。

**排查（三步，每步都落到物理事实）**：

1. **拉齐物理事实**，而不是先读代码。板上实测：

   | 项 | 值 |
   |---|---|
   | SOC 温度 | 44.38°C |
   | `trigger` 生效位 | `[none]`（不是 heartbeat） |
   | `brightness` | 255 |
   | 爆闪子进程 | **0 个** |
   | 巡检守护进程 | **不存在**，也没有对应 systemd 单元 |
   | 巡检日志最后一条 | 三天前 |

2. **把两个数字追到源头**。它们根本不是同一件事：

   | 数字 | 来源 | 作用域 |
   |---|---|---|
   | **20** | 守护命令行 `--temp-enter 20`（验收用低阈值） | 业务告警阈值 |
   | **75** | `rkllm_embodied_agent.cpp:135` 里写死的 `soc_temp > 75.0 ? "high_temperature_warning"` + sys_prompt 里那句「75°C 才算过热」 | 物理过热常识 |

   代码里 `g_cfg_temp_enter` 的**默认值恰好也是 75.0**——所以「75 才是默认阈值」这句话本身没错，
   错的是**它被当成了当前生效值**。守护这一轮是带 `--temp-enter 20` 起的。

3. **做对照实验**（同一问题，只改守护在不在跑）：

   | 场景 | 守护真实状态 | 灯的真实状态 | 修复前 Agent 回答 |
   |---|---|---|---|
   | 守护在跑（20°C） | THERMAL_ALERT | `255 0 255 0 255 0 255 0`（真爆闪） | 「44°C 远低于 75°C，不会报警」❌ |
   | 守护已停 | STOPPED | 残留 `none`+255（僵尸态） | 「LED 保持常亮」✅（但理由是错的） |

**根因（三层，一层比一层深）**：

1. **可发现性缺失（最根本）**：告警阈值是**进程内的运行时状态**（命令行参数），
   除了守护自己没有任何地方能读到。Agent 手上只有「历史事件日志」和「写死的默认值」两个来源，
   而它无法判断历史事件里那句「超过 20.00C 阈值」是**正在生效**还是**三天前的旧记录**。
   → 模型不是幻觉，是**没有可依据的观测**（grounding 失败），于是用 prompt 里的先验知识填了空。

2. **工具返回了「结论」而不是「事实」**：旧版 `get_system_health` 直接吐
   `status: "normal_optimal"`（由 `soc_temp > 75.0` 判定）。模型看到现成结论就直接抄，
   跳过了「这个 status 依据的是哪套阈值」这一步推理。
   **把结论塞进 Observation，等于替模型做了它该做的判断，而且做错了。**

3. **一条旧经验反噬**：之前踩过「模型把人为压低的演示阈值当成真实红线」，
   当时的修法是**把 75°C 领域常识写进 sys_prompt**（见速查表旧条目）。
   结果这次它拿这条常识去**否定了守护进程的真实行为**——同一个坑的镜像面。

**解法**：

1. **守护写运行态快照** `/root/patrol/patrol_state.json`（每周期刷新，`.tmp` + `rename` 原子替换）：
   `pid / running / interval_s / temp_enter_c / temp_exit_c / person_score_th / state / led / updated_at / stale_after_s`。
   这把「阈值」从进程内状态变成**可观测的外化状态**。
2. **新增工具 `get_patrol_status`**，与 `get_patrol_events` 明确分工：
   *events = 过去发生过什么（可研判历史），status = 此刻谁在控灯、阈值多少、还活着吗（决定会不会报警）*。
   **失联判定三条件与**：`running==true` **且** `now-updated_at <= stale_after_s` **且** `kill(pid,0)==0`。
   只看时间戳会漏掉「进程刚死但快照还没过期」的两三个周期；只看 pid 会漏掉 pid 被内核复用。
3. **`get_system_health` 删掉 `status` 结论字段**，只给原始温度 + 一句「是否告警取决于守护当前阈值，请查 get_patrol_status」。
4. **`get_patrol_events` 修复 DAEMON_START 行被丢字段**：该行的阈值只在 `detail` 对象里、
   没有 `summary` 键，旧代码 `if (detail.front()=='{') summary="";` 把它整条拍平丢掉了——
   历史日志里唯一能看出「这轮按什么阈值启动」的线索就此消失。现改为重建 summary：
   `巡检守护启动: 温度告警阈值 20.00C(恢复 18.00C), 采样间隔 2s`。
5. **守护启动时自检残留死态**：`trigger==none` **且** 无爆闪进程 → 判为上次异常退出的残留，
   主动恢复 heartbeat。注意**必须先查爆闪进程是否活着**：用户态爆闪时 `trigger` 也是 `none`
   （本板没编 `CONFIG_LEDS_TRIGGER_TIMER`），漏这一步会把正在报警的灯强行改成 heartbeat，
   等于亲手按掉警报。
6. **纳入 systemd**（`edgevision-patrol.service`，`Restart=always` + `ExecStopPost` 恢复 heartbeat
   + `SuccessExitStatus=0 3` 让单实例锁撞码不算失败），阈值走 `/etc/default/edgevision-patrol`，
   验收时改文件即可，不在 unit 里写死。
7. **prompt 改写**：删掉「超过 75°C 才算过热告警」这句诱导，改为
   **「告警阈值 ≠ 物理过热阈值」**：是否报警唯一依据是 `get_patrol_status` 的
   `active_thresholds.soc_temp_enter_c`；若阈值偏低，要**把两件事分开讲**——
   「设备物理温度正常」+「但按守护当前阈值已满足报警条件」，不许互相否定。

**修复后实测（同一块板、同一问题）**：

```
用例 A（守护在跑，44.38°C > 20°C 阈值）
  快照: {"running":true,"temp_enter_c":20.00,"state":"THERMAL_ALERT","led":"alarm",...}
  LED 采样: 255 0 255 0 255 0 255 0
  首轮工具: get_patrol_status  ← 主动查运行态, 不再是猜
  回答: 「是的，现在会触发 LED 闪烁。虽然 49°C 在物理安全范围内，
        但守护阈值极低（20°C），当前温度已远超进入告警条件。」✅

用例 B（守护已停）
  快照: {"running":false,"reason":"daemon_stopped","state":"STOPPED","led":"heartbeat"}
  回答: 「现在不会自动报警。巡检守护进程未运行（daemon_stopped）」✅
```

**证据**：
- 快照/日志/LED 采样原始输出见 `lab/docs/patrol_led_evidence.md` 与本轮 `verify_state.sh`；
- 残留自检实测触发：`检测到上次异常退出的残留态(trigger=none 且无爆闪进程), 已恢复 heartbeat [ok]`；
- systemd 单元 start→active、stop→`inactive` 且 trigger 回到 `[heartbeat]`、快照写 `STOPPED`；
- 修复前后对照：同一问题，修复前言 75°C 判"不报警"，修复后先查运行态、答"正在报警"。

**方法论收获**：
- **Agent 答错，先查「它有没有可能知道」**，别急着调 prompt。模型不会说"我不知道"，
  它会拿 prompt 里的常识**编一个确定的答案**——这比"报错"危险得多。
- **工具返回结论字段（`status: normal_optimal`）是个反模式**：结论会过期、会基于错的阈值、
  会让模型跳过推理。**给事实，不给判断**。
- **只看单点快照会骗人**：`trigger=[none]` + `brightness=255` 既可能是"正常常亮"，
  也可能是"报警中撞上灭相"，还可能是"守护崩溃后的僵尸态"。三者必须靠**进程存活 + 时间戳新鲜度**区分。
- **周期刷新的状态文件 + 时间戳**，是让「进程还活着吗」变成可判定事实的最便宜手段；
  比让模型去读 `ps` 输出可靠得多（模型解析 `ps` 文本极易出错）。

**面试讲法**：
> 「端侧 Agent 被问『什么时候会报警闪烁』，它答『只有 75°C 以上才会』。
> 但上板一查，守护进程是用 `--temp-enter 20` 跑的，44°C 早该报警了。
> 表面看是模型胡说，实际上是**我们的可观测性设计有问题**：阈值是守护进程的命令行参数，
> 除了它自己没人知道，Agent 只能从『历史日志』和『写死的默认值』里猜，
> 而模型判断不了一条历史日志是正在生效还是三天前的。
> 更糟的是 `get_system_health` 直接返回 `status: normal_optimal` 这种**结论字段**，
> 把基于错阈值的判断喂给模型，它连推理都省了，直接抄。
> 我做了三步修：一，让守护每周期把运行态（阈值/状态/pid/时间戳）原子写到快照文件，
> Agent 新增 `get_patrol_status` 去读，失联判定要同时满足『文件说在跑 + 时间戳没过期 + pid 还活着』
> 三个条件——只看时间戳会漏『进程刚死还没过期』的窗口，只看 pid 会漏 pid 复用；
> 二，把 `get_system_health` 里的 status 结论删掉，只给原始温度，**给事实不给判断**；
> 三，prompt 里改成『告警阈值 ≠ 物理过热阈值，要分开讲』。
> 顺手还修了个 bug：爆闪灯的 `trigger` 也是 `none`，所以残留自检必须先查爆闪进程在不在，
> 否则会把正在报警的灯改成 heartbeat，等于亲手按掉警报。
> 这件事让我改了个习惯：**Agent 答错时先问『它有没有可能知道正确答案』**，
> 模型不会说不知道，它会拿先验知识编一个很确定的答案——那比报错难查得多。」

---

## 十、语音链路与端侧 Agent 编排

> 这一节来自「给端侧 Agent 加语音输入输出」的过程：ASR → LLM → TTS 三段拼起来，
> 每一段都暴露了一个「不这么干就慢/就崩」的机制性问题。

### 29. 首轮推理要 28 秒：system prompt 的 prefill 成本，原来付在了用户身上

**现象**：Agent 启动后第一次提问，不管问什么都得等 28 秒才出第一个字；之后每轮只要 9~10 秒。

**排查**：
- 打时间戳逐行看：模型加载 3 秒完成，首个输出 token 在 31 秒才出现；
- 用长度差异极大的两个问题对比（"你好" vs 一长句）→ **耗时完全相同**，说明成本与输入无关，在固定部分。

**根因**：system prompt 2000 多字（约 2000+ token）。首轮推理必须把它**全部编码进 KV cache**（prefill）才能开始生成，
2B 模型在 3 核 NPU 上这个 prefill 要 24~28 秒。这笔钱躲不掉，但**什么时候付是可以选的**——
原来是在用户第一次提问时付。

**解法**：启动阶段预热 + 只清对话、保留 system prompt 的 KV

```c
// 1) 启动后立刻跑一次空推理，触发 system prompt 的 prefill
RKLLMInput wi{}; wi.role = "user"; wi.input_type = RKLLM_INPUT_PROMPT; wi.prompt_input = "hi";
rkllm_run(handle, &wi, &infer, nullptr);

// 2) keep_system_prompt = 1 —— 只丢掉预热这轮说的话，system prompt 的 KV 必须留下
rkllm_clear_kv_cache(handle, 1, nullptr, nullptr);
```

配套两点：预热时用全局 `g_quiet` 标志抑制 callback 输出（否则屏幕会闪一堆模型回答）；
`/clear` 指令本来就用 `keep_system_prompt=1`，所以清空上下文后也不会重新 prefill。

**量化**：

| 场景 | 改前 | 改后 |
|---|---|---|
| 启动耗时 | 3 s | 27 s（含 24 s 预热） |
| **第一次提问** | **28 s** | **1 s** |
| 后续每轮 | 9~10 s | 9~10 s |

**方法论收获**：把**一次性的固定成本**识别出来、挪到用户无感的阶段（启动/初始化），
是优化交互延迟性价比最高的一类手段。前提是能证明它确实与输入无关——用两个长度悬殊的输入对比耗时即可。

---

### 30. 每段播报都要等 3.75 秒：不是磁盘慢，是 ONNX session 被反复构建

**现象**：TTS 日志早就打出来了，但耳机里十几秒没声音。按句分段后，**每一段**都有约 3.7 秒的固定延迟。

**排查**（两步证伪）：
1. **是不是与文本长度有关？** 用 2~80 字共 9 组文本测合成耗时做线性拟合：截距 **3.70~3.88 s**、斜率 0.118 s/字。
   截距与长度无关 → 是固定开销。
2. **是不是磁盘 I/O？** 用 `posix_fadvise(POSIX_FADV_DONTNEED)` 把 116 MB 模型驱逐出 page cache 再跑：
   冷热只差 **0.02 s**（user 时间 5.7 s，sys 仅 0.3 s）→ **纯 CPU**。

**根因**：每次合成都 `fork/exec` 一个新的 `sherpa-onnx-offline-tts` 进程，
ONNX Runtime session 从头构建（图优化 + 权重预处理 + jieba/lexicon 2.4 MB/FST 解析）。

**解法**：改成常驻服务。官方 CLI 没有 server 模式，static 包也没有库——
但 **shared 包里带了 `libsherpa-onnx-c-api.so`**（22 MB 那个包），
用它写一个 Unix socket 守护进程，模型只加载一次，后续每次只需传文本、收 PCM。

**量化**：

| 项 | 改前 | 改后 |
|---|---|---|
| 15 字单句合成 | 6.0 s | 2.2 s |
| 80 字整段 | 13.7 s | 9.9 s |
| 三段播报·首段出声 | 5 s | **1 s** |
| 三段播报·总时长 | 40 s | 28 s |

代价：常驻内存峰值 482 MB，空闲 CPU 0%。

**方法论收获**：怀疑"是不是 I/O"时，**主动驱逐 page cache 再测一次**——
这是把"磁盘"和"CPU"分开的最低成本实验，比看 `iostat` 更直接。

---

### 31. 静态链接的子进程，`stdbuf` 完全无效——父进程读到死都是空

**现象**：用 pipe 抓 `sherpa-onnx` 子进程的 stdout，父进程一个字节都读不到；子进程自身运行完全正常（写文件能看到输出）。

**排查**：
- 先试 `stdbuf -oL` 包一层 → 依然读不到；
- `ldd` 一看：该二进制是**全静态链接**。`stdbuf` 靠 `LD_PRELOAD` 注入，**对静态程序不起作用**。

**根因**：stdout 接 pipe 时是全缓冲（4 KB），不攒满不 flush。

**解法**：给子进程开**伪终端（PTY）**。终端设备天生行缓冲，这是唯一不依赖子进程配合的办法。

```c
openpty(&master, &slave, nullptr, nullptr, nullptr);
// 子进程：
setsid();
ioctl(slave, TIOCSCTTY, 0);            // 显式设为控制终端
dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
tcsetpgrp(STDIN_FILENO, getpid());      // ← 少这一行就踩下面那个坑
// 父进程读 master，注意要 O_NONBLOCK
```

**连带坑**：`setsid()` 之后子进程**不在终端的前台进程组**，往 stdout 一写就收到 **SIGTTOU 被挂起**。
表现是"子进程起来了、一行输出都没有、然后进程消失"——很像崩溃，其实是被信号停掉了。
`ioctl(TIOCSCTTY)` 和 `tcsetpgrp()` 两步必须都做。

---

### 32. 孤儿进程霸占麦克风，新实例全部 `exit 255` 无限重启

**现象**：语音网关启动后 ASR 子进程每 3 秒崩一次（exit 255）反复重启；
`fuser /dev/snd/pcmC0D0c` 显示麦克风被一个**早已没有父进程**的 pid 占着。

**排查**：
- 用 `pgrep -f "vad-alsa-offline-asr$"` 找不到它 → **行尾锚定写错了**；
- 实际 cmdline 是 `...vad-alsa-offline-asr --silero-vad-model=... hw:0,0`，
  **末尾是 `hw:0,0` 而不是程序名**，`$` 永远匹配不上。

**根因**：两层叠加。
1. 清理逻辑按名字匹配 + 错误的行尾锚定 → 漏杀；
2. 主程序 `cleanup()` 只发 SIGTERM，而 ASR 在半双工时被 `SIGSTOP` 冻着，收不到信号 → 变孤儿。

**解法**（三条一起上）：
- 停止逻辑改成**按资源兜底**：`fuser /dev/snd/pcmC0D0c` 查出占用者直接 `kill -9`，比按名字匹配可靠；
- `cleanup()` 里 SIGTERM 等 2 秒不退就 `SIGKILL`（发之前先 `SIGCONT` 解冻）；
- 主循环加 `waitpid(pid, WNOHANG)` 探活，子进程死了自动重启并打日志——
  之前没有探活，子进程静默死亡后父进程空转了 100 分钟毫无察觉。

**方法论收获**：**「进程退了」和「资源释放了」是两件事**。
清理逻辑要按**资源占用**兜底（谁占着设备就杀谁），不能只按进程名匹配。

---

### 33. 官方 KWS 唤醒词模型一个都跑不起来：runtime 要的元数据，所有模型都没导出

**现象**：唤醒词工具启动即 `exit 255`，报
`'attention_dims' does not exist in the metadata`。

**排查**：先怀疑模型旧，换最新版（2025-12 的中英双语 3M 模型）→ **同样的错**。
再做版本矩阵：

| runtime | GLIBC 要求 | 结果 |
|---|---|---|
| v1.12.12 | 2.31 ✓ | ❌ 要求 encoder 带 `attention_dims` 元数据，官方所有 KWS 模型（含 2025-12）都没导出 |
| **v1.11.0** | 2.31 ✓ | ✅ **可用** |
| v1.10.30 | 2.33 ❌ | 二进制直接跑不起来（板子 2.31） |

**根因**：runtime 演进时新增了元数据校验，但 KWS 模型没有按新规范重新导出。
**是版本错配，不是参数问题**——换模型、调阈值、读源码都没用。

**教训**：碰到"官方模型 + 官方工具"组合失败时，**先做版本矩阵，别急着调参数**。
用 HTTP HEAD 批量探测 release 资产、用 `--help` 逐个试能否跑起来，
比读源码快一个数量级。另外注意 glibc 版本墙——同一个项目的不同版本可能编译环境不同，
`GLIBC_2.33 not found` 这种错跟功能无关，纯是构建环境问题。

**后续决策**：KWS 和 ASR 都要独占麦克风，切换一次要重载 ASR 模型（4 秒），
"喊完唤醒词还得再等 4 秒才能说话"，体验比不做唤醒还差。
最终放弃 KWS，改成 **ASR 软唤醒**：常驻的 Paraformer 一直识别，
文本里出现唤醒词（含同音字容错：小瑞/小锐/晓瑞/小睿/小蕊）就进入 10 秒激活态。
零切换开销，且能认任意词，不受 3.3M 小模型泛化能力的限制。

**软唤醒的代价要讲清楚**：ASR 常驻（RTF 0.023，CPU 约 3%），
而且"没唤醒时它仍然在听"——如果隐私是硬需求，那才值得上 KWS。

---

### 34. Agent 的回答被自己过滤光了：长行跨 read 截断 + 只认一个段落标记

**现象**：语音链路里 Agent 明明回答了，但送进 TTS 的文本是空的，只能降级成"我听到你说…"。

**排查**（靠打点日志，不靠猜）：
- 加统计输出：`读到 5204 字节 / absorb 430 次 / section=451`——数据读到了，但内容不对；
- 把 absorb 处理的每一行落盘，发现收进去的是**工具返回的 JSON 片段**，而不是回答。

**根因**：两个独立问题叠加。
1. **长行跨 read 截断**：管道每次只读 512 字节，`get_patrol_events` 返回的几千字节 JSON 被切成多段，
   第一段含 `[硬件感知` 被正确跳过，**后续片段开头没有这个前缀**，于是绕过所有过滤规则混进了正文。
   而当时的代码在 `absorb()` 之后直接 `buf.clear()`，把未完成的半行丢掉了。
2. **只认一个段落标记**：代码只收集「终局汇报」之后的内容，
   但 Agent 有两条输出路径——需要工具时打印「终局汇报」，**闲聊类直接答完不打印**，于是整段回答被丢弃。

**解法**：
- `absorb()` 改成"处理完整行、把半行留在 buffer 里继续拼"，不再 `clear()`；
- 维护 `sec_think` 和 `sec_final` 两段内容，**优先用终局汇报段，为空才用思考段**
  （路径 A 的思考段混着工具观测垃圾，路径 B 没有终局汇报，用思考段正好）。

**方法论收获**：流式解析别人的输出，**"按行切分"和"按块读取"是两个粒度，必须显式对齐**。
只处理完整行、保留半行，是所有增量解析器的基本功。

---
### 35. IOVA 泄漏不是宿命：装上 SIGTERM handler，kill 之后就不必再重启板子

**现象**：`systemctl stop` 或 `kill` 之后，再启动 agent 就 `errno 14`（`failed to malloc npu memory`），
只能重启板子复位。今晚为此重启了三次。

**先纠正一个误判**：`dmesg | grep "used:"` 打出来的那一行，
**是"分配失败那一刻"的快照，不是当前占用**——它只在 `dma map fail` 时才打印。
拿它当"现在用了多少"会得出错误结论：今晚据此误判过一次，实际只有 1.9 GB 残留，
被读成了 3.9 GB 全满，差点又去重启。

**根因**：`rkllm` 释放那 2 GB NPU 映射靠的是 `rkllm_destroy(handle)`。
进程如果没装信号处理器，收到 SIGTERM 会被内核**直接终止**，
`main` 末尾那行 `rkllm_destroy` 永远执行不到 → 映射留在驱动里。

`voice_start.sh --stop` 的流程是 SIGTERM → 等 3 秒 → SIGKILL。
既然第一步就已经死了，等于**每次都走强杀路径**，必然泄漏。

**解法**：

```c
static volatile sig_atomic_t g_stop = 0;
static void on_term(int) { g_stop = 1; }     // handler 里只置标志，不做任何事（信号安全）

// main 里，进主循环之前：
struct sigaction sa{};
sa.sa_handler = on_term;
sa.sa_flags = 0;        // ← 关键：不设 SA_RESTART
sigaction(SIGTERM, &sa, nullptr);
sigaction(SIGINT,  &sa, nullptr);

while (!g_stop) { ... }                       // 主循环条件
rkllm_destroy(handle);                        // 现在能正常走到了
```

**`sa_flags = 0` 是必需的**：glibc 的 `signal()` 默认带 `SA_RESTART`，
信号来了阻塞中的 `getline` 会被自动重启，表现为"杀了但没死"，还得敲回车才退出。

**验证**：连续 `起 → SIGTERM → 立刻再起` 两轮，第二次正常 `已就绪`，没有 `errno 14`，
日志里有 `[Agent] 会话结束，NPU 上下文已优雅释放。`

**边界**：`SIGKILL`（`kill -9`）拦不住，仍然泄漏。
所以**停服务只能用 `voice_start.sh --stop`**（走 SIGTERM），别用 `kill -9`。

**方法论收获**：把"必须重启才能恢复"当成结论之前，先问一句
**"这个资源是谁创建的、由谁释放"**。只要存在一条正常退出路径能走到释放点，
就不该接受"只能重启"这个答案。

---

### 36. 让 Agent 开关系统服务：自我关闭要延迟，systemctl 停不掉手动实例

**背景**：给 Agent 加了 `control_service(name, action)` 工具，可启停车辆看板(:8090)、
推流网关(:9000)、巡检守护、语音网关自身。

**坑一：杀自己之前要先把话说完**
用户说"关掉语音助手"时，Agent 自己就跑在语音网关里。直接 `systemctl stop` 会把
ASR、Agent、TTS 一起干掉，用户只听到半句就断线。

解法：先返回"将在 3 秒后关闭"，再 fork 一个**脱离会话**的子进程延迟执行：

```c
pid_t pid = fork();
if (pid == 0) {
    setsid();                                   // 脱离会话：父进程死了不影响自己
    int dn = open("/dev/null", O_RDWR);
    if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }   // 必须关掉继承的管道
    sleep(3);                                   // 留时间给上层播报
    execl("/bin/sh", "sh", "-c", "...", nullptr);
    _exit(0);
}
```

第三步不能省：子进程会继承父进程的 stdout 管道，不关掉的话调用方永远等不到 EOF（第 22 条）。

**坑二：`systemctl` 停不掉手动起的进程**
开发时用 `voice_start.sh` 手动起网关，生产时用 systemd。两条路径的进程归属不同，
`systemctl stop voice-gateway` 对**手动实例完全无效**。

解法：两条都执行，谁生效算谁：
```
sh -c "systemctl stop voice-gateway 2>/dev/null; pkill -TERM -f voice_gatewa"
```

**坑三：启动看板会让 Agent 自己变慢（实测掉一半）**
`dashboard` 跑 YOLOv8（npu 16 ms/帧，30 fps），是真抢 NPU：

| 状态 | decode 速度 |
|---|---:|
| 看板停止 | **7 tok/s** |
| 看板运行 | **3 tok/s** |

所以工具返回里带一条 `note`，要求模型**必须主动告诉用户"我之后的响应会变慢"**。
注意这是"给事实 + 附必要的上下文"，而不是替模型下结论。

**方法论收获**：让 Agent 操作"自己所在的系统"时，有三件事必须先想清楚——
**杀了自己怎么把话说完**、**不同启动方式怎么都停得掉**、**操作完之后自己出了什么变化要如实说**。

---

### 37. 耳机插着、`aplay` 返回成功、日志显示"播放 2s"，但一点声音都没有

**现象**：做声学自测（板子自己说、自己听）时，`aplay` 退出码 0、播放日志正常，
但麦克风录到的峰值只有 **0.04**，识别出来是「我的的的的的的没的设了」这类乱码。
一开始怀疑麦克风、距离、音量，全都不对。

**排查**：

```bash
amixer -c 2 cget numid=14   # Headphone Jack
→ values=on                 # 插孔检测说"耳机插着"

amixer -c 2 cget numid=16   # Headphone Switch
→ values=off                # ← 但输出通道是关的
```

**根因**：ES8388 的**插孔检测**和**输出开关是两个独立控件**。
插入耳机只会把 `Jack` 置 on，`Switch` 仍保持默认 off。
`aplay` 依然正常写入 DMA 缓冲并返回成功——声音在芯片内部就被掐断了，**没有任何报错**。

**解法**：

```bash
amixer -c 2 cset numid=16 on
```

**必须放进 systemd 的 `ExecStartPre`**，因为每次设备重开/重启它都会复位成 off。

**量化**：录音峰值 **0.04 → 0.20**（5 倍），识别从乱码变成 `小瑞小瑞小瑞`，
`bench_voice.sh` 跑出 **10/10**。

**方法论收获**：**"aplay 成功"只证明数据交给了 ALSA，不证明声音出了芯片。**
排查"没声音"要按层走：① 数据写没写进去（返回值）→ ② DMA 有没有在跑
（`/proc/asound/cardX/pcm0p/sub0/status` 的 `hw_ptr`）→ ③ 模拟侧开关/音量开没开。
这次的坑在第三层，前两层全是绿的。

---

### 38. ASR 把自己刚说的话又识别了一遍：`SIGSTOP` 冻进程 ≠ 清空 ALSA 缓冲

**现象**：语音网关播报完，ASR 立刻识别出一段新输入，内容正是**刚播报的回答**的一部分
（实测把自己的「我在」识别成「在」、把整段回答识别成新问题，直接污染了对话上下文）。

**排查**：日志里 `👤 [2] 在` 紧跟在 `🔊 播报: 我在` 之后，时间完全吻合。

**根因**：防自激用的是 `SIGSTOP` 冻结 ASR 子进程。但 **`SIGSTOP` 只停进程，不停硬件**：

- ALSA 的 DMA 仍在采集麦克风
- 播报的声音全部堆进环形缓冲
- `SIGCONT` 之后，`vad-alsa` 从缓冲区接着读，把这段回声当成新语音

**解法**（两层一起上）：

1. 恢复后再静默 2 秒：`g_mute_asr_until = time(nullptr) + 2`，这期间识别结果直接丢弃
2. 过短过滤从 3 字节提到 **6 字节**（单字「在」「嗯」会被挡掉，汉字最短 3 字节）

**方法论收获**：**"暂停消费者"不等于"暂停生产"。**
任何"停掉处理者来避免自反馈"的设计，都要追问一句：生产者那边还在往缓冲里写吗？

---

### 39. 停一次服务就要重启板子：`cleanup` 等 2 秒不够，`rkllm_destroy` 需要 3~5 秒

**现象**：`systemctl stop` 之后 NPU 内存不释放，下次启动 `errno 14`，只能重启板子。
一天之内为此重启了四次。

**排查**：

- `dmesg | grep "used:"` 一直停在 `3873 MiB`。**但要注意：那是"分配失败那一刻"的快照，
  不是当前占用**（它只在 `dma map fail` 时才打印）。拿它当实时值会误判。
- 给 agent 装了 SIGTERM handler（第 35 条）之后，**手动** SIGTERM 能干净退出，
  但**走 gateway 的 cleanup** 就泄漏 —— 说明问题不在 agent，在网关的等待时间。

**根因**：`reap()` 的流程是 SIGTERM → 等 **2 秒** → SIGKILL。
而 agent 收到 SIGTERM 后要执行 `rkllm_destroy()` 释放那 2 GB 映射，**实测需要 3~5 秒**。
2 秒的刀正好砍在释放中途，映射表留下半截 → IOVA 泄漏。

**解法**：等待时间 2 秒 → **6 秒**。

**方法论收获**：**优雅退出的超时值必须大于它真正需要的释放时间。**
拍脑袋写 2 秒，等于把"优雅"变成"半途而废"——而且表现和"根本没装 handler"一模一样，
极难定位。

---

### 40. 改用 systemd 托管后，连踩三个"以为能行"的坑

**背景**：为了让语音网关不受 SSH 会话影响、能开机自启，从 `nohup ... &` 改成 systemd 托管。

**坑一：`nohup` 和 `setsid` 都挡不住 SSH 断开**
ssh 命令一结束，网关就收到 SIGHUP 被杀（日志里出现 `已回收全部子进程`）。
`nohup` 只对直接子进程生效，`setsid` 也没能彻底脱离——子进程组的信号传播比想象中复杂。
**解法**：用 `systemd-run --unit=xxx` 或写 unit 文件，交给 init 系统管。

**坑二：unit 里没有的服务，等于不存在**
`voice_start.sh` 明明打印"✓ TTS daemon 就绪"，网关却一直报"daemon 不可用"。
**根因**：`voice-gateway.service` 的 `ExecStart` **只有一个 `/root/voice_gateway`**，
根本不含 `tts_daemon`——手动脚本里那套启动顺序，在 systemd 下完全没被执行。
**解法**：拆成独立服务 `tts-daemon.service`，网关用 `After=` + `Wants=` 声明依赖。

**坑三：单元文件里漏了环境准备**
改用 systemd 后耳机又没声了。**根因**：`voice_start.sh` 里的
`amixer cset numid=16 on` 在 systemd 路径下不会执行。**解法**：写进 `ExecStartPre=`。

**方法论收获**：**从"手动脚本"迁移到"服务化"时，脚本里每一行环境准备都要显式搬过去。**
`ExecStart` 只写主程序，那些 `amixer` / `mkdir` / `rm` / 环境变量全都会消失，
而且**失败是静默的**。

---

### 41. 编译优化对这个系统完全无效（负面结论也是结论）

**做了什么**：写 `bench_params.sh` 自动扫参数，每组「编译 → 起 → 跑固定问题 → 提 tok/s」：

| 编译 flag | max_context_len | prefill | decode |
|---|---|---:|---:|
| -O2 | 8192 | 1886 ms | **7 tok/s** |
| -O2 | 4096 | 1863 ms | **7 tok/s** |
| -O3 | 8192 | 1887 ms | **7 tok/s** |
| -O3 | 4096 | 714 ms | **7 tok/s** |

**结论**：`decode` 恒等于 **7 tok/s**，`-O2`/`-O3`、`ctx 8192/4096` 全部无差异
（`-O3 4096` 的 714 ms 是 page cache 命中，不算数）。

**根因**：瓶颈在 **NPU 推理 + 内存带宽**。C++ 侧只负责读 sysfs、拼 prompt、解析输出，
总共几毫秒——`-O3` 再怎么优化，也被 NPU 的推理时间淹没。

**价值**：这个**否定结论**省掉了以后所有"要不要开 -O3 / LTO / 换编译器"的纠结。

**方法论收获**：优化之前先算**理论上限**。
2 GB 权重 × 7 tok/s = 14 GB/s，已接近 NPU 可用带宽——这时候任何 CPU 侧的微调都是噪声。

### 42. `pkill -x` / `pgrep -x` 对长进程名永远不匹配

**现象**：`pkill -x rkllm_embodied_agent_p0` 返回 0（成功），进程却还在跑；
`pgrep -x` 也查不到自己明明看着在运行的进程。两者都不报错。

**排查**：

```bash
ps -eo pid,comm | grep rkllm
# 2062 rkllm_embodied_        ← 只剩 15 个字符
cat /proc/2062/comm
# rkllm_embodied_            ← 完整名字是 rkllm_embodied_agent_p0
```

**根因**：内核的 `task_comm` 只有 **16 字节**（含结尾 NUL），进程名超过 **15 字符**就被**静默截断**。
`-x` 是拿 **comm** 做精确匹配，不是 cmdline —— 用完整名字去匹配必然匹配不到，
而且 `pkill` 找不到进程也算"执行成功"，于是表现为「杀了，但没杀掉」。

**解法**（三种，按可靠性排序）：

| 场景 | 命令 |
| :--- | :--- |
| 按 pid 杀（最稳） | `kill -TERM $(ps -eo pid,comm \| awk '$2=="rkllm_embodied_"{print $1}')` |
| 按截断后的 comm | `pkill -x rkllm_embodied_` |
| 按完整 cmdline | `pkill -f "rkllm_embodied_agent_p0"` —— **注意会匹配到自己**，见第 9 条 |

**顺带**：`voice_gateway`（13 字符）没超限，所以代码里的 `pgrep -x voice_gateway` 是安全的；
`rkllm_embodied_agent_p0`（24 字符）超了，脚本里必须走 pid 或 `-f`。

---

### 43. 快速通道把「怎么做」当成「现在怎样」

**现象**：问「语音助手日志怎么看」，快速通道命中服务分支，回了句「正在运行。」——
问的是运维方法，答的是运行状态。同一类还会把「看板怎么开」「网关是什么」当成 status 查询。

**根因**：意图路由是**关键词匹配**，只能看出句子里出现了服务名，
分不清「下指令」（打开看板）和「问方法」（看板怎么开）。而 status 是不带副作用的默认分支，
所以任何含糊的问句都会掉进「查一下状态」。

**解法**：在服务分支前加一层**元问题拦截**——句子里出现「怎么/如何/为什么/是什么/
干什么/有什么用/在哪/哪里」就不进服务分支，交回 LLM 自己组织答案。

```cpp
const bool meta = has_any(q, {"怎么","如何","为什么","是什么","干什么","有什么用","在哪","哪里"});
if (!meta && has_any(q, {服务名...})) { ... }
```

另外「日志」单独走一条短路，直接给真实命令（`journalctl -u voice-gateway -f`），
顺带解决「LLM 编造不存在的日志路径」——这也是把事实塞进代码而不是 prompt 的一例。

**代价**：像「灯怎么关」这种带方法的指令会退回 LLM（多等 10 秒）。
可接受——**宁可慢一点，也不能答得驴唇不对马嘴**，这是意图路由的第一原则。

---

### 44. `systemctl stop` 一个没在跑的服务，返回 0

**现象**：用户说「关闭车辆检测」，工具返回 `success: true`、回答「已停止」，
但那个服务本来就没在运行——**这是一次成功的 no-op，却被报成了「刚刚停下」**。
同理 `systemctl start` 一个已在运行的服务也返回 0。

**根因**：`systemctl` 的返回值语义是「目标状态已达成」，不是「我做了状态迁移」。
所以「已经是 inactive」和「我刚把它停掉」在返回码上完全一样。

**解法**：操作前先读一次真实状态，用它决定措辞：

```cpp
std::string prev = (action == "status") ? ""
                 : run_cmd("systemctl is-active " + svc->unit);
// ... 执行 action ...
if (action == "stop" && ok && prev != "active")
    ss << "\"note\": \"本来就没在运行。\"";
```

配合快速通道的措辞，用户听到的是「看板本来就没在运行」，而不是含糊的「已停止」。

**为什么值得单独记**：这是第 37 条（`aplay` 成功≠出声）的同族问题——
**返回码表达的是「状态已满足」，不是「动作已发生」**。凡是幂等命令（systemctl / kill / rm -f /
mkdir -p）都有这个陷阱，用之前先问一句：我怎么知道它「本来是」什么样？

---

### 45. 同一份枚举表维护了两遍，第二遍必然漏

**现象**：把灯设成报警后问「灯是什么状态」，回答是 **「指示灯当前是alarm_blink。」**——
一句中英混杂的话。用户听到的是一串英文标识符。

**根因**：`tool_get_led_status()` 里已经有一份完整映射（heartbeat / alarm / **alarm_blink** /
on / off），而快速通道为了"少解析一层"，又自己写了一份简化版：

```cpp
std::string desc = (mode == "heartbeat" ? "心跳呼吸" : mode == "alarm" ? "报警急闪" ... : mode);
//                                                                          ↑ 漏了 alarm_blink
```

漏掉的 `alarm_blink` 是**用户态爆闪**（不是内核 trigger，所以看 `/sys/class/leds/work/trigger`
根本找不到它），只有工具里那个 `led_user_blink_alive()` 判断才知道。

**解法**：快速通道直接读工具返回的 `description` 字段，不再自己映射：

```cpp
std::string desc = extract_json_field(tool_get_led_status(), "description");
if (desc.empty()) desc = extract_json_field(st, "current_mode");
```

**通用教训**：同一个事实只能有**一处**定义。为了让调用方"少解析一层"而复制一份映射，
等于给自己埋一个"新增枚举值时忘了同步"的定时炸弹，而且它只在**新值出现时**才炸。

---

### 46. 说「谢谢」，它调了三个硬件工具

**现象**：对 Agent 说「谢谢」，它连续调 `get_system_health` + `get_led_status` +
`get_patrol_status`，回了 5 秒、一屏设备状态。说「你好」也一样（调 `get_led_status`）。

**根因**：2B 模型把"有输入就必须有动作"当成了规则，闲聊时也要找点事做。
sys_prompt 里写了"打招呼、闲聊不调工具"，**但它不听**——这类约束对 2B 的约束力很弱。

**解法**：在快速通道里把社交用语直接答掉，不给模型调工具的机会：

```cpp
if (q.size() <= 12 &&
    has_any(q, {"谢谢","多谢","感谢","收到","明白","好的","辛苦了","嗯","ok"})) {
    out = pick_variant({"不客气。", "嗯。", "好。"}, seed);
    return true;
}
```

长度限制 12 字节是护栏：避免把「好的，把灯关掉」这种带诉求的句子也吞掉（21 字节，不会被吃）。

**为什么不用 prompt 解决**：这是典型的 B 类问题（模型行为），修不完。
凡是"绝不允许"的，都要落成**代码里的短路**——把 B 类转成 A 类。

---

### 47. `systemctl is-active` 说 active，服务 7 秒后就死了

**现象**：用户说「打开推流网关」，Agent 回答「推流网关已经打开了，访问地址
tcp://<board-ip>:9000」。用户去连——连不上。翻日志才发现进程早就退出了。

**证据链**（2026-10-31 15:38，全是 dmesg/journal 原文）：

```
15:38:24  w4_edgevision_gateway: [System] 正在加载 NPU 模型
15:38:28  RKNPU: rknpu_gem_get_pages: dma map 1228800 fail   ×3
15:38:28  used: 4094 MiB                                     ← IOMMU 满了
15:38:27  [NPU Core #1] 引擎已释放 / [TCP Server] 已关闭      ← 退出
```

**根因**：`control_service` 的启动判据是「`sleep(3)` + `is-active`」。
而**模型加载失败要 7 秒才暴露**——第 3 秒那个窗口里，进程当然还活着，
于是判据给出 active，Agent 就宣布成功了。

**解法**：启动判据从「进程活着」升级为「**功能可用**」：

```cpp
if (a == "dashboard") ok = http_ok("8090", 10);   // curl 返回 200 才算起来
else if (a == "gateway") ok = port_ok("9000", 10); // :9000 在 listen 才算起来
// 失败时明确说：进程起来了但端口没通，多半是 NPU 内存不足导致模型加载失败
```

**为什么这条值得单独记**：它是第 37 条（`aplay` 成功≠出声）、第 44 条（`systemctl stop` 的
no-op 也返回 0）的同族——**「操作成功」和「目标达成」是两件事**。
凡是"启动成功"，都要问一句：我探的是**生存**，还是**功能**？

在线时长、端口、HTTP 码、文件可读——这些才是功能；`is-active`、返回码、进程存在，只是生存。

---

### 48. 同名脚本有几份，改了那份不生效的那份还在

**现象**（2026-10-31 整理 `/root` 时发现，尚未真实触发）：`/root` 下有 5 个单字短名脚本
`bp.sh` `bv.sh` `te.sh` `tk.sh` `vs.sh`，是平时敲熟了的手感入口。逐份对 md5 才发现：

| 短名 | 正本 | 状态 |
| :--- | :--- | :--- |
| `bp.sh` | `bench_params.sh` | 相同 |
| `bv.sh` | `bench_voice.sh` | 相同 |
| `te.sh` | `test_e2e.sh` | 相同 |
| `tk.sh` | `test_kws.sh` | **不同** |
| `vs.sh` | `voice_start.sh` | **不同** |

差异不是无害的：

```
vs.sh  缺 setsid nohup                        → TTS 常驻进程会被 SIGHUP 带走
vs.sh  缺 amixer -c 2 cset numid=16 on        → 耳机开关 off，完全没声音
tk.sh  --keywords-score=1.0 --keywords-threshold=0.25
       现为 3.0 / 0.10                        → 唤醒率会被测低
```

`vs.sh` 这一条最要命：用旧副本启动网关，`aplay` 照常返回 0、日志照常显示播放完成、
服务照常 `active`——**只有声音没有**。跟第 37 条是同一个坑，只是换了个入口让人踩进去。

**根因**：同一个用途存在多份**实体副本**。scp 上传时为少打字起了短名，后来正本更新了，
短名那份没人管，就停在旧版本。它不报错、不警告，只是悄悄给你旧行为。

**解法**：短名保留（手感不能丢），但**全部改成软链接指向正本**，从此不可能再漂：

```bash
for pair in "bp.sh bench_params.sh" "bv.sh bench_voice.sh" "te.sh test_e2e.sh" \
            "tk.sh test_kws.sh"     "vs.sh voice_start.sh"; do
  set -- $pair; rm -f "/root/$1"; ln -sfn "/root/$2" "/root/$1"
done
```

同时清掉了 `/root/rkllm_deploy/` 里 5 份 Agent 源码中的 4 份（只有 `rkllm_embodied_agent.cpp`
是当前的，其余含一份与正本 md5 完全相同的 `agent_new.cpp` 中转残留），
旧文件移到 `/root/archive/2026-10-31-dedup/`，附 `MANIFEST.md` 说明来历与还原方法。

**为什么这条值得单独记**：这是"假成功"在**文件层面**的版本。前面 37 / 44 / 47 条讲的是
运行时骗你，这条是**源码骗你**——你读到的不是你以为的那份代码。
一条命令自检：

```bash
ls -l /root/*.sh | awk '$1 ~ /^l/ {print $9, $10, $11}'   # 软链接该有的样子
```

`find /root -name '*.cpp' | xargs md5sum | sort | uniq -w32 -d` 能揪出内容完全相同的副本。

---

### 49. 跑出「唤醒率 0/10」——其实一声都没出

**现象**（2026-10-31 21:20）：跑 `bench_voice.sh "小瑞" 10`，结果 **0/10**，
识别出来的文本是：

```
小孩女孩玩的玩具 / 小孩公衣 / 小爱小妈 / 小班英语 / 我想爱你
小孩儿的 / 想说一点话好吗 / sorry you / 小孩女女人的的的的的的的女子 / 小怪鱼
```

看起来像"唤醒词识别率极差"，而且这些结果**全是 `小 + X` 结构**，
很容易顺势得出"模型不认识小瑞，只能往高频词上凑"的结论。

**但这是错的。** 接着查：

```
① 同一句话用 TTS 合成成干净 16k 单声道，离线识别：
     小瑞 → 小瑞        ✓
     温度多少 → 温度多少  ✓
     开启语音助手 → 开启语音助手 ✓       三个全对，模型没问题
② 拿录音文件算能量包络（每 0.5 s 一格）：
     0.0s  -22 dB   ← 这是 arecord 启动的瞬态
     0.5s  -30 dB   ┐
     1.0s  -31 dB   ├ 之后一路 −30 ~ −32 dB，全程只有底噪
     4.5s  -32 dB   ┘
③ 满音量 aplay 一个已知 wav，同时录音 —— 包络依然一条直线
④ 查声卡：amixer -c 2 cget numid=14  →  Headphone Jack = on
```

**根因（两层，第二层才是真凶）**：

第一层：`amixer -c 2 cget numid=14` → `Headphone Jack = on`，
声音优先走耳机口，摄像头麦克风在空气里收不到。

第二层（**这才是主要的**）：

```
amixer -c 2 sget Speaker   →  Mono: Playback [off]
```

ES8388 的 **`Speaker` 是板载扬声器通路的总开关，默认 off**。
而 `voice_start.sh` 和 unit 的 `ExecStartPre` 里**只写了 `numid=16`（耳机开关），没写 Speaker**。
所以板子从一开始就只往耳机口送声音——**只要没插耳机，板载喇叭就是静音的**，
而 `aplay` 依旧返回 0、音量显示满档、日志显示播放完成。

打开后立刻测到台阶：

```
改前：峰值 −26.85 dB @ 0.00s（0.00s 是 arecord 启动瞬态，不是语音）
改后：峰值 −24.36 dB @ 0.75s（0.75s 才是真实播放时刻）
      台阶 8.4 dB  ≥ 8 dB  → 通过
```

**同一个根因还带出一个音量 bug**：unit 里原先写的是

```ini
ExecStartPre=/bin/sh -c '... amixer -c 2 sset PCM 200 ...'
```

而 `PCM` 的上限是 **192（0 dB）**，`200` 被内核钳到满档。
于是**每次开机音量都被拉到最大**——这就是"怎么这么大声"的来源。
已改为 `PCM 160`（−13.5 dB）+ `Output 1/2 28`（−2 dB），
并且 `voice_start.sh` 与 unit 两处同步修改（**同一个参数在两处设置，必须都改**）。

**`loop_check.sh` 的配套设计**：平时播报用舒适音量，但自检需要足够电平。
所以它在开测前把音量临时拉满、用 `trap ... EXIT` 保证测完恢复，
只在中间播 1 秒「小瑞」。否则会出现"为了能自测就得一直很吵"的两难。

**为什么这条比第 37 条更阴险**：第 37 条是"没声音"，你会去查耳机开关；
这条是"没声音，但测试照样给你一个数字"。0/10 是个**看起来很真实的测量结果**，
它带着 10 条具体的识别文本，任何人看了都会去改唤醒词、改 ASR 参数，
而真相是**根本没在测量**。

**解法**：`/root/loop_check.sh` —— 播一段已知音频、同时录音、算能量台阶，
台阶 < 8 dB 直接判定回路不通并退出。

```bash
bash /root/loop_check.sh      # 通过返回 0，不通返回 1 并打印排查命令
bash /root/loop_check.sh -v   # 附能量包络明细
```

`bench_voice.sh` 和 `test_e2e.sh` 开头都已接入这一关，**过不了就不跑**。

**判据为什么是"能量台阶"而不是"能不能识别"**：
识别成功需要露天的信噪比、模型、VAD 全部配合，任何一个环节坏都失败；
而"空气里有没有声音"只需要看能量有没有台阶。用能识别的判据去测回路，
会把一个物理问题伪装成算法问题——这正是上面 0/10 的由来。

**顺带解释了另一件事**：`CURRENT_STATUS.md` 里 `bench_voice.sh` 记的是
**实测 10/10**（10-31 12:50），当晚变成 0/10。中间唯一的变化是耳机插上了。
所以**"唤醒率不稳"这类历史结论，要先确认当时回路通不通，再谈模型行不行。**
没有回路自检的语音测量，失败原因是不可归因的。

---

---
| 类别 | 一句话 |
| :--- | :--- |
| 算法 | DFL 距离是格子单位，**必须乘 stride** |
| 算法 | 重构要用**误差对比**验收，不靠肉眼 |
| 量化 | 校准集决定分类分支死活；余弦相似度高 ≠ 模型可用 |
| C++ | 张量布局先打印 dims，再写分支 |
| 性能 | 同一算法，访存顺序能差一倍 |
| 验证 | 「看起来在工作」和「在工作」是两件事，检查输出有没有被消费 |
| 验证 | 同一个假象会复制到多处，修完一处要全文搜同类模式 |
| 环境 | 缺某个大依赖时，先问「它真的需要吗」 |
| 环境 | 文件大小对 ≠ 下载成功，`gzip -t` / `md5sum` 才是判据 |
| Shell | `pkill -f` 会匹配到调用者自己，用 `pgrep -f '^...'` |
| 编译 | `"xxx.h"` 优先搜源文件目录，改过却不生效先 `find` 同名副本 |
| 并发 | 实时推流队列必须做背压/丢旧，否则断连重连会导致历史积压雪崩 |
| 摄像头 | 不设 `CAP_PROP_FPS` 就用设备默认档（20fps），**默认值 ≠ 物理上限** |
| 摄像头 | `cap.get(CAP_PROP_FPS)` 回读值会骗人，必须实测帧间隔才算数 |
| OpenCV | `initUndistortRectifyMap(m1type=CV_16SC2)` 固定产出 map2=CV_16UC1，定点/浮点查表是整条方案的开关 |
| OpenCV | `cv2.FileStorage` 在 Windows 走窄字符 fopen，**中文路径必炸**；imread 却没事（API 不一致），写 yml 先试 ASCII 临时目录再搬 |
| 升级 | OpenCV 5.0 删了 `FileNode.int_value()`；标定脚本换机器跑先冒烟一遍再信「上次是好的」 |
| 测量 | 两个口径给出相同数字 = 口径错了 |
| 报告 | 指标要和任务对齐，并且必须附带局限 |
| MPP | 「参数怎么调都返回同一个错误码」→ 优先怀疑库/驱动版本倒挂，源码编译库做 A/B 对照 |
| MPP | 系统预装 so 不一定匹配板上内核；静态链源码编译库一行代码不用改 |
| 验证 | 检测链路排错必须用「已知含目标+已知分数」的锚点图，不能随手拿图当输入 |
| 验证 | 图像链路排错：dump 中间结果与参考做逐通道线性回归，相关性≈1 就是好的 |
| 测试 | 多张「不同」测试图先 md5sum——同名不同图/同图不同名的素材混乱浪费半小时 |
| 测试 | 板上摄像头实验**一次只允许一个会话**：两个 agent 并行测会互抢 /dev/video81/83 互相挤停（连 systemd 服务都被挤出 inactive）；先 fuser 确认空闲再开跑 |
| 测试 | `fuser` 报 PID 但 `ps` 查无此进程 = 陈旧内核记录，不算占用 |
| 端侧Agent | RKLLM `role="tool"` 强制要求 JSON 数组 `[{...}]`，单个 JSON 对象会被 runtime 直接拒绝 |
| 端侧Agent | sysfs 写入必须逐步查返回值+回读比对，否则「底层 EINVAL + 上层不验」出假成功；内核裁剪过的触发器（如 timer）写了就 EINVAL |
| 端侧Agent | 只 fork 不 exec 的子进程会继承 NPU 内存映射，孤儿进程能把 196MB NPU CMA 钉死（malloc npu memory errno 14） |
| NPU | **RK3588 NPU IOMMU 只有 32 位 / 4 GiB 地址空间**（与物理内存无关）：2B 大模型每实例约吃 ~1.95 GB IOVA，干净板最多 2 个，叠加视觉网关后只能 1 个；第 N+1 个报 `errno 14 Bad address` |
| NPU | `rkllm_init failed` + `errno 14` 先看 dmesg 的 `used: N MiB`，**`used + 请求 size > 4096 MiB` 就是 IOMMU 耗尽铁证**，别先去升驱动 |
| NPU | errno 分诊：**14 = EFAULT 地址映射失败**、12 = 真 OOM、23/24 = fd 耗尽；`size` 远小于 MemAvailable 却失败 → 限制不在物理内存那一层 |
| NPU | `flags: 0x2` 按 `rknpu-ioctl.h` = **CACHEABLE**（runtime 另 OR 上 IOMMU\|ZEROING）→ 走 dma-buf+IOMMU，**不占 CMA**；所以「CmaFree=0」与 rkllm 能否加载无关 |
| 排错 | **「昨天好好的今天坏了」≠ 代码回归**：同一份二进制换环境结果不同 = 资源竞争；先查「机器上还有什么在跑」，别先 git diff / 先怀疑版本 |
| 排错 | 日志里**相邻出现的 warning 和 error 不一定有因果关系**（`driver version too low` 是软警告，真凶在下一行 `dma map fail`） |
| 端侧Agent | 长寿子进程 exec 前必须把 0/1/2 重定向到 /dev/null，否则持有父进程 stdout 管道写端 → 读端等不到 EOF，表现为「命令卡死」 |
| 端侧Agent | 模型把参数包在换行里（`\nalarm\n`），提取后必须 trim；否则打印成空值、白名单还会误判非法 |
| 端侧Agent | **周期性物理量（LED brightness）绝不能单点采样定性**：灭相与「关闭」编码相同，撞上就误判。**判据优先用占空比这类纯观测量**——「查爆闪进程是否存活」只对用户态爆闪成立，内核 `heartbeat` 触发器没有 pid，换驱动方式就静默失效（该坑踩了两次：爆闪 50% 误判、心跳 78% 误判） |
| 排错 | **修一个坑时要问「这个判据依赖的前提，在其它分支上还成立吗」**：判据一旦绑定某种实现（如"有 pid 文件"），换实现就静默失效。**观测优先于归因**——占空比对任何驱动方式都成立，「谁在驱动」只对已知的几种成立 |
| 端侧Agent | Agent 给出离谱结论时，**先把 Observation 逐条拉出来对齐物理事实**——很可能是感知层喂了错数据，不是模型幻觉，别一上来调 prompt |
| 端侧Agent | **演示判 PASS ≠ 没问题**：断言只验「有没有调工具/JSON 合不合法」，验不了「工具返回的值对不对」。要翻 trace 逐条人看 |
| 端侧Agent | 领域常识必须写进 sys_prompt：模型不知道「这块板子多少度才算热」，就会把**人为压低的演示阈值**当成真实红线，把正常温度判成过热。**但注意镜像坑（第 28 条）**：这条常识会让模型拿 75°C 去否定守护进程的真实告警，必须同时声明「当前生效阈值只能从运行态查询，不得用常识替代」 |
| 端侧Agent | 明确禁止模型**臆测工具内部实现**（枚举值编码、sysfs 数值含义），它会一本正经编一段「若系统内部逻辑为 0=off…」 |
| 编译 | **`CMAKE_BUILD_TYPE` 为空时 CMake 不加任何优化标志且不报警**——"编译成功"≠"编译正确"，构建摘要要自检 `-O` 并 `message(WARNING)` |
| CMake | 变量名**大小写敏感**：`CMAKE_BUILD_TYPE` 的值是 `Release`，但标志变量是全大写 `CMAKE_CXX_FLAGS_RELEASE`，拼之前必须 `string(TOUPPER)` |
| 性能 | **不同构建设施（手工 `g++ -O2` vs CMake -O0）的数字不能横向比较**，混用会互相掩盖问题 |
| 性能 | 做 A/B 前先设一个**"理论上不该变"的对照组**（如 NPU 纯推理），它稳了差异才能归因 |
| 性能 | **不绑核时 `mean` 会骗你**：调度迁移的长尾把 O0 的 mean 从 3.45 拉到 8.15 ms（p50 不变），能把 5.5× 压成 2× |
| MPP | **"硬件加速" ≠ "更快"**：硬编 = 0.35 ms 固定开销 + 7.0e-6×像素，与软编每像素算力打平 → 小任务/多核并行时反而输 |
| MPP | 怀疑"是不是回退到软件"时，查 `/proc/interrupts` 里硬件核的中断**增量**，这比看返回值可靠 |
| MPP | `prep:range` / `set_color_range` 这类键**存在但被忽略**：12 组组合输出字节相同 → 只能靠 PSNR 这类端到端指标验证 |
| MPP | `cv::cvtColor` 出的是 **limited-range** YUV(16~235)，JPEG 要求 full-range，硬编原样写入会画面偏暗、PSNR 掉 11 dB |
| 测试 | **测试失败先证明测试工具没坏**：用完全独立的路径（curl / 浏览器 / 最小复现）打同一目标做交叉验证，别一上来读被测代码 |
| Python | 闭包里 `x += y` 会让 `x` 在**编译期**被判成本函数的局部变量 → `UnboundLocalError`；`bytearray` 的 `+=` 运行时是就地修改也救不了，改 `x.extend(y)` 消除赋值形态 |
| 测试 | `ast.parse` / `-fsyntax-only` / `node --check` 只证明「能解析」，**证明不了跑起来对**；有调用链的必须 mock 真跑 |
| 测试 | 阴性结果要归因到具体机制（这次是「编译期名字绑定 vs 运行期就地修改」），不能停在「它坏了」 || C++ | 非限定名字查找**不会跨 namespace**（ADL 只作用于函数调用、不作用于类型名）：把 `A::Foo` 拿到 `namespace B` 里用，得先 `using A::Foo;`，否则报 `'Foo' does not name a type` |
| Web | **先确认前端资源是「运行时读文件」还是「编译期内联」**——决定改完 HTML 要不要重新编译。判据：启动日志里有没有打印读到的字节数 |
| 前端状态机 | 除了「正常 / 成功过后失败」，还要单独处理 **「一次都没成功过」** 这个初始态，否则会出现表格与空态同显、横幅永不弹 |
| 端侧Agent | **阈值是进程内运行时状态，不暴露给模型 = 模型只能编**：它会拿 prompt 里的默认值/常识当"当前生效阈值", 据此否定守护进程的真实行为。必须把运行态(阈值/状态/pid/时间戳)外化成可读快照 + 独立查询工具 |
| 端侧Agent | 工具**只给事实不给结论**：返回 `status: normal_optimal` 这类预判字段, 模型会直接抄、跳过推理, 而结论很可能基于错的前提 |
| 端侧Agent | 判断「进程还活着吗」用**三条件与**：快照说 running + 时间戳未过期 + `kill(pid,0)==0`。只看时间戳漏「刚死未过期」的两三周期, 只看 pid 漏 pid 被内核复用 |
| 端侧Agent | **结论对、过程错**比结论错更危险：Agent 答「灯常亮」碰巧与物理一致, 但对的原因是"守护崩了留僵尸态"而非"温度低", 这种答案没人会复查 |
| 端侧Agent | 残留自检要先分清 `trigger=none` 的两种含义：**用户态爆闪中** 与 **异常退出的僵尸态**。漏判会把正在报警的灯改成 heartbeat, 等于亲手按掉警报 |
| 系统 | **"必须重启才能恢复"往往只是没走释放路径**：给进程装 SIGTERM handler（`sa_flags=0`，不设 SA_RESTART）让主循环自然退出，`destroy()` 才执行得到 |
| 系统 | `dmesg` 里的 `used: N MiB` 是**分配失败那一刻的快照**，不是当前占用——拿它当实时值会误判 |
| 端侧Agent | 让 Agent 关自己：**延迟 3 秒 + `setsid()` 脱离会话**，先把话播完；`systemctl` 停不掉手动起的实例，必须再配一条 `pkill` |
| 端侧Agent | Agent 操作系统服务后要**如实报告自身的副作用**：开看板抢 NPU，实测 decode 从 7 掉到 3 tok/s，必须主动告知用户 |
| 音频 | 耳机开关 numid=16 默认 off：**aplay 成功 ≠ 出声**，先 `amixer -c 2 cset numid=16 on` |
| 音频 | SIGSTOP 冻 ASR **≠** 清空 ALSA 缓冲：播报后必须留 2 秒静音窗，否则会识别到自己的回声 |
| NPU | 优雅退出只等 2 秒不够：rkllm_destroy 释放 2 GB 要 3~5 秒，等不够就 IOVA 泄漏、只能重启 |
| systemd | 从脚本迁到 unit：ExecStart 只写主程序，脚本里的 amixer/mkdir/环境变量**全部静默消失** |
| 性能 | 优化前先算理论上限：2 GB × 7 tok/s = 14 GB/s 已贴带宽墙，CPU 侧微调都是噪声 |
| 进程 | `-x` 匹配的是被截断到 15 字符的 comm；长进程名用 pid 杀，别信 `pkill -x` 的返回值 |
| 意图路由 | 关键词分不清「下指令」和「问方法」；含「怎么/在哪/为什么」一律交回 LLM |
| systemd | `systemctl stop` 没在跑的服务也返回 0；先读 prev 状态，别把 no-op 报成「已停止」 |
| 意图路由 | 枚举表别维护两遍：状态描述统一读工具返回的 description |
| 意图路由 | 闲聊/道谢必调工具——用代码短路，别指望 prompt 管住 2B |
| systemd | 启动成功要探**功能**（端口/HTTP），别只探**生存**（is-active 会在加载失败前报 active） |
| 工程习惯 | 同一用途**只留一份实体文件**，短名一律做软链接；多份实体副本必然各自漂移，而且旧的那份不报错 |
| 音频 | 开机没声音先查两级：`numid=16`（耳机开关）**和 `Speaker`（板载喇叭总闸，默认 off）**；只开前者时没插耳机就全程静音 |
| 音频 | `PCM` 上限 192=0 dB，写 200 会被钳到满档——"莫名很吵"往往是这里；同一参数散在两个文件里就要两处一起改 |
| 测量 | 回路不通时测试仍会给出具体数字：先证明"信号真的进到空气里"（能量台阶），再谈识别率 |
