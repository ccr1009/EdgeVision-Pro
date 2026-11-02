# ESP32-S3 舵机云台 — RK3588 视觉追踪执行器

> **状态（2026-11-01 晚）**：
> - ✅ 固件已烧录到真实 ESP32-S3 N16R8（PC 上枚举为 COM5），硬件自检 5/5 通过
> - ✅ 闭环逻辑 18 项自测全绿（PC 与**板端 Python 3.9** 都跑过）
> - ✅ 端到端联调通过（模拟网关 + 真串口 + 真板子，pan 跨度 137°）
> - ✅ **板端环境已就绪**：`/root/esp32_gimbal/`，pyserial 3.5 复用 `/root/rknn_env`
> - ✅ **真实 9000 网关已验证**：帧格式 248B 正确，坐标口径实测确认（见 8.4）
> - ⏳ **唯一待办**：把 ESP32 的 USB 线从电脑改插到 RK3588 的 USB 口

---

## 为什么需要这颗 MCU（这是整个方案的立论）

翻了 `rk3588开发板配套资料` 里的规格书和使用手册，**EVM3588-B 主板没有引出 40Pin GPIO，
也没有任何 PWM 引脚**。规格书列出的全部接口：

> 2×RS232、2×RS485、4×ADC、4×buttons、1×RTC、1×TF、1×SIM、1×FAN、3×LED、
> MINI-PCIE、PCIE3.0×4、SATA、M.2、USB2.0×2、USB3.0×2、HDMI_TX×2、HDMI_RX、DP、
> MIPI_CSI×4、MIPI_DSI×2

资源图（规格书 P5）也只画了 RS485/RS232、ADC×4、Buttons、FAN、LED、MINI-PCIE 这些。

所以 RK3588 **无法直接产生 SG90 要的 50Hz PWM**。这不是将就，是正确分层：

| | 谁干 | 周期 | 为什么 |
|:---|:---|:---|:---|
| **快层** | ESP32-S3 LEDC 硬件 PWM | 50Hz 连续 | 硬件定时器零抖动。Linux 非实时内核里 `usleep` 会被调度器打断，抖动直接打到舵机齿轮 |
| **慢层** | RK3588 做检测/决策 | 秒级 | 跑 NPU + LLM |

这和项目里「巡检守护快层不加载 LLM」是同一个设计哲学：实时的事交给实时的东西。

**没有采用飞线到核心板 BTB 引脚的方案**：那是 1.8V/3.3V 混排的板对板连接器，
飞线破坏可靠性且无官方支持。USB 串口是标准接口，可维护。

---

## 一、架构

```
        ┌──────────────────────────────────────────────┐
        │  RK3588  EVM3588-B                           │
        │                                              │
        │  UVC 摄像头 ──► MPP硬解 ──► NPU推理 ──► 9000 │  248B 定长帧
        │                                        │     │
        │                         gimbal_tracker.py    │  ← 新增：追踪闭环
        │                        (读 BBox → 算角度)     │
        └────────────────────────┬─────────────────────┘
                                 │ USB (CDC-ACM)
                                 │ /dev/ttyACM0  115200
                                 ▼
        ┌──────────────────────────────────────────────┐
        │  ESP32-S3 N16R8                              │
        │   LEDC 硬件 PWM 50Hz ──► Pan 舵机 (GPIO4)    │
        │                       └► Tilt 舵机 (GPIO5)   │
        └──────────────────────────────────────────────┘
```

---

## 二、目录

| 路径 | 作用 |
|:---|:---|
| `main/gimbal_main.c` | ESP-IDF 固件：LEDC PWM + 串口协议 + 软限位 + 缓动 |
| `CMakeLists.txt` / `main/CMakeLists.txt` / `sdkconfig.defaults` | IDF 工程 |
| `build.ps1` | Windows 构建/烧录脚本（已处理本机 IDF 环境问题，见第四节） |
| `host/gimbal_control.py` | **控制律与协议解析（纯函数）**，被 tracker 和测试共用 |
| `host/gimbal_probe.py` | 连通性与机械限位自检（**先跑这个**） |
| `host/gimbal_tracker.py` | 视觉追踪闭环主程序 |
| `host/fake_gateway.py` | 模拟 9000 网关（无板子时验证） |
| `host/test_loopback.py` | 闭环逻辑自测（18 项） |
| `host/e2e_check.py` | 端到端联调（假网关 + 真串口） |
| `host/results_*.txt` | 实测结果留档 |

---

## 三、接线

### 3.1 供电（最关键的这一步，别省）

SG90 堵转电流约 **1.4A/颗**，两颗同时堵转接近 3A。**绝对不要**从 ESP32 的 5V/3.3V 引脚取电
（N16R8 板载 LDO 撑不住，会掉压重启）。

```
  12V DC (板子套件自带的电源)
        │
        ├──► EVM3588-B DC_IN           (主板原样供电)
        │
        └──► LM2596S 降压模块 IN+
                  IN-  ──────────────┐
                                     │
             OUT+ ──► 5.0V 母线 ─────┼──► Pan  舵机 VCC (红)
                  (调到 5.0V)        └──► Tilt 舵机 VCC (红)

  ┌────────────────── 共地（GND）网络 ──────────────────┐
  │  LM2596S OUT-  ─┐                                  │
  │  Pan 舵机 GND   ─┤                                  │
  │  Tilt 舵机 GND  ─┼──► ESP32-S3 GND                 │
  │  ESP32-S3 GND  ─┘                                   │
  └─────────────────────────────────────────────────────┘
```

> ⚠️ **共地是死线**。PWM 是相对 GND 的电压，不共地则信号没有参考电平，
> 舵机表现为「乱抖 / 不动 / 单方向顶死」。这一条比任何代码问题都常见。

### 3.2 信号线

| 舵机 | 线色 | 接到 | 说明 |
|:---|:---|:---|:---|
| **Pan**（底部水平旋转） | 橙/黄 | ESP32 **GPIO4** | 3.3V 逻辑，SG90 兼容 |
| | 红 | 5V 母线 | 独立供电，不接 ESP32 |
| | 棕/黑 | GND | 共地 |
| **Tilt**（上部俯仰） | 橙/黄 | ESP32 **GPIO5** | |
| | 红 | 5V 母线 | |
| | 棕/黑 | GND | 共地 |

方向反了就改固件顶部 `PAN_INVERT` / `TILT_INVERT`（0/1），不用动逻辑。

### 3.3 ESP32 ↔ RK3588

ESP32-S3 N16R8 用**原生 USB-Serial-JTAG**，插上就枚举，不需要 CH340 驱动：

```
ESP32-S3 USB 口 ──USB线──► EVM3588-B 的 USB2.0 HOST 口
```

板子上会直接出现 **`/dev/ttyACM0`**。

> 已在 PC 上实测确认：设备 `USB\VID_303A&PID_1001` = Espressif 原生 USB-JTAG，枚举为 COM5。

### 3.4 摄像头装在哪

**摄像头必须固定在云台的俯仰平台上**（跟着 Tilt 一起动），这是闭环的前提。
摄像头不动 → 云台转多少画面都不变 → 闭环不可能成立。
用扎带或双面胶把 USB 摄像头绑到二自由度支架顶部的平面板上即可。

---

## 四、构建与烧录

### 4.1 本机环境（这里踩了四个坑，都已绕开）

你的机器上装的是 **ESP-IDF v6.0.1**（`C:\Users\15354\esp\esp-idf-v6.0.1`）。
直接用官方 `export.ps1` 会失败，`build.ps1` 已处理：

| 问题 | 现象 | 处理 |
|:---|:---|:---|
| IDF 找 `idf6.0_py3.12_env`，实际是 `py3.13_env` | `ESP-IDF Python virtual environment ... not found` | 直接设 `IDF_PYTHON_ENV_PATH`，不调 export.ps1 |
| `ESP_IDF_VERSION` 未设置 | `TypeError: expected string or bytes-like object, got 'NoneType'` | 手动设 `6.0.1` |
| `.espressif\tools` 层级不统一 | 找不到 `xtensa-esp32s3-elf-gcc` / `ninja` | 递归发现含 `.exe` 的目录加入 PATH |
| **工程路径含中文** | cmake 崩溃 `exit code 3221226505` | 复制到纯 ASCII 路径构建，产物再拷回 |

> 最后一个最有价值：cmake 在含中文的路径下直接段错误，而且报错信息完全看不出是路径问题。
> 以后遇到「cmake 无输出就崩」，先怀疑路径。

### 4.2 构建 / 烧录

```powershell
cd esp32_gimbal
pwsh -File .\build.ps1                    # 只编译
pwsh -File .\build.ps1 -Flash -Port COM5  # 编译 + 烧录
pwsh -File .\build.ps1 -Clean             # 清理重来
```

产物 `build\esp32_gimbal.bin`，实测 **188,384 字节**，分区剩余 82%。

### 4.3 RK3588 上烧录

板子上的 IDF 环境不存在时，用 esptool 直接烧：

```bash
# 先确认串口
ls -l /dev/ttyACM0

python3 -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0     build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/esp32_gimbal.bin
```

---

## 五、串口协议

**115200 8N1，文本行，`\n` 结尾，走原生 USB-Serial-JTAG。**

用文本不用二进制，是因为要能用串口助手手敲命令调机械限位——调限位时这一点非常省事。
**数据面（9000 推流）才需要 248 字节定长帧抗粘包**；同一项目里两种选择并存，
正好说明「协议设计服务于场景」。

| 方向 | 报文 | 说明 |
|:---|:---|:---|
| 主机→ESP32 | `P <pan> <tilt>\n` | 设置相对中位角度，如 `P -12.5 8.0` |
| 主机→ESP32 | `H\n` | 回中（pan=0, tilt=0） |
| 主机→ESP32 | `Q\n` | 查询状态 |
| 主机→ESP32 | `S <ms>\n` | 缓动时间常数（0=立即到位，最大 2000） |
| 主机→ESP32 | `L <0\|1>\n` | 板载 LED |
| ESP32→主机 | `OK <pan> <tilt>\n` | 当前实际角度 |
| ESP32→主机 | `S <pan> <tilt> <ready>\n` | 状态回复，ready=1 表示已到位 |
| ESP32→主机 | `ERR <code> <msg>\n` | `1`=out_of_range / `2`=bad_number / `3`=need_two_args / `9`=unknown_cmd |
| ESP32→主机 | `BOOT gimbal v1\n` | 上电横幅 |

### 角度定义

**相对中位的角度（度）**，中位 0 = 舵机 90° 绝对位置：

| 轴 | 范围 | 机械含义 |
|:---|:---|:---|
| Pan | **−90 ~ +90** | 负 = 往左，正 = 往右（从上往下看） |
| Tilt | **−45 ~ +45** | 负 = 低头，正 = 抬头 |

> Tilt 故意限 ±45° 而不是 ±90°：二自由度支架俯仰端在极限角会**撞到底座**，
> 硬顶 = 堵转 = 拉大电流 = 烧电机或扫齿。**软件限位防的是电气故障，不只是机械。**

超限时固件会**夹紧后执行**并回 `ERR 1 out_of_range`——不是静默接受，
避免主机以为请求被完整执行（这正是项目里反复出现的「假成功」模式）。

---

## 六、验证顺序（严格按顺序，每步过了再下一步）

跳过任何一步，出问题时你会分不清是接线还是代码。

### Step 1：烧固件（不接舵机）

串口应看到：
```
BOOT gimbal v1
READY pan=[-90,90] tilt=[-45,45]
```

### Step 2：不接舵机，只验证串口

```powershell
python gimbal_probe.py --port COM5 --no-move
```

### Step 3：接一颗舵机（先只接 Pan）

```powershell
python gimbal_probe.py --port COM5
```
应看到 6 次小角度扫动全部 OK、2 次超限被拦截、最后回中。

### Step 4：闭环逻辑自测（不需要硬件）

```powershell
python test_loopback.py
```
18 项全绿。

### Step 5：端到端联调（假网关 + 真串口）

```powershell
python e2e_check.py COM5 15
```

### Step 6：上 RK3588，接真网关

```bash
systemctl status edgevision          # 确认 9000 在推流
python3 gimbal_probe.py --port /dev/ttyACM0
python3 gimbal_tracker.py --port /dev/ttyACM0 --class person
```

---

## 七、调参

| 参数 | 默认 | 症状与调整 |
|:---|:---|:---|
| `--kp-pan` | 0.45 | 来回抽 → 调小；跟不上 → 调大 |
| `--kp-tilt` | 0.40 | 同上 |
| `--fov-pan` | 60.0 | 摄像头水平视场角。不确定就实测：让目标横穿画面，看云台转的角度 |
| `--fov-tilt` | 36.0 | 垂直视场角 |
| `--deadzone` | 0.05 | 画面中心 ±5% 不动，防静止目标抽搐 |
| `--max-step` | 6.0 | 单帧最大角度变化，防目标突跳时猛甩 |
| `--lost-timeout` | 1.5 | 丢失多久后停住（**不会乱找**） |

**调参顺序**：先 `--kp` 调到「能跟上但不振荡」，再调小 `--deadzone` 提精度。先 Pan 后 Tilt。

⚠️ **不要加积分项（I）**。舵机行程有界，积分累积会导致超调后长时间回不来，
表现为「转过头再慢慢飘回来」。纯 P + 死区 + 限速够用且可控。

---

## 八、实测结果

### 8.1 固件自检（真实硬件 ESP32-S3 N16R8 @ COM5）

```
[1/5] 链路正常，S 0.0 0.0 1
[2/5] Q 查询 OK
[3/5] 小角度扫动 6/6 OK（pan ±15°、tilt ±10°）
[4/5] 超限拦截 2/2：pan 200 → ERR 1 out_of_range；tilt 120 → ERR 1 out_of_range
[5/5] 回中 OK
全部通过 ✅
```

### 8.2 闭环逻辑自测：18/18

含协议往返、畸形帧丢弃、目标选择、符号方向、死区、限速、收敛仿真。
详见 `host/results_test_loopback.txt`。

**收敛实测**（摄像头随云台动的物理模型，目标世界角度 25°）：
```
轨迹：0.0 → 6.0 → 12.0 → 17.9 → 21.1 → 22.8   6 步收敛
最终误差 2.16°（死区对应 3.00°）
```

### 8.3 端到端联调（假网关 20fps + 真串口 + 真板子，15 秒）

```
pan 指令 55 条，范围 [-82.90, 54.10]，跨度 137.00°
不同 pan 取值 49 个
串口错误 0 次
✅ 通过
```
详见 `host/results_e2e.txt`。

### 8.4 真实 9000 网关核对（2026-11-01 上板实测）

`systemctl start edgevision` 后，3 核 NPU 硬绑定就绪，实测 **21.5 ms/帧**，
9000 端口正常推流。用 `check_gateway.py` 取 10 帧：

```
成功帧 10 / 失败 0，不同 frame_id 10 个（持续推流）
[03] frame_id=1977  n=1  vase 0.29 (942,4,337x393)
[04] frame_id=1980  n=1  vase 0.30 (944,4,335x710)
坐标范围：x 942~944，w 335~337，x+w 最大 1279，y+h 最大 714
```

**口径结论（重要）**：网关推的是**原始推流分辨率坐标系**（1280×720），
不是 640×640 网络输入坐标——说明网关已经做了 letterbox 逆变换。
`v4l2-ctl` 确认 `/dev/video81` 支持 1280×720 MJPG，与此吻合。

所以 `gimbal_tracker.py` 的默认 `--img-w 1280 --img-h 720` **是对的，不用改**。

> 这次画面里没有真实目标：`vase 0.29` 是空画面误检，特征很明显—
> 置信度低（0.29）、框高在 392 和 710 之间乱跳。**做追踪演示前要让摄像头对着真目标。**

### 8.5 板端环境（2026-11-01）

板子是 Debian 11 + **Python 3.9.2**，系统环境**没有 pip**。处理方式：
复用项目已有的 `/root/rknn_env`（里面有 pip 24.3.1），装 pyserial 3.5。

不要另起 venv 再去手工解压 pip wheel —— 那是本项目之前踩过的坑
（见 `docs/TROUBLESHOOTING.md` 里 rknn 环境那几条）。

---

## 九、故障排查

| 现象 | 根因 | 处理 |
|:---|:---|:---|
| `/dev/ttyACM0` / COM5 不存在 | `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` 没开 | `sdkconfig.defaults` 已配好，检查烧录是否成功 |
| 舵机乱抖 | **没共地** | 把 ESP32 GND 和舵机电源 GND 接一起 |
| 舵机不动，串口有 `OK` | 信号线接错 GPIO / 没共地 | 万用表量 GPIO4 对 GND 有无 50Hz 方波 |
| 舵机单方向顶死 | 方向反了或角度超限 | 改 `PAN_INVERT`；`Q` 看实际角度 |
| 转一下就重启 | 供电不足（从 ESP32 取电了） | 改独立 5V，见 3.1 |
| 抖动大、咔咔响 | PWM 频率/分辨率错 | 确认 50Hz + `LEDC_TIMER_14_BIT` |
| 追踪来回抽 | `kp` 太大 | 调小 `--kp-pan` |
| 追踪完全不走 | 摄像头没装在云台上 | 摄像头必须随 Tilt 动 |
| `cmake failed with exit code 3221226505` | **工程路径含中文** | 移到 ASCII 路径构建 |
| `TypeError: ... got 'NoneType'` | `ESP_IDF_VERSION` 未设 | 见 4.1 |

---

## 十、踩坑记录（面试可讲）

这些是这次真实撞上、且能讲出原理的：

1. **ESP32-S3 的 LEDC 在 50Hz 下最高只到 14 位**，写 `LEDC_TIMER_16_BIT` 直接编译不过。
   原理：分辨率上限 = log2(80MHz/50) ≈ 20.6，但 S3 定时器计数器位宽限制，
   50Hz 分频后实际最高 14 位。14 位 → 1.22us/级 → 约 0.11°/级，对 SG90（精度 ±1°）够用。
   **这属于「文档数字对，但你选的频率下拿不到」**——必须按实际工作点算，不能看规格书上限。

2. **cmake 在中文路径下静默崩溃**（`0xC0000409`），日志里一个字都不提路径。
   排查办法：把工程复制到 `C:\gimbal_build\` 立刻就好。这类「环境层失败伪装成代码错误」
   最容易浪费半天。

3. **测试和实现各写一份公式 = 两个都不可信**。第一版 `test_loopback.py` 自己抄了一份控制律，
   后来主程序把符号方向改对了、测试里那份没改，于是测试「全绿」但测的是**过期逻辑**。
   正解：把控制律抽成 `gimbal_control.py` 纯函数，测试 `import` 真实实现。
   这个和项目里「口径隔离」是同一类问题。

4. **符号方向必须只在一层定义**。追踪层只管语义（目标偏右 → pan 增大），
   物理安装反了由固件 `PAN_INVERT` 兜底。两层都改方向 = 一定会绕晕。

5. **`recv` 的所有错误都要收敛**。端到端测试第一次崩溃在 `ConnectionResetError` 没捕获——
   而「网关重启 / 客户端断开」在真实场景是常态。接收侧不该因对端断开就整个进程挂掉。

---

## 十一、当前未完成

- ⏳ **ESP32 未插到板子**（这是唯一阻塞项）：`lsusb` 没有 Espressif 设备，
  `/dev/ttyACM*` 不存在。把 ESP32 的 USB 线从电脑改插到 EVM3588-B 的 USB2.0 HOST 口即可。
- **真摄像头闭环未验**：8.3 的闭环验证用的是模拟网关（协议完全一致，
  且 8.4 已确认真网关口径一致）。等 ESP32 插上板子后跑一次即闭环。
- **未做**：多目标选择策略（当前按面积最大）、云台速度前馈、目标跟踪 ID 关联、
  PID 的 D 项（当前纯 P，行程有界场景够用）。

### 接上 ESP32 后的一键流程

```bash
ssh root@10.21.3.103
cd /root/esp32_gimbal
bash setup_board.sh          # 装依赖 + 自检 + 核对网关 + 云台连通性
bash setup_board.sh --run    # 上面全做 + 直接起追踪
```

`setup_board.sh` 会自动找 `/dev/ttyACM*` 或 `/dev/ttyUSB*`，
找不到会明确告诉你"还差把 ESP32 插上"。
