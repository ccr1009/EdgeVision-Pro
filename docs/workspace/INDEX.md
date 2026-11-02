# docs/workspace/ 是什么 —— 索引

**工作区根目录 4 份文档的镜像副本 + 根目录 `_tools/` 的镜像。**

> ⚠️ **为什么要有这个目录（2026-11-02）**
>
> 工作区根目录 `D:\a研究生\项目\rk3588项目\` **不是 git 仓库**，
> 所以根目录下的文档**从来没进过任何版本控制**。
> 其中 `AGENTS.md` 有 **65 KB**，是整个项目的总纲（12 周路线 + 全部踩坑 +
> 协作偏好），它自己写着「权威版本在工作区根目录」——**而它本身没有任何备份**。
> 硬盘坏了就没了。
>
> 这里放的是**只读镜像**，用于备份。**改文档还是改根目录那份。**

---

## 一、镜像清单

| 文件 | 大小 | 是什么 | 正本位置 |
| :--- | :--- | :--- | :--- |
| `AGENTS.md` | 65 KB | **项目总纲（权威版）**：我是谁 / 硬件环境 / 关键坑 / 已完成的事 / 12 周路线 / 协作偏好 | 根目录（**改这份**） |
| `README.md` | 11.6 KB | 工作区使用说明：目录地图 / 五分钟上手 / 端口表 / 四条硬约束 | 根目录（**改这份**） |
| `run.md` | 0.7 KB | **手动速查**：启动看板、重新编译、跑 Agent、语音启动的原始命令 | 根目录（**改这份**） |
| `TROUBLESHOOTING.md` | 1.0 KB | 指路文件，正文在 `docs/TROUBLESHOOTING.md` | 根目录（**改这份**） |

---

## 二、和仓库内同名文件的关系（**别搞混**）

仓库里另有两个同名文件，**内容完全不同，不是重复**：

| 文件 | 区别 |
| :--- | :--- |
| `docs/workspace/AGENTS.md`（65 KB）vs 仓库根 `AGENTS.md`（6.3 KB） | 前者是**权威全量版**，后者是**精简副本**，只留开发时立刻要知道的。精简版开头自己写着「权威版本在工作区根目录」。 |
| `docs/workspace/README.md`（11.6 KB）vs 仓库根 `README.md`（20.3 KB） | 前者是**工作区**说明（讲根目录各子目录干什么、怎么连板子），后者是**仓库**门面（讲项目亮点、架构、基准数字）。两个不同的东西。 |

> **一句话**：`docs/workspace/` 讲「这个工作区怎么用」，仓库根的讲「这个项目是什么」。

---

## 三、根目录里还有什么没入库（现状）

| 条目 | 大小 | 处理 |
| :--- | :--- | :--- |
| `_tools/` | 45 文件 | ✅ 已脱敏后镜像到仓库 `tools/`（见第五、六节） |
| `_workbench/asr/` | 837 MB | ❌ **不入库**：ASR/TTS 模型，可重新下载 |
| `_workbench/stereo3d-rk3588-archive/` | 43 MB | ❌ 不入库：已在 `stereo3d/` 归一，这里只是标定原图 |
| `_workbench/session_logs/` | 0.1 MB ×2 | ❌ **永不入库**：含手机号等隐私 |
| `_workbench/test_results/` | 88 文件 | ❌ 不入库：回归产物，可由 `regress.py` 重新生成 |
| `_workbench/audio_probe/` | 2 文件 | ❌ 不入库：探针产物 |
| `rkllm_deploy/` | 7 MB | ⚠️ 只有 `librkllmrt.so` + `rkllm.h` + demo，**厂商产物**，可从 SDK 重新获取 |
| `舵机/` | 9 MB | ⚠️ SG90 组装教程 docx + 商家文件，非代码 |
| `rk3588开发板配套资料/` | 27.5 GB | ❌ 厂商 SDK/镜像，已备份成 `.rar` |
| `linux-health-lab/` | 17 文件 | ⚠️ **独立学习工程，未入库**（自带 README/Makefile/tests） |

> `_workbench` 已确认无「只在板端」的源码——2026-11-02 抢救时核对过：
> 板端 140 个源码文件中 102 个是孤本，已全部落下，`_workbench` 判重 0 命中。

---

## 四、`tools/` 是什么（对应根目录 `_tools/`）

45 个辅助脚本的镜像，**已脱敏**。

脱敏处理：
- 内网 IP（形如 `10.xx.xx.xx`）→ 改为读环境变量 `BOARD_IP`
- 板子 root 密码 → 改为读环境变量 `BOARD_PASSWORD`（无默认值）

**用之前先设环境变量**：

```bash
export BOARD_IP=<板子IP>
export BOARD_PASSWORD=<板子密码>   # 仅 setup_board_ssh.py 需要
```

> **镜像里的 `AGENTS.md` 也做了脱敏**：正本里写着板子 IP 与 root 密码
> （正本在本地工作区，不公开，无所谓）。镜像要进公开仓库，所以把这
> 两类信息都换成了 `<板子IP>` / `<板子密码>`。**正本不动**。

其余脚本和 `_tools/` 里的**逐字节相同**——它们的价值是证据链和排查方法，
不是通用工具，见 `_tools/README.md` 的分类说明。

> **实测核对**：45 个文件中 **43 个逐字节相同**，
> 只有 `setup_board_ssh.py` 和 `test_d20_from_pc.py` 两个因脱敏而改动。
> 改后两个脚本都通过了 Python 语法检查。

---

## 五、恢复方法

硬盘丢了要还原工作区：

```bash
# 1. 克隆仓库
git clone git@github.com:ccr1009/EdgeVision-Pro.git edgevision-rk3588

# 2. 把镜像复制回根目录（如果要重建完整工作区）
cp edgevision-rk3588/docs/workspace/*.md .
cp -r edgevision-rk3588/tools ./_tools

# 3. 板端代码：本仓库 lab/ learn/ 就是全部源码，按各 README 部署
```

**没有备份、需要重新获取的东西**：
- ASR/TTS 模型 → 见 `docs/tech_notes/VOICE_ASR_PLAN.md` 的下载地址
- `librkllmrt.so` + `rkllm.h` → RKLLM SDK
- `rk3588开发板配套资料/` → 你的 `.rar` 备份

---

## 六、这批镜像的校验方式

复制时做了规范化行尾（`\r\n` → `\n`），内容**逐文件比对过**：

```bash
# 检查是否与根目录同步
for f in README.md AGENTS.md run.md TROUBLESHOOTING.md; do
  diff <(tr -d '\r' < "../$f") <(tr -d '\r' < "$f") > /dev/null && echo "$f ✅" || echo "$f ⚠ 已漂移，需重新镜像"
done
```

> **⚠️ 末尾的 `TROUBLESHOOTING.md` 是 1 KB 的指路文件，不是正文**。
> 正文 2153 行在 `docs/TROUBLESHOOTING.md`（那份本来就在 git 里）。
