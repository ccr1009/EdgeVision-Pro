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
`lab/w4_edgevision_gateway.cpp` 215~222 行是**同一个毛病的另一份拷贝**：

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
   `lab/w4_edgevision_gateway.cpp:133-134`、`lab/w4_edgevision_preview_real.cpp:259-260`、
   `lab/bench_cam_fps.cpp:11-13`，三处**都只设了 `FOURCC` + `WIDTH/HEIGHT`，没有一处设 `CAP_PROP_FPS`**。
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
> `-O` 都没有——等于项目跑了一个月全是 -O0。CMake 在 build type 为空时**不报错不警告**，
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
| 端侧Agent | 长寿子进程 exec 前必须把 0/1/2 重定向到 /dev/null，否则持有父进程 stdout 管道写端 → 读端等不到 EOF，表现为「命令卡死」 |
| 端侧Agent | 模型把参数包在换行里（`\nalarm\n`），提取后必须 trim；否则打印成空值、白名单还会误判非法 |
| 端侧Agent | **周期性物理量（爆闪灯的 brightness）绝不能单点采样定性**：灭相与「关闭」编码相同，撞上就误判。要判「爆闪进程是否存活」，或多点采样看有无翻转 |
| 端侧Agent | Agent 给出离谱结论时，**先把 Observation 逐条拉出来对齐物理事实**——很可能是感知层喂了错数据，不是模型幻觉，别一上来调 prompt |
| 端侧Agent | **演示判 PASS ≠ 没问题**：断言只验「有没有调工具/JSON 合不合法」，验不了「工具返回的值对不对」。要翻 trace 逐条人看 |
| 端侧Agent | 领域常识必须写进 sys_prompt：模型不知道「这块板子多少度才算热」，就会把**人为压低的演示阈值**当成真实红线，把正常温度判成过热 |
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
