#!/bin/bash
# ============================================================================
#  install_voice_service.sh —— 安装 voice-gateway.service
# ============================================================================
#  做三件事：校验单元 -> daemon-reload -> enable（开机自启）
#  **故意不 start**：当前可能有一个手工 nohup 起来的 voice_gateway 正占着
#  麦克风和 NPU，此时启动服务 = 两个实例抢 hw:0,0，新的一定 exit 255 然后
#  无限重启。要交给 systemd 管，请先按脚本末尾的提示停掉手工实例。
#
#  用法：
#    bash /root/install_voice_service.sh
# ============================================================================

set -euo pipefail

UNIT=voice-gateway.service
UNIT_PATH=/etc/systemd/system/$UNIT
CONFLICT=edgevision.service

die() { echo "[✗] $*" >&2; exit 1; }
ok()  { echo "[✓] $*"; }

[ "$(id -u)" -eq 0 ] || die "需要 root 权限"
[ -f "$UNIT_PATH" ] || die "缺少 $UNIT_PATH（单元文件没有就位，先把它放进 /etc/systemd/system/）"
[ -x /root/voice_gateway ] || die "/root/voice_gateway 不存在或没有可执行权限"

chmod 644 "$UNIT_PATH"   # 单元文件带执行位会被 systemd 警告

echo "── [1/4] 单元语法检查 ─────────────────────────────"
# systemd-analyze verify 对"未知依赖单元"之类只报警告，退出码仍为 0；
# 真正的语法错误/依赖循环才会非 0，那种情况直接中止安装。
if ! out=$(systemd-analyze verify "$UNIT_PATH" 2>&1); then
    printf '%s\n' "$out"
    die "systemd-analyze verify 未通过，已中止"
fi
if [ -n "$out" ]; then printf "%s\n" "$out"; fi
ok "单元语法与依赖关系正常"

echo "── [2/4] daemon-reload ───────────────────────────"
systemctl daemon-reload
ok "systemd 已重载单元"

echo "── [3/4] enable（不 start）───────────────────────"
systemctl enable "$UNIT"
ok "已设为开机自启（WantedBy=multi-user.target）"

echo "── [4/4] 现状检查 ────────────────────────────────"
echo "    单元文件   : $UNIT_PATH"
echo "    enable     : $(systemctl is-enabled "$UNIT")"
echo "    运行状态   : $(systemctl is-active "$UNIT")  （预期 inactive，本脚本不启动）"
echo "    $CONFLICT : $(systemctl is-active "$CONFLICT")"

MANUAL=$(pgrep -f 'voice_gatewa' 2>/dev/null | tr '\n' ' ' | sed 's/ $//' || true)
if [ -n "${MANUAL:-}" ] && ! systemctl is-active --quiet "$UNIT"; then
    echo
    echo "    ⚠ 检测到手工启动的实例（pid: $MANUAL），它仍占着 hw:0,0。"
    echo "      现在直接 systemctl start 会和它抢麦克风 -> exit 255 无限重启。"
    echo "      交给 systemd 接管的正确顺序："
    echo "        bash /root/voice_start.sh --stop          # 停手工实例（会顺带恢复 $CONFLICT）"
    echo "        systemctl start $UNIT                     # 此时才会成功"
    echo "        journalctl -u $UNIT -f                    # 看日志（前 ~42s 是 Agent 模型预热）"
fi

if systemctl is-active --quiet "$CONFLICT"; then
    echo
    echo "    ⚠ $CONFLICT 正在运行，两者抢 NPU（Agent 响应 10s -> 39s）。"
    echo "      单元里已声明 Conflicts=$CONFLICT，启动本服务时会自动把它停掉；"
    echo "      下次开机只会拉起本服务，$CONFLICT 保持 stopped。"
fi

echo
echo "完成。未启动任何进程。"
