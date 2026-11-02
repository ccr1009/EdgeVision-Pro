# lab/ 是什么

**这里是唯一的源码目录。** 板子上的同名文件都是部署副本，改板子上的不算数。

目录里有 73 个文件，但**当前在用的只有下面这 11 个**。其余是 10-22 ~ 10-27 的实验草稿
（各种 `_p0_*` `_p6_*` `bench_*`），留着是为了查历史，**不要改它们**。

## 一、活文件清单（改这些）

| 文件 | 是什么 | 部署到板上 |
| :--- | :--- | :--- |
| `rkllm_embodied_agent.cpp` | Agent 主体：工具定义、sys_prompt、快速通道、护栏 | `/root/rkllm_deploy/rkllm_embodied_agent.cpp` |
| `voice_gateway.cpp` | 语音编排：ASR 子进程、软唤醒、半双工、TTS 分段播报 | `/root/voice_gateway.cpp` |
| `regress.py` | **回归用例 20 条**：改完跑，全绿再继续 | `/root/regress.py` |
| `explore.py` | **探索性自测**：撞未知边界（注入/矛盾/越权/假成功） | `/root/explore.py` |
| `test_e2e.sh` | 语音端到端自测（靠声学回路，板子自己说给自己听） | `/root/test_e2e.sh` |
| `loop_check.sh` | **声学回路连通性自检**：播一段录音，看空气里有没有能量台阶 | `/root/loop_check.sh` |
| `bench_voice.sh` | 唤醒词命中率基准 | `/root/bench_voice.sh` |
| `bench_params.sh` | 参数扫描（-O2/-O3 × ctx） | `/root/bench_params.sh` |
| `voice-gateway.service` | 语音网关 unit | `/etc/systemd/system/` |
| `tts-daemon.service` | TTS 常驻 unit | `/etc/systemd/system/` |
| `edgevision-dashboard.service` | 8090 看板 unit | `/etc/systemd/system/` |

> `rkllm.h` / `.rkllm` 模型 / `lib/librkllmrt.so` 不在 lab/，在 `rkllm_deploy/`（厂商产物，别动）。

## 二、板子上的目录

| 路径 | 内容 |
| :--- | :--- |
| `/root/rkllm_deploy/` | Agent 二进制 + 2.98 GB 模型 + `rkllm.h` + 运行库 |
| `/root/voice_gateway` + `.cpp` | 语音网关二进制与源码副本 |
| `/root/tts_daemon` + `.cpp` | TTS 常驻服务 |
| `/root/asr/` | 全部模型：Paraformer、VITS、KWS、VAD、两版 runtime |
| `/root/regress.py` `/root/explore.py` | 自测脚本 |
| `/etc/systemd/system/voice-gateway.service` 等 | 三个 unit |

`bp.sh` `bv.sh` `te.sh` `tk.sh` `vs.sh` 是**短名入口，现在是软链接**，全指向正本：

```bash
bp.sh -> bench_params.sh      bv.sh -> bench_voice.sh      te.sh -> test_e2e.sh
tk.sh -> test_kws.sh          vs.sh -> voice_start.sh
```

> 2026-10-31 之前它们是**实体副本**，各自漂移过。其中 `vs.sh` 停在 10-29 版，
> 缺 `amixer -c 2 cset numid=16 on`——用它启动网关**完全没声音，日志却显示播放完成**，
> 正好掉回"假成功"那个坑。`tk.sh` 停在旧唤醒参数（`score=1.0/thr=0.25`，现为 `3.0/0.10`），
> 用它测唤醒率会得到偏低的错误结论。改成软链接后就再也漂不了。
> 旧副本留档在板上 `/root/archive/2026-10-31-dedup/dup-scripts/`。

对比源码时以 `/root/*.cpp` 和 `/root/*.py` 为准。

## 二之半、`/root/archive/` 什么时候看

板上 `/root/archive/` 是**移走而不是删除**的归档区，每个子目录有 `MANIFEST.md` 说明
移了什么、为什么移、怎么还原。

| 目录 | 内容 |
| :--- | :--- |
| `2026-10-31-dedup/old-agent-src/` | 4 份旧版 Agent 源码（含和正本 md5 相同的 `agent_new.cpp` 残留） |
| `2026-10-31-dedup/dup-scripts/` | 4 组 `xxx.sh`/`xxxr.sh` 重复对 + `vgw.cpp` + `.bak` + 两份过期短名 |

判断标准：**只移"多份同名、改了不生效"的东西**。历史实验草稿（`_p0_*` `_p6_*` `bench_*`）
不搬，因为 7 份文档用路径引用了它们（如 `lab/bench_cam_fps.cpp`），搬走就得改文档，
收益不抵成本。

## 三、改了代码怎么上线

```bash
cd edgevision-rk3588/lab
./deploy.sh agent            # 同步 + 编译，不重启
./deploy.sh agent --restart  # 同步 + 编译 + 重启语音网关（会等 70s 就绪）
./deploy.sh gateway          # 语音网关同理
```

`deploy.sh` 会**先编译再替换**：编译失败就保留旧二进制，不会把板子弄成不能跑。

手动等价步骤（脚本就是干这个）：

```bash
scp rkllm_embodied_agent.cpp rk3588:/root/rkllm_deploy/agent_new.cpp
ssh rk3588 'cd /root/rkllm_deploy && tr -d "\r" < agent_new.cpp > rkllm_embodied_agent.cpp \
  && g++ -O2 rkllm_embodied_agent.cpp -L. -lrkllmrt -Wl,-rpath=. -o .new \
  && mv -f .new rkllm_embodied_agent_p0'
```

**为什么要 `tr -d "\r"`**：Windows 行尾的 `\r` 会让 g++ 报 `stray '\r'`。脚本里已处理。

## 四、改完必须做的两件事

```bash
python3 /root/regress.py     # 20 条已知行为，必须全绿
python3 /root/explore.py     # 15 项边界探测，看有没有新发现
```

两个脚本都会**自己停网关腾出 NPU、跑完自动拉起**，全程约 2~3 分钟，不需要人参与。

## 五、源码与部署的漂移检查

```bash
# 本地算规范化 md5（去掉 \r），板上直接算
for f in rkllm_embodied_agent.cpp voice_gateway.cpp regress.py explore.py; do
  printf "%-28s %s\n" "$f" "$(tr -d '\r' < $f | md5sum | cut -c1-8)"
done
ssh rk3588 'md5sum /root/rkllm_deploy/rkllm_embodied_agent.cpp /root/voice_gateway.cpp'
```

md5 不一致 = 有人在板上直接改了代码，或者本地改了没部署。**以本地为准**。
