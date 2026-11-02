/*
 * ESP32-S3 N16R8 — SG90 二自由度云台执行器
 * =========================================
 *
 * 角色：RK3588 的"实时执行器"。
 *
 *   为什么需要这颗 MCU：
 *     EVM3588-B 主板规格书的接口清单里没有 40Pin GPIO、也没有任何 PWM 引出
 *     （只有 2×RS232 / 2×RS485 / 4×ADC / 4×buttons / MINI-PCIE / PCIE / SATA / M.2 /
 *      USB×4 / HDMI / DP / MIPI-CSI×4 / MIPI-DSI×2 / TF / SIM / FAN / LED）。
 *     Linux 非实时内核也无法稳定产生 50Hz 波形——抖动直接打到舵机齿轮上。
 *     LEDC 是硬件定时器，零抖动。这和"巡检守护快层不加载 LLM"是同一个分层哲学。
 *
 * 串口：**UART0 (GPIO43 TX / GPIO44 RX) 115200 8N1，文本行协议**
 *
 *   为什么不用原生 USB-Serial-JTAG：
 *     EVM3588-B 内核没编 CONFIG_USB_ACM（实测 `# CONFIG_USB_ACM is not set`），
 *     而 ESP32-S3 原生 USB 是 CDC-ACM 类 → 板子能 lsusb 看到设备但建不出 ttyACM0。
 *     用 usbserial_generic 强绑能短暂出 ttyUSB，但该驱动是"测试用"，不发
 *     SET_LINE_CODING / SET_CONTROL_LINE_STATE，通信不稳定。
 *     板子内核内置了 CH341 / CP210x / FTDI，所以走外置 USB-转-TTL 模块最稳。
 *
 * 协议（115200 8N1，文本行，\n 结尾）：
 *   收: P <pan> <tilt>      设置相对中位角度（度）
 *       H                   回中
 *       Q                   查询状态
 *       S <ms>              缓动时间常数（0=立即到位）
 *       L <0|1>             板载 LED
 *       G <pin>             运行时切换 PAN 舵机 GPIO（不用重烧）
 *       T <pin>             运行时切换 TILT 舵机 GPIO
 *       X                   引脚扫描：挨个 GPIO 输出脉宽，看舵机哪个动
 *       I                   打印当前引脚配置
 *       N <pin>             GPIO 裸翻转 300ms 方波（万用表可量，诊断用）
 *       A                   停止翻转，引脚还给 LEDC
 *   发: OK <pan> <tilt>
 *       S <pan> <tilt> <ready>
 *       GPIN <pan> <tilt>
 *       SCAN <pin>          （扫描中，当前正在驱动这个引脚）
 *       TOGGLE <pin>        （开始/停止翻转诊断）
 *       ERR <code> <msg>
 *       BOOT gimbal v2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"   /* 让原生 USB 口也能下命令，不只是看日志 */
#include "esp_timer.h"                /* 回环自测打微秒时间戳 */
#include "esp_rom_gpio.h"       /* IDF 6.0：GPIO 矩阵 API 从 gpio_matrix_out 迁到这里 */
#include "soc/gpio_sig_map.h"

/* ============ 引脚（可用 G/T 命令运行时改，不用重烧）============
 *
 * 默认值按实际接线定：核心板左侧排针从下往上依次是
 *     GPIO14 / 5Vin / GND      ← PAN 舵机就插在这三个上
 *     GPIO15                   ← TILT 舵机（第二个，紧挨着）
 * 这种"信号+5V+GND 三针相邻"的排法对杜邦线最省事，不用跨半个板子。
 *
 * 注意：这不是"物理第 14 个针脚"，而是丝印 GPIO14。核心板排针只有 GPIO 丝印，
 * 没有物理序号，所以一定要按丝印认，别数格子。 */
#define PAN_PIN_DEFAULT    14      /* 左侧排针 GPIO14，紧挨 5Vin/GND */
#define TILT_PIN_DEFAULT   15      /* 左侧排针 GPIO15 */
static int g_pan_pin  = PAN_PIN_DEFAULT;
static int g_tilt_pin = TILT_PIN_DEFAULT;
#define STATUS_LED_GPIO    48      /* N16R8 板载 WS2812 */

#define PAN_LEDC_CH        LEDC_CHANNEL_0
#define TILT_LEDC_CH       LEDC_CHANNEL_1
#define LEDC_TIMER_SEL     LEDC_TIMER_0
#define LEDC_MODE_SEL      LEDC_LOW_SPEED_MODE

/* ESP32-S3 的 LEDC 在 50Hz 下最高 14 位。
 * 分辨率上限 = log2(80MHz/50) ≈ 20.6，但 S3 定时器计数器位宽 + 50Hz 分频后实际 14 位。
 * 写 16 位直接编译不过（LEDC_TIMER_16_BIT 不被 esp32s3 HAL 接受）。
 * 14 位 → 1.22us/级 → 约 0.11°/级，SG90 精度 ±1°，够用。 */
#define SERVO_FREQ_HZ      50
#define SERVO_RES_BITS     LEDC_TIMER_14_BIT
#define SERVO_RES_LEVELS   (1U << 14)
#define SERVO_PERIOD_US    20000
#define PULSE_MIN_US       500
#define PULSE_MID_US       1500
#define PULSE_MAX_US       2500

/* 相对中位的行程限制（度）。
 * Tilt 限 ±45 而不是 ±90：支架俯仰端在极限角会撞底座，
 * 硬顶 = 堵转 1.4A = 拉垮 5V → ESP32 复位。软件限位防的是电气故障。 */
#define PAN_RANGE_DEG      90.0f
#define TILT_RANGE_DEG     45.0f

#define LINE_BUF_MAX       160     /* 要容得下 "V 0 1 2 3 ... 48" 这种一次探多个脚的命令 */
#define CTRL_PERIOD_MS     20

/* 扫描时依次尝试的 GPIO。已排除：
 *   0/45/46 = strapping 引脚，上电电平影响启动
 *   19/20   = 原生 USB D-/D+
 *   43/44   = UART0 控制台（本固件正在用）
 *   26~37   = N16R8 的 Flash / Octal PSRAM 占用
 *   48      = 板载 WS2812
 * 剩下的都是相对安全的普通 IO。 */
static const int SCAN_PINS[] = {
    1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
    21, 38, 39, 40, 41, 42, 47
};
#define SCAN_PIN_COUNT (sizeof(SCAN_PINS) / sizeof(SCAN_PINS[0]))

/* ============ 运行时状态 ============ */
static float g_pan = 0.0f;
static float g_tilt = 0.0f;
static float g_target_pan = 0.0f;
static float g_target_tilt = 0.0f;
static uint32_t g_slew_ms = 0;

/* 扫描模式状态 */
static bool  g_scanning = false;
static int   g_scan_idx = 0;
static TickType_t g_scan_next = 0;
static float g_scan_phase = 0.0f;

/* GPIO 裸翻转诊断状态（N/A 命令） */
static bool  g_toggling      = false;
static int   g_toggle_pin    = -1;
static int   g_toggle_phase  = 0;
static TickType_t g_toggle_next = 0;

/* 三连发状态（W 命令）：同一路 LEDC 信号同时驱动多个相邻脚。
 * 舵机的 3 孔壳拆不开，只能整排套——用它一次性覆盖所有可能位置。 */
static int  g_wfan_pins[8] = {0};
static int  g_wfan_n       = 0;

/* 演示摆动状态（D 命令）。
 * 用来和"别人给的 Arduino 固件"做**公平对照**：
 * 那个 bin 的规格是 1000-2000us / 50Hz / 每边 4 秒（见固件内字符串
 * "GPIO15 SG90: 1000-2000 us, 50 Hz, 4 s each way"），但它写死了 GPIO15，
 * 而本板唯一能接舵机的三针是 GND/5Vin/GPIO14 → 那个 bin 在这块板子上用不了。
 * 于是把它的波形参数原样搬到 GPIO14 上：如果舵机在它上面能转，在这里也必须能转，
 * 这样就把"是不是我的 PWM 实现有问题"这个变量彻底消掉。 */
static bool  g_demo      = false;
static float g_demo_phase = 0.0f;

/* ============ 回环自测状态（R 命令）============
 * 没有示波器/万用表时，怎么证明 PWM 真的出来了？
 * 让 ESP32 自己量自己：把输出脚同时设成输入，用边沿中断 + esp_timer 微秒时间戳
 * 记下每个上升沿/下降沿，算出脉宽和周期。量出来的就是 pad 上真实的电平。 */
static volatile int      g_lb_pin      = -1;
static volatile bool     g_lb_active   = false;
static volatile uint32_t g_lb_edges    = 0;
static volatile int64_t  g_lb_rise_ts  = 0;
static volatile int64_t  g_lb_prev_rise = 0;
static volatile uint32_t g_lb_high_lvl = 0;
static volatile uint32_t g_lb_per_sum  = 0;
static volatile uint32_t g_lb_per_cnt  = 0;
static volatile uint32_t g_lb_pw_sum   = 0;
static volatile uint32_t g_lb_pw_cnt   = 0;
static volatile uint32_t g_lb_pw_min   = 0xFFFFFFFFu;
static volatile uint32_t g_lb_pw_max   = 0;

/* ============ 角度 → duty ============ */
static uint32_t us_to_duty(uint32_t pulse_us)
{
    return (uint32_t)(((uint64_t)pulse_us * SERVO_RES_LEVELS) / SERVO_PERIOD_US);
}

static uint32_t angle_to_duty(float deg, float range)
{
    float norm = deg / range;
    if (norm > 1.0f)  norm = 1.0f;
    if (norm < -1.0f) norm = -1.0f;

    uint32_t mid  = us_to_duty(PULSE_MID_US);
    int32_t  half = (int32_t)us_to_duty((PULSE_MAX_US - PULSE_MIN_US) / 2 + PULSE_MID_US)
                    - (int32_t)mid;
    int32_t  duty = (int32_t)mid + (int32_t)(norm * (float)half);

    int32_t lo = (int32_t)us_to_duty(PULSE_MIN_US);
    int32_t hi = (int32_t)us_to_duty(PULSE_MAX_US);
    if (duty < lo) duty = lo;
    if (duty > hi) duty = hi;
    return (uint32_t)duty;
}

/* 把 LEDC 信号路由到指定 GPIO。
 * IDF 6.0 把 GPIO 矩阵 API 从 gpio_matrix_out() 迁到了 esp_rom_gpio_connect_out_signal()。
 * 用矩阵路由而不是重配 channel —— 这样切换引脚时不用停定时器，PWM 连续。
 *
 * ★★★ 这里有一个坑，曾经让整个项目卡了好几天，务必看明白 ★★★
 *
 * 第一版代码是这么写的：
 *     esp_rom_gpio_connect_out_signal(pin, sig, false, false);
 *     gpio_set_direction(pin, GPIO_MODE_OUTPUT);
 * 看起来完全合理——先把信号接到引脚，再把引脚设成输出。
 * 但实际结果是**引脚上一点波形都没有**（回环自测 edges=0），舵机一动不动。
 *
 * 根因（去看 esp_driver_gpio/src/gpio.c 的实现）：
 *     gpio_set_direction(mode & OUTPUT) -> gpio_output_enable() ->
 *         gpio_hal_matrix_out_default(gpio_num);   // ← 把矩阵重新指向"普通 GPIO 输出"
 *         gpio_hal_output_enable(...);
 *         gpio_hal_func_sel(..., PIN_FUNC_GPIO);
 * 也就是说 **gpio_set_direction 会把刚配好的外设路由抹掉**，引脚退回成
 * 由 GPIO_OUT 寄存器控制的普通输出——而那个寄存器是 0，于是恒低电平。
 *
 * 正确顺序是**先设方向、后接信号**：让 matrix_out_default 先跑完，
 * 再由 esp_rom_gpio_connect_out_signal 把矩阵指向 LEDC。这个顺序不能反。 */
static void route_ledc_to_gpio(int ch, int pin)
{
    /* LEDC_LS_SIG_OUT0_IDX 是低速通道 0 的输出信号索引 */
    int sig = LEDC_LS_SIG_OUT0_IDX + ch;
    if (pin >= 0 && pin <= 48) {
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);           /* ① 先使能输出驱动 */
        /* ② 把驱动能力拉到最大档（40mA，默认只有 20mA）。
         *
         * 为什么需要：SG90 这类廉价舵机，信号脚内部是 串联电阻 + 三极管/MCU 输入，
         * 而它的 VDD 是 5V。很多克隆板的输入判决门限按 0.7×VDD 设计 = 3.5V，
         * 高于 ESP32 的 3.3V 输出电平 → 舵机"看到"的信号永远是低，表现就是完全不认。
         * 拉满驱动能力不改变电压，但能让边沿更陡、抗住信号脚那点半导体的负载，
         * 是成本最低的一次尝试。真不行就得加电平转换/三极管缓冲。 */
        gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
        esp_rom_gpio_connect_out_signal(pin, sig, false, false); /* ③ 再接 LEDC 信号 */
    }
}

/* 断开某个 GPIO 的矩阵输出（恢复成普通 GPIO 输出） */
static void unroute_gpio(int pin)
{
    if (pin >= 0 && pin <= 48) {
        esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    }
}

static void apply_servos(void)
{
    ledc_set_duty(LEDC_MODE_SEL, PAN_LEDC_CH,  angle_to_duty(g_pan,  PAN_RANGE_DEG));
    ledc_update_duty(LEDC_MODE_SEL, PAN_LEDC_CH);
    ledc_set_duty(LEDC_MODE_SEL, TILT_LEDC_CH, angle_to_duty(g_tilt, TILT_RANGE_DEG));
    ledc_update_duty(LEDC_MODE_SEL, TILT_LEDC_CH);
}

/* 前置声明：回环自测 / 电平探测(都定义在 out_line 之前)要用到 */
static void out_line(const char *fmt, ...);
static void send_err(int code, const char *msg);

/* ============ 回环自测 ============
 * 没有示波器怎么证明 PWM 真的出到了引脚上？
 * 让 ESP32 量自己：引脚同时开输出(供 LEDC 驱动)和输入(读回 pad 真实电平)，
 * 用任意边沿中断 + esp_timer 微秒时间戳记录每个沿，算出脉宽和周期。
 *
 * 这一步的价值在于把「舵机不动」的两种可能彻底分开：
 *   R 报出 pw≈1500us per≈20000us  → 信号确实出到 pad 了，问题在舵机/线/供电
 *   R 报 0 个沿                    → 引脚根本没有波形，问题在固件/引脚配置 */
static void IRAM_ATTR lb_isr(void *arg)
{
    (void)arg;
    if (!g_lb_active) return;

    int64_t now = esp_timer_get_time();
    int level = gpio_get_level((gpio_num_t)g_lb_pin);

    g_lb_edges++;

    if (level) {                        /* 上升沿 */
        g_lb_high_lvl++;
        if (g_lb_prev_rise != 0) {
            uint32_t per = (uint32_t)(now - g_lb_prev_rise);
            g_lb_per_sum += per;
            g_lb_per_cnt++;
        }
        g_lb_prev_rise = now;
        g_lb_rise_ts   = now;
    } else {                            /* 下降沿 */
        if (g_lb_rise_ts != 0) {
            uint32_t pw = (uint32_t)(now - g_lb_rise_ts);
            if (pw < 100000) {          /* 过滤异常值 */
                g_lb_pw_sum += pw;
                g_lb_pw_cnt++;
                if (pw < g_lb_pw_min) g_lb_pw_min = pw;
                if (pw > g_lb_pw_max) g_lb_pw_max = pw;
            }
        }
    }
}

static void lb_start(int pin)
{
    g_lb_active = false;
    g_lb_edges = 0;
    g_lb_high_lvl = 0;
    g_lb_per_sum = g_lb_per_cnt = 0;
    g_lb_pw_sum = g_lb_pw_cnt = 0;
    g_lb_pw_min = 0xFFFFFFFFu;
    g_lb_pw_max = 0;
    g_lb_prev_rise = 0;
    g_lb_rise_ts = 0;
    g_lb_pin = pin;

    /* 关键：只**开输入**，绝不能再调 gpio_set_direction(GPIO_MODE_INPUT_OUTPUT)。
     * 因为 gpio_set_direction 里带 OUTPUT 位 -> gpio_output_enable() ->
     * gpio_hal_matrix_out_default() 会把 LEDC 的矩阵路由抹掉，测出来永远是 0 个沿。
     * gpio_input_enable() 只写 IO_MUX 的输入使能，不碰矩阵，安全。 */
    gpio_input_enable((gpio_num_t)pin);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE);

    gpio_install_isr_service(0);        /* 重复调用返回 ESP_ERR_INVALID_STATE，忽略即可 */
    gpio_isr_handler_add(pin, lb_isr, NULL);

    g_lb_active = true;
}

static void lb_stop(void)
{
    g_lb_active = false;
    if (g_lb_pin >= 0) {
        gpio_isr_handler_remove((gpio_num_t)g_lb_pin);
        gpio_set_intr_type((gpio_num_t)g_lb_pin, GPIO_INTR_DISABLE);
    }
}

static void lb_report(void)
{
    uint32_t edges = g_lb_edges;
    uint32_t pwc = g_lb_pw_cnt, perc = g_lb_per_cnt;

    if (edges == 0) {
        out_line("LB pin=%d edges=0 NO_SIGNAL\n", (int)g_lb_pin);
        return;
    }

    uint32_t pw_avg = pwc ? (g_lb_pw_sum / pwc) : 0;
    uint32_t per_avg = perc ? (g_lb_per_sum / perc) : 0;
    uint32_t pw_min = (g_lb_pw_min == 0xFFFFFFFFu) ? 0 : g_lb_pw_min;

    out_line("LB pin=%d edges=%u high=%u pw_avg=%uus pw_min=%uus pw_max=%uus per_avg=%uus n=%u\n",
             (int)g_lb_pin, (unsigned)edges, (unsigned)g_lb_high_lvl,
             (unsigned)pw_avg, (unsigned)pw_min, (unsigned)g_lb_pw_max,
             (unsigned)per_avg, (unsigned)pwc);
}

/* 判断某个 GPIO 能不能拿来做舵机/探测。排除的类比 SCAN_PINS 更严格：
 *   0/45/46  strapping，上电电平影响启动
 *   19/20    ★ 原生 USB D-/D+ —— 碰一下 USB 就掉线（实测过：扫描 19/20 后
 *              PC 上 COM 口直接消失，报 ClearCommError / 找不到文件）
 *   43/44    UART0 控制台
 *   26~37    N16R8 的 Flash / Octal PSRAM
 *   48       板载 WS2812
 * 这个函数被 V(探测) 和 X(扫描) 共用，避免两处列表不同步又踩同一个坑。 */
static bool is_user_gpio(int pin)
{
    if (pin < 0 || pin > 48) return false;
    if (pin == 0 || pin == 45 || pin == 46) return false;
    if (pin == 19 || pin == 20) return false;
    if (pin == 43 || pin == 44) return false;
    if (pin >= 26 && pin <= 37) return false;
    if (pin == STATUS_LED_GPIO) return false;
    return true;
}

/* ============ 引脚电平探测 ============
 * 舵机是个 3 孔塑料壳，必须整体套在相邻三个排针上。问题在于：
 *   ① 壳可能反着插（橙线跑到 GND 那头）
 *   ② 壳可能整体偏移一格，根本没套在预期的那三个针上
 *   ③ 壳里的金属簧片夹不紧排针（虚接）
 * 这三种情况外观上一模一样，都表现为"舵机全软"。
 *
 * 这函数用内部上拉/下拉各读一次电平，能区分：
 *   上拉读到 0、下拉读到 0  → 有较强下拉，说明**确实有东西接着**（舵机信号脚内部
 *                              通常带 10k 下拉），接上了但没波形 → 查壳的位置/方向
 *   上拉读到 1、下拉读到 0  → 悬空，**那个针上什么都没接**
 *   上拉读到 1、下拉读到 1  → 被拉死在高电平，可能接到了 5Vin/3V3（危险，赶紧拔）
 *
 * 注意：探测前必须先把 LEDC 路由摘掉，否则读到的是自己驱动的输出电平。 */
static void sense_pin(int pin)
{
    if (!is_user_gpio(pin)) { send_err(4, "bad_pin"); return; }

    bool was_pan  = (pin == g_pan_pin);
    bool was_tilt = (pin == g_tilt_pin);

    if (was_pan)  unroute_gpio(g_pan_pin);
    if (was_tilt) unroute_gpio(g_tilt_pin);

    gpio_set_direction(pin, GPIO_MODE_INPUT);

    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(10));
    int pu = gpio_get_level(pin);

    gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(10));
    int pd = gpio_get_level(pin);

    gpio_set_pull_mode(pin, GPIO_FLOATING);

    /* 判读 */
    const char *verdict;
    if (pu == 0 && pd == 0)      verdict = "CONNECTED";   /* 有外部下拉/接负载 */
    else if (pu == 1 && pd == 0) verdict = "FLOATING";    /* 没接东西 */
    else if (pu == 1 && pd == 1) verdict = "HIGH_FIXED";  /* 接到电源了 */
    else                         verdict = "PULLUP_ONLY"; /* 外部上拉 */

    out_line("VPIN %d pu=%d pd=%d %s\n", pin, pu, pd, verdict);

    if (was_pan)  route_ledc_to_gpio(PAN_LEDC_CH,  g_pan_pin);
    if (was_tilt) route_ledc_to_gpio(TILT_LEDC_CH, g_tilt_pin);
}

/* ============ 演示摆动（D 命令）============
 * 原样复刻 sg90_gpio15_4s_merged.bin 的波形：1000~2000us、50Hz、每边 4 秒。
 * 不经过角度映射，直接把脉宽写进 LEDC，排除"角度换算写错了"这类可能。 */
static void update_demo(uint32_t dt_ms)
{
    if (!g_demo) return;

    const uint32_t period_ms = 4000;    /* 每边 4 秒 */
    g_demo_phase += (float)dt_ms / (float)period_ms;
    while (g_demo_phase >= 1.0f) g_demo_phase -= 1.0f;

    /* 三角波 0→1→0，对应 1000us→2000us→1000us */
    float t = (g_demo_phase < 0.5f) ? (g_demo_phase * 2.0f)
                                    : (2.0f - g_demo_phase * 2.0f);
    uint32_t us = 1000u + (uint32_t)(t * 1000.0f);

    uint32_t duty = (uint32_t)(((uint64_t)us * SERVO_RES_LEVELS) / SERVO_PERIOD_US);
    ledc_set_duty(LEDC_MODE_SEL, PAN_LEDC_CH, duty);
    ledc_update_duty(LEDC_MODE_SEL, PAN_LEDC_CH);
}

/* ============ 输出 ============ */
static void out_line(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
}

static void send_ok(void)     { out_line("OK %.1f %.1f\n", g_pan, g_tilt); }
static void send_err(int code, const char *msg) { out_line("ERR %d %s\n", code, msg); }

static void send_status(void)
{
    bool ready = (fabsf(g_pan - g_target_pan) < 0.6f) &&
                 (fabsf(g_tilt - g_target_tilt) < 0.6f);
    out_line("S %.1f %.1f %d\n", g_pan, g_tilt, ready ? 1 : 0);
}

static void send_pins(void)
{
    out_line("GPIN %d %d\n", g_pan_pin, g_tilt_pin);
}

/* ============ 解析 ============ */
static bool parse_float_strict(const char *s, float *out)
{
    if (s == NULL || *s == '\0') return false;
    char *endp = NULL;
    float v = strtof(s, &endp);
    if (endp == s || *endp != '\0') return false;   /* 整串必须被消费 */
    if (!isfinite(v)) return false;
    *out = v;
    return true;
}

static void set_pan_pin(int pin)
{
    if (!is_user_gpio(pin)) { send_err(4, "bad_pin"); return; }
    unroute_gpio(g_pan_pin);
    g_pan_pin = pin;
    route_ledc_to_gpio(PAN_LEDC_CH, g_pan_pin);
    apply_servos();
    send_pins();
}

static void set_tilt_pin(int pin)
{
    if (!is_user_gpio(pin)) { send_err(4, "bad_pin"); return; }
    unroute_gpio(g_tilt_pin);
    g_tilt_pin = pin;
    route_ledc_to_gpio(TILT_LEDC_CH, g_tilt_pin);
    apply_servos();
    send_pins();
}

static void start_scan(void)
{
    /* 先把两个通道都停到中位，再从第 0 个候选脚开始 */
    unroute_gpio(g_pan_pin);
    g_pan = g_tilt = g_target_pan = g_target_tilt = 0.0f;
    apply_servos();

    g_scanning = true;
    g_scan_idx = 0;
    g_scan_phase = 0.0f;
    g_scan_next = xTaskGetTickCount();

    out_line("# SCAN START: 每 1.2s 换一个引脚，幅度 ±30°，注意看舵机\n");
    out_line("SCAN %d\n", SCAN_PINS[0]);
    g_pan_pin = SCAN_PINS[0];
    route_ledc_to_gpio(PAN_LEDC_CH, g_pan_pin);
}

static void handle_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    char cmd = line[0];
    char *rest = line + 1;
    while (*rest == ' ' || *rest == '\t') rest++;

    switch (cmd) {
    case 'P':
    case 'p': {
        char *sp = rest;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        if (*sp == '\0') { send_err(3, "need_two_args"); return; }
        *sp = '\0';
        char *tok2 = sp + 1;
        while (*tok2 == ' ' || *tok2 == '\t') tok2++;

        float pan = 0.0f, tilt = 0.0f;
        if (!parse_float_strict(rest, &pan) || !parse_float_strict(tok2, &tilt)) {
            send_err(2, "bad_number");
            return;
        }

        bool clamped = false;
        if (pan > PAN_RANGE_DEG)    { pan  = PAN_RANGE_DEG;   clamped = true; }
        if (pan < -PAN_RANGE_DEG)   { pan  = -PAN_RANGE_DEG;  clamped = true; }
        if (tilt > TILT_RANGE_DEG)  { tilt = TILT_RANGE_DEG;  clamped = true; }
        if (tilt < -TILT_RANGE_DEG) { tilt = -TILT_RANGE_DEG; clamped = true; }

        g_target_pan  = pan;
        g_target_tilt = tilt;

        if (g_slew_ms == 0) {
            g_pan  = pan;
            g_tilt = tilt;
            apply_servos();
        }
        if (clamped) send_err(1, "out_of_range");
        else         send_ok();
        return;
    }
    case 'H':
    case 'h':
        g_target_pan = g_target_tilt = 0.0f;
        g_pan = g_tilt = 0.0f;
        apply_servos();
        send_ok();
        return;

    case 'Q':
    case 'q':
        send_status();
        return;

    case 'S':
    case 's': {
        long ms = strtol(rest, NULL, 10);
        if (ms < 0)    ms = 0;
        if (ms > 2000) ms = 2000;
        g_slew_ms = (uint32_t)ms;
        out_line("# slew %u ms\n", (unsigned)g_slew_ms);
        send_ok();
        return;
    }
    case 'L':
    case 'l':
        gpio_set_level(STATUS_LED_GPIO, atoi(rest) ? 1 : 0);
        send_ok();
        return;

    case 'G':
    case 'g':
        set_pan_pin(atoi(rest));
        return;

    case 'T':
    case 't':
        set_tilt_pin(atoi(rest));
        return;

    case 'I':
    case 'i':
        send_pins();
        return;

    case 'X':
    case 'x':
        start_scan();
        return;

    /* N = GPIO 裸翻转。把指定脚当普通 IO 每 300ms 翻转一次（方波）。
     * 用途：**万用表量不到 50Hz 的 PWM**（普通表交流档频响只有几十 Hz 到几百 Hz，
     * 20ms 周期的 5% 占空比会读成接近 0V 的乱数），但 300ms 的方波一量就清楚：
     *   有 0V↔3.3V 跳变  → ESP32 输出正常，问题在舵机/供电/杜邦线
     *   恒定 0V 或 3.3V   → 引脚没配成输出，或量错了脚（丝印不是物理序号） */
    case 'N':
    case 'n': {
        int pin = atoi(rest);
        if (!is_user_gpio(pin)) {
            send_err(4, "bad_pin");
            return;
        }
        /* 先把 LEDC 从这脚上摘掉，否则矩阵还在往它灌 PWM，翻转没效果 */
        if (pin == g_pan_pin)  unroute_gpio(g_pan_pin);
        if (pin == g_tilt_pin) unroute_gpio(g_tilt_pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
        g_toggle_pin    = pin;
        g_toggle_phase  = 0;
        g_toggle_next   = xTaskGetTickCount();
        g_toggling      = true;
        out_line("TOGGLE %d\n", pin);
        return;
    }

    /* 停止翻转，并把引脚还给 LEDC */
    case 'A':
    case 'a':
        if (g_toggling) {
            g_toggling = false;
            gpio_set_level(g_toggle_pin, 0);
            if (g_toggle_pin == g_pan_pin)  route_ledc_to_gpio(PAN_LEDC_CH,  g_pan_pin);
            if (g_toggle_pin == g_tilt_pin) route_ledc_to_gpio(TILT_LEDC_CH, g_tilt_pin);
            out_line("TOGGLE STOP %d\n", g_toggle_pin);
        } else {
            out_line("TOGGLE STOP 0\n");
        }
        return;

    /* V = 引脚电平探测（上拉/下拉各读一次，判断那个针上到底有没有东西接着）
     *   V <pin>        探测单个引脚
     *   V 14 15 16 13 17 18   一次探多个（用来确认 3 孔壳到底套在哪三个针上） */
    case 'V':
    case 'v': {
        if (rest[0] == '\0') { send_err(3, "need_args"); return; }
        char *p = rest;
        bool any = false;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char *endp = NULL;
            long pin = strtol(p, &endp, 10);
            if (endp == p) break;
            p = endp;
            if (!is_user_gpio((int)pin)) { send_err(4, "bad_pin"); continue; }
            sense_pin((int)pin);
            any = true;
        }
        if (!any) send_err(2, "bad_number");
        return;
    }

    /* W = 三连发（"Wide"）：D0 D1 D2 三个脚**同时**输出同一路舵机信号。
     *
     * 为什么需要这个：
     *   舵机是 3 孔一体塑料壳，拆不开，只能整排套在相邻三个针上。
     *   于是"信号线到底落在哪个针"不由我们决定，而由壳体孔距决定。
     *   与其让用户去挪线（挪不了），不如让固件**三个针一起驱动** ——
     *   真信号一定在其中之一上，一次性覆盖，不用猜。
     *
     * 用法：
     *   W 14 15 16     把 PAN 通道同时接到这三个脚，之后 P/H 命令都生效
     *   W 0            关闭三连发，恢复单脚
     *
     * 安全性：is_user_gpio() 会把 19/20(USB)、0/45/46(strapping)、
     * 26~37(Flash/PSRAM)、43/44(UART0) 全部挡掉，避免又把自己 USB 打断。 */
    case 'W':
    case 'w': {
        if (rest[0] == '\0') { send_err(3, "need_args"); return; }

        int pins[8];
        int np = 0;
        char *p = rest;
        while (*p && np < 8) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char *endp = NULL;
            long v = strtol(p, &endp, 10);
            if (endp == p) break;
            p = endp;
            pins[np++] = (int)v;
        }
        if (np == 1 && pins[0] == 0) {      /* W 0 = 关闭 */
            for (int i = 0; i < g_wfan_n; i++) unroute_gpio(g_wfan_pins[i]);
            g_wfan_n = 0;
            route_ledc_to_gpio(PAN_LEDC_CH, g_pan_pin);
            apply_servos();
            out_line("WFAN 0\n");
            send_pins();
            return;
        }
        if (np == 0) { send_err(2, "bad_number"); return; }

        /* 先撤掉旧的 */
        for (int i = 0; i < g_wfan_n; i++) unroute_gpio(g_wfan_pins[i]);
        g_wfan_n = 0;

        /* 原 PAN 脚也撤掉，避免多驱动一个无关引脚 */
        unroute_gpio(g_pan_pin);

        bool bad = false;
        for (int i = 0; i < np; i++) {
            if (!is_user_gpio(pins[i])) { bad = true; continue; }
            g_wfan_pins[g_wfan_n++] = pins[i];
            route_ledc_to_gpio(PAN_LEDC_CH, pins[i]);
        }
        apply_servos();

        /* 把主引脚记为第一个，这样 I 命令和后续单脚操作有稳定基准 */
        g_pan_pin = (g_wfan_n > 0) ? g_wfan_pins[0] : PAN_PIN_DEFAULT;

        out_line("WFAN");
        for (int i = 0; i < g_wfan_n; i++) out_line(" %d", g_wfan_pins[i]);
        out_line("\n");
        if (bad) send_err(4, "bad_pin_skipped");
        return;
    }

    /* D = 演示摆动：1000~2000us / 50Hz / 每边 4 秒，和那个 Arduino bin 同参数
     *   D 1   开始
     *   D 0   停止（回到 P 命令控制） */
    case 'D':
    case 'd': {
        g_demo = (atoi(rest) != 0);
        g_demo_phase = 0.0f;
        out_line("DEMO %d\n", g_demo ? 1 : 0);
        return;
    }

    /* R = 回环自测：量指定脚上真实输出的脉宽/周期
     *   R <pin>   开始测（跑 1 秒后自动出报告）
     *   R         只出报告
     *   R -1      停止 */
    case 'R':
    case 'r': {
        int pin = (rest[0] == '\0') ? -2 : atoi(rest);
        if (pin == -1) {
            lb_stop();
            out_line("LB STOP\n");
            return;
        }
        if (pin >= 0) {
            lb_start(pin);
            /* 让它先采 1 秒，再出报告 */
            out_line("LB START %d\n", pin);
            return;
        }
        lb_report();
        return;
    }

    default:
        send_err(9, "unknown_cmd");
        return;
    }
}

/* ============ 回环自测：定时出报告 ============ */
static void update_loopback(void)
{
    if (!g_lb_active) return;
    static TickType_t next = 0;
    TickType_t now = xTaskGetTickCount();
    if (next == 0) next = now + pdMS_TO_TICKS(1000);
    if (now < next) return;
    next = now + pdMS_TO_TICKS(1000);
    lb_report();
}

/* ============ 翻转输出（诊断用）============ */
static void update_toggle(void)
{
    if (!g_toggling) return;
    TickType_t now = xTaskGetTickCount();
    if (now < g_toggle_next) return;

    g_toggle_next = now + pdMS_TO_TICKS(300);
    g_toggle_phase ^= 1;
    gpio_set_level(g_toggle_pin, g_toggle_phase);
}

/* ============ 缓动 ============ */
static void update_slew(uint32_t dt_ms)
{
    if (g_slew_ms == 0) return;

    float k = (float)dt_ms / (float)g_slew_ms;
    if (k > 1.0f) k = 1.0f;

    g_pan  += (g_target_pan  - g_pan)  * k;
    g_tilt += (g_target_tilt - g_tilt) * k;

    if (fabsf(g_target_pan  - g_pan)  < 0.05f) g_pan  = g_target_pan;
    if (fabsf(g_target_tilt - g_tilt) < 0.05f) g_tilt = g_target_tilt;

    apply_servos();
}

/* ============ 引脚扫描：每 1.2s 换一个脚，±30° 摆动 ============ */
static void update_scan(void)
{
    if (!g_scanning) return;

    TickType_t now = xTaskGetTickCount();

    if (now >= g_scan_next) {
        /* 该换脚了 */
        g_scan_idx++;
        if (g_scan_idx >= (int)SCAN_PIN_COUNT) {
            g_scanning = false;
            unroute_gpio(g_pan_pin);
            g_pan = g_target_pan = 0.0f;
            g_pan_pin = PAN_PIN_DEFAULT;
            route_ledc_to_gpio(PAN_LEDC_CH, g_pan_pin);
            apply_servos();
            out_line("SCAN DONE\n");
            return;
        }
        g_scan_phase = 0.0f;
        g_scan_next = now + pdMS_TO_TICKS(1200);
        unroute_gpio(g_pan_pin);
        g_pan_pin = SCAN_PINS[g_scan_idx];
        route_ledc_to_gpio(PAN_LEDC_CH, g_pan_pin);
        out_line("SCAN %d\n", g_pan_pin);
    }

    /* 正弦摆动 ±30°，一个扫描周期里走完一个来回 */
    g_scan_phase += 0.09f;
    if (g_scan_phase > 6.2832f) g_scan_phase -= 6.2832f;
    g_pan = sinf(g_scan_phase) * 30.0f;
    ledc_set_duty(LEDC_MODE_SEL, PAN_LEDC_CH, angle_to_duty(g_pan, PAN_RANGE_DEG));
    ledc_update_duty(LEDC_MODE_SEL, PAN_LEDC_CH);
}

/* ============ 初始化 ============ */
static void ledc_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_MODE_SEL,
        .timer_num       = LEDC_TIMER_SEL,
        .duty_resolution = SERVO_RES_BITS,
        .freq_hz         = SERVO_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    /* gpio_num 必须给一个**合法输出脚**。
     *
     * 这里踩过一个致命坑：为了"通道不绑死引脚、方便运行时切换"，第一版传了
     * gpio_num = -1，结果每次上电都挂：
     *     ESP_ERROR_CHECK failed: esp_err_t 0x102 (ESP_ERR_INVALID_ARG)
     *     file: "./main/gimbal_main.c" line 421
     *     expression: ledc_channel_config(&c0)
     * 因为 IDF 6.0 的 ledc_channel_config() 第一件事就是
     *     LEDC_ARG_CHECK(GPIO_IS_VALID_OUTPUT_GPIO(gpio_num), "gpio_num");
     * -1 不在 [0, SOC_GPIO_PIN_COUNT) 里 → 返回 ESP_ERR_INVALID_ARG →
     * ESP_ERROR_CHECK 直接 abort → app_main 根本没进主循环，串口上除了 panic
     * 什么都没有，表现就是"舵机一动不动"。
     *
     * 正确做法：用默认脚完成通道初始化，之后切换引脚走 route_ledc_to_gpio()
     * 的 GPIO 矩阵路由（绕开 channel 配置，PWM 波形不中断）。 */
    ledc_channel_config_t c0 = {
        .gpio_num   = g_pan_pin,
        .speed_mode = LEDC_MODE_SEL,
        .channel    = PAN_LEDC_CH,
        .timer_sel  = LEDC_TIMER_SEL,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config_t c1 = c0;
    c1.channel  = TILT_LEDC_CH;
    c1.gpio_num = g_tilt_pin;

    ESP_ERROR_CHECK(ledc_channel_config(&c0));
    ESP_ERROR_CHECK(ledc_channel_config(&c1));
}

void app_main(void)
{
    ledc_init();
    route_ledc_to_gpio(PAN_LEDC_CH,  g_pan_pin);
    route_ledc_to_gpio(TILT_LEDC_CH, g_tilt_pin);

    /* 把原生 USB-Serial-JTAG 也装上，让它**能收**命令。
     *
     * 为什么需要这一步：
     *   双控制台里 UART0 是 primary（getchar 从它读），USB-JTAG 只是 secondary，
     *   而 IDF 的 secondary 按设计**只输出不输入**（见 esp_stdio/stdio_vfs.c：
     *   "the secondary is only used for output"）。
     *   结果就是：插 PC 的 USB 口只能看日志不能下命令（实测写超时），
     *   只有接 COM 口(经 CH343 的 UART0)才能发命令 —— 调试时来回换线很烦。
     *
     * 装了 driver 之后 USB 口也能收发，两个口都能发命令，插哪个都行。 */
    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    bool usb_ok = (usb_serial_jtag_driver_install(&usb_cfg) == ESP_OK);

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << STATUS_LED_GPIO),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(STATUS_LED_GPIO, 0);

    /* 上电先回中，避免带着上次角度猛冲撞限位 */
    g_pan = g_tilt = g_target_pan = g_target_tilt = 0.0f;
    apply_servos();

    out_line("BOOT gimbal v2\n");
    out_line("READY pan=[-%.0f,%.0f] tilt=[-%.0f,%.0f] panPin=%d tiltPin=%d usbIn=%d\n",
             PAN_RANGE_DEG, PAN_RANGE_DEG, TILT_RANGE_DEG, TILT_RANGE_DEG,
             g_pan_pin, g_tilt_pin, usb_ok ? 1 : 0);

    char line[LINE_BUF_MAX];
    size_t pos = 0;
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        /* --- 输入 ①：UART0（COM 口 / USB-转-TTL）--- */
        int n;
        while ((n = getchar()) != EOF) {
            char c = (char)n;
            if (c == '\n' || c == '\r') {
                if (pos > 0) {
                    line[pos] = '\0';
                    handle_line(line);
                    pos = 0;
                }
            } else if (pos < LINE_BUF_MAX - 1) {
                line[pos++] = c;
            } else {
                pos = 0;
            }
        }

        /* --- 输入 ②：原生 USB（插 PC 的 USB 口）--- */
        if (usb_ok) {
            char ubuf[32];
            int got = usb_serial_jtag_read_bytes(ubuf, sizeof(ubuf), 0);  /* 0 tick = 不阻塞 */
            for (int i = 0; i < got; i++) {
                char c = ubuf[i];
                if (c == '\n' || c == '\r') {
                    if (pos > 0) {
                        line[pos] = '\0';
                        handle_line(line);
                        pos = 0;
                    }
                } else if (pos < LINE_BUF_MAX - 1) {
                    line[pos++] = c;
                } else {
                    pos = 0;
                }
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CTRL_PERIOD_MS));

        if (g_demo)          update_demo(CTRL_PERIOD_MS);
        else if (g_toggling) update_toggle();
        else if (g_scanning) update_scan();
        else                 update_slew(CTRL_PERIOD_MS);

        update_loopback();
    }
}
