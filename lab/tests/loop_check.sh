#!/bin/bash
# ============================================================================
# /root/loop_check.sh —— 声学回路连通性自检（跑 bench/e2e 之前必须先过这一关）
# ============================================================================
#  为什么需要它：
#    2026-10-31 晚上跑 bench_voice.sh「小瑞」10 次，结果 0/10，
#    识别出来是「小孩女孩玩的玩具」「sorry you」「小孩女女人的的的的的的」。
#    看着像"唤醒词识别率极差"，实际是**声学回路物理断开**——
#    ES8388 的 Headphone Jack 检测到耳机插着（numid=14 = on），
#    声音全进了耳机，摄像头麦克风在空气里什么也听不到。
#    板子录到的只有 −31 dB 底噪，ASR 对着纯噪声硬编，于是输出幻觉文本。
#
#    也就是说：**没有这个自检，0/10 这种数字全是假的**，
#    既可能是"识别率真的差"，也可能是"根本没出声"，两者无法区分。
#
#  判据：播放一段已知音频，同时录音，看录音里有没有出现明显的能量台阶。
#        台阶 < 8 dB 视为回路不通。
#
#  用法：
#    bash loop_check.sh            # 自检，通过返回 0，不通过返回 1
#    bash loop_check.sh -v         # 附带能量包络明细
# ============================================================================
set -u

VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

VGW=/root/voice_gateway
WAV=/tmp/loopchk_play.wav
REC=/tmp/loopchk_rec.wav
MONO=/tmp/loopchk_mono.wav

fail() { echo "✗ 声学回路不通：$*"; exit 1; }

# ---- 前置 ----
[ -S /tmp/tts_daemon.sock ] || fail "TTS daemon 没跑（/root/tts_daemon &），无法合成测试音"
if pgrep -x voice_gateway >/dev/null 2>&1; then
    fail "voice_gateway 在跑，会独占麦克风。先 bash /root/voice_start.sh --stop"
fi

# ---- 临时拉满音量，退出时恢复 ----
# 平时播报用舒适音量（用户会嫌吵），但自测需要足够电平才能测到能量台阶。
# 所以这里借一下满档，只播 1 秒「小瑞」，测完立刻还原。
# 用 trap 保证即使中途 exit 也能恢复，否则会把板子留在最大音量上。
# 注意字段位置，两种控件格式不同（取错会取到百分比，恢复时差一档）：
#   PCM      : "  Front Left: 160 [83%] [-16.00dB]"        → 值是 $3
#   Output 1 : "  Front Left: Playback 28 [78%] [...]"     → 值是 $4
pcm_before=$(amixer -c 2 sget PCM 2>/dev/null | awk '/Front Left:/{print $3}')
out1_before=$(amixer -c 2 sget "Output 1" 2>/dev/null | awk '/Front Left:/{print $4}')
out2_before=$(amixer -c 2 sget "Output 2" 2>/dev/null | awk '/Front Left:/{print $4}')
spk_before=$(amixer -c 2 sget Speaker 2>/dev/null | awk '/Mono:/{print $3}' | tr -d '[]')

restore_vol() {
    case "${pcm_before:-}"  in ''|*[!0-9]*) ;; *) amixer -c 2 sset PCM "$pcm_before" >/dev/null 2>&1 ;; esac
    case "${out1_before:-}" in ''|*[!0-9]*) ;; *) amixer -c 2 sset "Output 1" "$out1_before" >/dev/null 2>&1 ;; esac
    case "${out2_before:-}" in ''|*[!0-9]*) ;; *) amixer -c 2 sset "Output 2" "$out2_before" >/dev/null 2>&1 ;; esac
    # Speaker 开关不动：它是回路能不能出声的总闸，误关掉会比音量问题更糟
}
trap restore_vol EXIT INT TERM

# Speaker 必须开着——板载扬声器通路默认是 off，声音只走耳机，
# 麦克风在空气里什么都收不到（2026-10-31 实测：Speaker=off 时台阶只有 6.4 dB）
amixer -c 2 sset Speaker on  >/dev/null 2>&1
amixer -c 2 cset numid=16 on >/dev/null 2>&1
amixer -c 2 sset PCM 192      >/dev/null 2>&1
amixer -c 2 sset "Output 1" 36 >/dev/null 2>&1
amixer -c 2 sset "Output 2" 36 >/dev/null 2>&1

# ---- 合成测试音（用 TTS 走真实链路，不用 sox 正弦波：正弦波过不了声学路径的频响判断）----
"$VGW" --say "小瑞" >/dev/null 2>&1
[ -f /tmp/vgw_spk.wav ] || fail "TTS 合成失败，/tmp/vgw_spk.wav 没生成"
cp -f /tmp/vgw_spk.wav "$WAV"

# ---- 同步播放 + 录音 ----
arecord -D plughw:0,0 -f S16_LE -r 16000 -c 2 -d 4 "$REC" 2>/dev/null &
apid=$!
sleep 0.4
aplay -D hw:2,0 "$WAV" >/dev/null 2>&1
wait "$apid" 2>/dev/null

sox "$REC" -c 1 "$MONO" remix 1 2>/dev/null || fail "录音降混失败"
[ -s "$MONO" ] || fail "录音为空"

# ---- 算能量包络：0.25 秒一格 ----
# 基线取最后 1 秒（播放早已结束），峰值取全程最大格
n=$(( $(soxi -D "$MONO" 2>/dev/null | cut -d. -f1) * 4 ))
[ "$n" -ge 8 ] || n=8

peak=-999; base=-999; peak_at=0
for i in $(seq 0 $((n-1))); do
    s=$(awk -v i="$i" 'BEGIN{printf "%.2f", i*0.25}')
    sox "$MONO" /tmp/loopchk_seg.wav trim "$s" 0.25 2>/dev/null
    r=$(sox /tmp/loopchk_seg.wav -n stats 2>&1 | awk '/^RMS lev dB/{print $4}')
    [ -z "$r" ] && continue
    if awk -v a="$r" -v b="$peak" 'BEGIN{exit !(a>b)}'; then peak="$r"; peak_at="$s"; fi
    # 最后 4 格（=1 秒）取最大作为基线
    if [ "$i" -ge $((n-4)) ]; then
        if awk -v a="$r" -v b="$base" 'BEGIN{exit !(a>b)}'; then base="$r"; fi
    fi
done
rm -f /tmp/loopchk_seg.wav

[ "$VERBOSE" = "1" ] && echo "  峰值 ${peak} dB @ ${peak_at}s   基线 ${base} dB"

delta=$(awk -v p="$peak" -v b="$base" 'BEGIN{printf "%.1f", p-b}')
echo "  声学回路台阶：${delta} dB（判据 ≥ 8 dB）"

if awk -v d="$delta" 'BEGIN{exit !(d < 8)}'; then
    cat <<EOF
✗ 声学回路不通：播放时录音没有出现能量台阶（${delta} dB）

  按可能性排查（三个都查过再动手）：

  ① **板载扬声器通路是否开着**（最常见）
       amixer -c 2 sget Speaker        # 必须是 [on]
       amixer -c 2 sset Speaker on
     这是 ES8388 的总输出开关，默认 **off**。关着时声音只走耳机口，
     aplay 照常返回 0、音量显示满档、日志显示播放完成，就是没有声音。

  ② 耳机是否插着
       amixer -c 2 cget numid=14       # Headphone Jack，=on 表示插着
     声音会优先走耳机，摄像头麦克风在空气里收不到。

  ③ 输出级音量是否为 0
       amixer -c 2 sget "Output 1"

  注意：回路不通时 bench_voice / test_e2e 的**任何数字都不作数**——
  那不是"识别失败"，是"没有测量"。ASR 会对着纯噪声硬编出幻觉文本，
  看起来非常像一个真实的失败结果。
EOF
    exit 1
fi

echo "✓ 声学回路连通（台阶 ${delta} dB）"
exit 0
