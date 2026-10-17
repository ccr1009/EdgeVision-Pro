#!/usr/bin/env bash
# EVM3588-B (RK3588) 板卡自检脚本: 内存 / 存储 / 开机 / NPU 驱动
# 用法: 拷贝到板子上执行  ->  bash check_board.sh

PASS=0
WARN=0
FAIL=0

line() { printf '%s\n' "------------------------------------------------------------------"; }
ok()   { printf '  [ OK ]   %s\n' "$1"; PASS=$((PASS+1)); }
warn() { printf '  [WARN]   %s\n' "$1"; WARN=$((WARN+1)); }
bad()  { printf '  [FAIL]   %s\n' "$1"; FAIL=$((FAIL+1)); }

echo "=================================================================="
echo " EVM3588-B 板卡自检  (RK3588  8GB RAM / 64GB eMMC)"
echo " 时间: $(date '+%Y-%m-%d %H:%M:%S')"
echo "=================================================================="

# ---------------- 1. 系统基础 ----------------
line
echo "[1/5] 系统信息"
line
if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo "  系统   : ${PRETTY_NAME:-unknown}"
fi
echo "  内核   : $(uname -r)"
echo "  架构   : $(uname -m)"
echo "  主机名 : $(hostname)"

# ---------------- 2. CPU ----------------
line
echo "[2/5] CPU"
line
CORES=$(nproc)
echo "  逻辑核心数 : ${CORES}"
if grep -q "Cortex-A76" /proc/cpuinfo 2>/dev/null || [ "${CORES}" -ge 8 ]; then
    ok "识别到 8 核 (4xA76 + 4xA55)"
else
    warn "核心数 ${CORES} 与 RK3588 预期的 8 核不符，请确认"
fi

# ---------------- 3. 内存 ----------------
line
echo "[3/5] 内存 (标称 8GB)"
line
free -h
# 用 /proc/meminfo 精确取 MiB 总量
TOTAL_MB=$(awk '/MemTotal/ {printf "%d", $2/1024}' /proc/meminfo)
echo "  MemTotal = ${TOTAL_MB} MiB"
if [ "${TOTAL_MB}" -ge 7300 ] && [ "${TOTAL_MB}" -le 8300 ]; then
    ok "内存容量正常 (8GB 减去 GPU/NPU/内核保留后约 7.6~7.9GiB)"
elif [ "${TOTAL_MB}" -ge 3500 ] && [ "${TOTAL_MB}" -le 4300 ]; then
    bad "内存只认到约 4GB (${TOTAL_MB} MiB)，疑似只识别了一半，联系售后"
else
    warn "MemTotal=${TOTAL_MB} MiB 不在预期区间，请人工确认"
fi

# ---------------- 4. 存储 (eMMC) ----------------
line
echo "[4/5] 存储 eMMC (标称 64GB)"
line
lsblk -o NAME,SIZE,TYPE,MOUNTPOINT 2>/dev/null || echo "  (lsblk 不可用)"
if [ -b /dev/mmcblk0 ]; then
    # 读 eMMC 容量(字节) = 扇区数 * 512
    SEC=$(cat /sys/block/mmcblk0/size 2>/dev/null)
    if [ -n "${SEC}" ]; then
        CAP_GB=$(awk -v s="${SEC}" 'BEGIN{printf "%.1f", s*512/1000/1000/1000}')
        echo "  /dev/mmcblk0 标称容量 = ${CAP_GB} GB"
        if awk -v c="${CAP_GB}" 'BEGIN{exit !(c>=58)}'; then
            ok "eMMC 容量正常 (64GB 标称的可用值约 58~60GB)"
        else
            bad "eMMC 容量 ${CAP_GB}GB 明显偏小，联系售后"
        fi
    fi
    echo "  根分区使用情况:"
    df -h / 2>/dev/null | sed 's/^/    /'
else
    bad "未发现 /dev/mmcblk0，可能从 SD 卡启动或 eMMC 未识别"
fi

# ---------------- 5. 驱动 & 开机健康 ----------------
line
echo "[5/5] 关键驱动 & 开机健康"
line
# NPU: 老驱动节点 /dev/rknpu, 新驱动(0.9.x)走 DRM /dev/dri/renderD*, 或看 debugfs
NPU_OK=0
if [ -e /dev/rknpu ]; then
    NPU_OK=1; ok "/dev/rknpu 存在 (旧版 NPU 节点)"
elif ls /dev/dri/renderD* >/dev/null 2>&1 || [ -e /sys/kernel/debug/rknpu/version ]; then
    NPU_OK=1; ok "NPU 走 DRM 接口 (/dev/dri/renderD*) 或 debugfs, 正常"
fi
[ "${NPU_OK}" -eq 1 ] || bad "NPU 接口缺失 (/dev/rknpu 与 /dev/dri/renderD* 均无)"

for dev in mpp_service rga; do
    if [ -e "/dev/${dev}" ]; then
        ok "/dev/${dev} 存在"
    else
        bad "/dev/${dev} 缺失 (MPP/RGA 驱动异常)"
    fi
done

# NPU 版本 & 负载
if [ -f /sys/kernel/debug/rknpu/version ]; then
    echo "  NPU 驱动版本: $(cat /sys/kernel/debug/rknpu/version 2>/dev/null)"
fi
if [ -f /sys/kernel/debug/rknpu/load ]; then
    echo "  NPU 负载:"; sed 's/^/    /' /sys/kernel/debug/rknpu/load 2>/dev/null
fi

# 温度
for tz in /sys/class/thermal/thermal_zone*; do
    [ -r "${tz}/temp" ] || continue
    T=$(cat "${tz}/temp" 2>/dev/null)
    [ -n "${T}" ] && echo "  $(basename "${tz}") = $((T/1000)) C"
done

# 开机报错扫描
echo "  开机 dmesg 报错扫描:"
FATAL=$(dmesg 2>/dev/null | grep -i -E "kernel panic|Internal error|Unable to handle kernel|\bOops\b|BUG: " | head -n 5)
WARNS=$(dmesg 2>/dev/null | grep -i -E "WARNING:|Call trace" | head -n 3)
if [ -n "${FATAL}" ]; then
    bad "发现内核致命报错:"; printf '%s\n' "${FATAL}" | sed 's/^/    /'
elif [ -n "${WARNS}" ]; then
    warn "存在内核 WARNING/Call trace (非致命, 常见于 WiFi/驱动):"
    printf '%s\n' "${WARNS}" | sed 's/^/    /'
else
    ok "未发现 panic/oops/WARNING 类报错"
fi

echo "  运行时长: uptime = $(uptime -p 2>/dev/null || uptime)"

# ---------------- 总结 ----------------
echo
echo "=================================================================="
echo " 结论:  OK=${PASS}   WARN=${WARN}   FAIL=${FAIL}"
if [ "${FAIL}" -eq 0 ]; then
    echo " >>> 内存 / 存储 / 开机 / 核心驱动 全部通过 <<<"
else
    echo " >>> 存在 FAIL 项，请根据上面提示排查或联系英码售后 <<<"
fi
echo "=================================================================="

exit ${FAIL}
