# Linux 应用层系统编程实战笔记 (第 1 周 D1 ~ D4)

> **设备平台**：RK3588 (EVM3588-B, Debian 11, 内核 5.10.110)  
> **实战环境**：板端 `/root/lab/`  
> **目标岗位**：大湾区（大疆/华为/地平线/汇川/比亚迪/机器人新贵）端侧 AI 部署 & 嵌入式 Linux 研发岗

---

## 目录
1. [Day 1: 文件描述符与 Sysfs 读取温度](#day-1-文件描述符与-sysfs-读取温度)
2. [Day 2: stat 与 lseek 测模型大小及 perror 排错](#day-2-stat-与-lseek-测模型大小及-perror-排错)
3. [Day 3: write 硬件控制与 ioctl 终端查询](#day-3-write-硬件控制与-ioctl-终端查询)
4. [Day 4: 多进程生命周期与僵尸进程实战](#day-4-多进程生命周期与僵尸进程实战)
5. [大厂面试高频核心考点与标准应答](#大厂面试高频核心考点与标准应答)

---

## Day 1: 文件描述符与 Sysfs 读取温度
* **核心代码**：`w1_e1_temp.cpp`
* **读取节点**：`/sys/class/thermal/thermal_zone0/temp`
* **系统调用**：`open()`, `read()`, `close()`
* **核心概念**：
  * **文件描述符（fd）**：Linux 内核为一个打开的文件分配的整型句柄（0=标准输入, 1=标准输出, 2=标准错误，其余从 3 开始分配）。
  * **Sysfs 虚拟文件系统**：内核把硬件状态抽象成文本文件暴露给用户层。读取温度节点返回的是毫摄氏度（例如 `38850` 代表 38.85 ℃）。

---

## Day 2: stat 与 lseek 测模型大小及 perror 排错
* **核心代码**：`w1_e2_filesize.cpp`
* **测试文件**：`/root/edgevision/data/models/yolov8n_rk3588_i8.rknn`（实测 `6526425` 字节，约 6.22 MB）
* **对比两种方式**：
  1. **`stat(path, &st)`（查户口/档案）**：
     * **优点**：不需要 `open()`，不分配 fd，直接读取文件系统 inode 元数据，速度极快。
     * **用法**：读取 `st.st_size`。
  2. **`lseek(fd, 0, SEEK_END)`（磁带游标尺）**：
     * **优点**：经典流式/嵌入式模型加载规范。
     * **底层机制**：将读写指针直接移动到文件末尾，返回当前位置相对于开头的总偏移字节数。
     * **⚠️ 踩坑关键**：若测完大小后要继续 `read()` 模型，**必须先调用 `lseek(fd, 0, SEEK_SET)` 归位磁头**，否则读指针停在末尾会直接返回 0 (EOF)。
* **错误诊断**：`perror("open failed")`
  * 底层捕获全局变量 `errno`（例如 `ENOENT`），自动翻译成人类可读的英文报错 `No such file or directory`。

---

## Day 3: write 硬件控制与 ioctl 终端查询
* **核心代码**：`w1_e3_led.cpp`, `w1_e3_ioctl.cpp`
* **控制节点**：
  * `/sys/class/leds/work/trigger`（设为 `none` 剥夺内核心跳控制权）
  * `/sys/class/leds/work/brightness`（写 `"1"` 点亮，写 `"0"` 熄灭）
* **系统调用 `write(fd, buf, count)`**：
  * 体验 Linux **“一切皆文件”** 哲学：控制板载物理 LED 与写普通文本无异。
  * 循环写入时，配合 `lseek(fd, 0, SEEK_SET)` 确保每次写入都覆盖文件起始位置。
* **系统调用 `ioctl(fd, request, arg)`**：
  * 为什么有 `read/write` 还需要 `ioctl`？
    * `read/write` 负责传输大批“流数据”；
    * `ioctl`（Input/Output Control）是外设控制的瑞士军刀，负责向硬件下发**特殊控制命令、配置参数或查询元数据**。
  * 实战案例：调用 `ioctl(STDOUT_FILENO, TIOCGWINSZ, &size)`，通过 SSH 伪终端（PTY）实时读取 Windows 终端的窗口行数与列数。

---

## Day 4: 多进程生命周期与僵尸进程实战
* **核心代码**：`w1_e4_process.cpp`, `w1_e4_zombie.cpp`
* **核心三剑客**：
  1. **`fork()` —— 影分身（调用一次，返回两次）**：
     * 父进程返回：子进程的 `PID`（> 0）；
     * 子进程返回：`0`；
     * 失败返回：`-1`。
  2. **`execvp()` —— 夺舍（变身新程序）**：
     * 彻底替换当前进程的代码段、数据段和堆栈，去执行新的可执行程序（如 `uname -m`）；
     * 执行成功后原代码不再返回，执行失败才返回 -1。
  3. **`waitpid(pid, &status, options)` —— 监护人（回收子进程）**：
     * 等待子进程退出，回收 PCB 资源；
     * 用宏 `WIFEXITED(status)` 判断是否正常退出；
     * 用宏 `WEXITSTATUS(status)` 提取子进程的退出码（如 0 或自定义状态码）。

### 僵尸进程（Zombie Process）实证实操
* **现象**：
  * 子进程调用 `exit(88)` 死亡；
  * 父进程 `sleep(10)` 故意不调用 `waitpid()`；
  * 在此期间运行 `ps -ef | grep w1_e4_zombie`，子进程显式挂起为：
    ```
    root  1939  1937  0 12:50 pts/0  00:00:00 [w1_e4_zombie] <defunct>
    ```
  * 10 秒后父进程醒来调用 `waitpid`，成功捕获退出码 `88`，僵尸进程立即被内核彻底销毁！

---

## Day 5: 目录遍历 dirent 与 /proc 进程扫描 (Mini-ps 实战)
* **核心代码**：`w1_e5_proc.cpp`
* **扫描目录**：`/proc` 虚拟文件系统
* **核心 API**：
  1. `DIR* dir = opendir("/proc")` —— 打开目录流；
  2. `struct dirent* entry = readdir(dir)` —— 顺序翻页读取条目，读到末尾返回 `NULL`；
  3. `closedir(dir)` —— 关闭目录流，释放内存。
* **数据结构 `struct dirent`**：
  * `entry->d_name`：文件名/文件夹名（如 `"1"`, `"1740"`, `"cpuinfo"`, `"."`, `".."`）；
  * `entry->d_type`：文件类型（`DT_DIR` 为目录，`DT_REG` 为普通文件）。
* **`/proc` 的底层本质**：
  * 纯内核在内存中实时维护的虚拟文件系统，不占外存（eMMC/TF卡）空间；
  * 每个活跃进程都对应一个 `/proc/<PID>/` 目录；
  * `/proc/<PID>/comm` 保存进程短名，`/proc/<PID>/cmdline` 保存启动时的完整命令行参数，`/proc/<PID>/status` 保存内存占用与线程状态。
* **实操战果**：
  * 成功过滤非数字条目，遍历出板上当前正在运行的 **247 个活跃进程**，彻底还原了 Linux `ps` 命令的底层实现原理！

---

## Day 6: 第 1 周综合大练兵 (全温区健康巡检与日志持久化)
* **核心代码**：`w1_e6_thermal_logger.cpp`
* **融会贯通点**：
  1. `opendir("/sys/class/thermal")` + `readdir`：遍历系统全部温区目录（D5 技能）；
  2. `open(..., O_RDONLY)` + `read`：读取 `type` 传感器名称与 `temp` 原始温度（D1 技能）；
  3. `open("thermal.log", O_WRONLY | O_CREAT | O_APPEND, 0644)`：用 `O_APPEND` 追加模式打开持久化日志文件，防止覆盖旧记录（D2/D3 技能）；
  4. `time()` + `strftime()`：格式化系统墙上时间；
  5. `snprintf()` + `write(log_fd, ...)`：规整写入日志文件，实现工业级传感数据持久化。
* **实操战果**：
  * 精准扫描出 RK3588 的全部 **7 大核心温区**：
    * `bigcore0-thermal` (大核集群0): 39.77 ℃
    * `bigcore1-thermal` (大核集群1): 39.77 ℃
    * `littlecore-thermal` (小核集群): 39.77 ℃
    * `soc-thermal` (片上系统主温区): 39.77 ℃
    * `gpu-thermal` (Mali GPU): 37.92 ℃
    * `npu-thermal` (6 TOPS NPU): 37.92 ℃
    * `center-thermal` (芯片中心温区): 38.85 ℃
  * 数据持久化存储进 `thermal.log`，硬件待机温度均在 40 ℃ 以下，健康状态优良！
  * **★ 标志着 Linux 应用层系统编程第 1 周全面大圆满！**

---

## 大厂面试高频核心考点与标准应答

### Q1：什么是僵尸进程？有什么危害？如何避免？
* **成因**：子进程已经终止退出，但父进程没有调用 `wait()` 或 `waitpid()` 读取其退出状态，导致子进程的进程控制块（PCB / PID）依然残留在内核进程表中，状态为 `Z`（`<defunct>`）。
* **危害**：Linux 系统的 PID 总量是有限的（`/proc/sys/kernel/pid_max`）。大量僵尸进程会占满系统 PID，导致新进程无法创建（报 `fork: Resource temporarily unavailable`）。
* **解决方案**：
  1. 父进程及时调用 `wait()` / `waitpid()` 回收；
  2. 捕获 `SIGCHLD` 信号：子进程终止时内核会向父进程发送 `SIGCHLD`，注册信号处理函数并在其中非阻塞调用 `waitpid(-1, &status, WNOHANG)`；
  3. 孤儿化收养：如果父进程先退出，子进程会被 1 号进程（`init` / `systemd`）收养，`systemd` 会自动清理僵尸。

### Q2：`fork()` 时父子进程之间如何共享内存？什么是写时复制（COW）？
* **回答**：`fork()` 时并不是立即把父进程的整块物理内存完整拷贝一份给子进程（那样太慢且浪费），而是采用了**写时复制（Copy-On-Write, COW）**技术。
* **机制**：
  * 父子进程初始时共享同一套物理内存页，但页表权限被全部标记为**只读（Read-Only）**；
  * 只有当父进程或子进程试图向某块内存执行“写入”时，CPU 触发缺页异常（Page Fault），内核才会在此时为写入方单独分配一个物理页并拷贝数据。这使得 `fork()` 紧接着调用 `exec()` 时开销极小。

### Q3：为什么需要 `stat` 和 `lseek` 两种测大小方式？
* **回答**：
  * `stat` 适用于**只获取文件元数据、不需要访问文件内容**的场景，无需 `open()`，不消耗文件描述符，性能最高；
  * `lseek` 适用于**已经打开了文件描述符、正准备进行数据读取或分块加载**的场景，可以直接计算动态读取流的末尾并配合内存分配。

### Q4：Linux 虚拟文件系统 /proc 的本质是什么？ps 命令是怎么实现的？
* **回答**：
  * `/proc` 是内核驻留在 RAM 中的伪文件系统（procfs），所有文件和内容都是内核在读取请求到达时即时计算生成的，用于暴露系统内核及进程实时的运行时状态；
  * `ps` 命令本质上就是利用 `opendir("/proc")` 遍历目录，筛选出所有纯数字形式的 PID 目录，读取其中的 `status`、`comm` 或 `cmdline` 文件并格式化排版打印。
