# PIPELINE — 数据流与线程模型

## 一帧的生命周期（单主循环串行，简单可测优先）

```
camL(81) ─┐
          ├─ grab/retrieve → L,R (1280×720 BGR)
camR(83) ─┘        │
                   ▼
            rectify()  remap×2（定点映射表 CV_16SC2+16UC1）
                   │    → rectL, rectR 全分辨率灰度
                   │    → procL_, procR_ 640×360 灰度（INTER_AREA 防混叠）
                   ▼
            computeDisparity()  SGBM 3WAY
                   │    → disp16 (CV_16S, ×16 定点)
                   ▼
            depthFromDisparity()  Z = fx·B/d
                   │    → depth (mm, 无效=0)
                   ▼
            Face3DLocator::detectAndLocate()
                   │    Haar 检测(640×360 灰度) → 框内深度中位数 → 反投影 (X,Y,Z)
                   ▼
            画图：cvtColor 灰度→BGR → 框/标签 → 2×2 拼图 + HUD → JPEG
                   ▼
            HttpPreview::publish(jpeg, stats_json)
                   ▼
        /stream 订阅者（条件变量 broadcast 唤醒，只发最新帧）
```

## 线程模型

| 线程 | 职责 | 阻塞点 |
|---|---|---|
| main | 采集→矫正→视差→人脸→画图→发布（串行） | V4L2 grab |
| http accept | 监听 8081 | accept() |
| http 每连接 | /stream 推流 / 单帧路由 | 条件变量 wait / send |

设计取舍：**观众是人级（1~5），所以线程-per-连接而非 epoll**；主循环串行而非流水线，
是为了分段计时口径干净（每段耗时可直接 A/B 对比）。

## 为什么几处实现长这样（面试追问点）

- **矫正输出灰度**：SGBM 只吃灰度；remap 双线性在灰度域与 BGR 域几何等价，省 3 倍访存。画图前才 cvtColor 回 BGR。
- **proc 缩放用 INTER_AREA**：缩小时 AREA = 像素面积平均，先低通再抽样，不会像双线性那样漏采样出摩尔纹——纹理混叠对块匹配是致命的。
- **视差定点 ×16（CV_16S）**：SGBM 原生输出格式，subpixel 精度 1/16 px，比 float 省 2 字节/像素。
- **深度中位数在视差域取**：`depth_median_mm` 与 `center_m.z` 永远是同一个数，不会出现「距离 1.83 但 z=1.87」的自相矛盾。
- **publish 背压**：新连接 `last_seq = seq_`（只发新帧不回放历史），杜绝刷新页面灌旧帧。
- **MSG_NOSIGNAL 而非全局忽略 SIGPIPE**：浏览器关标签页不再杀进程；逐调用控制只影响这一处。

## 摄像头时序

两颗 UVC 是独立 USB 通道，无硬件同步。压时差办法：先 `capL.grab()` 紧接 `capR.grab()`，
再各自 retrieve。实测 grab 间隔在毫秒级（V4L2 缓冲队列），对人脸级速度目标（<2 m/s 位移）足够；
真正的硬件级同步需要外接触发线或改用板载 MIPI 双通道，属后续可扩展项。
