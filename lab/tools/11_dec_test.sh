#!/bin/bash
# 决定性实验：用官方 mpi_dec_test 验证 jpegd 硬件能不能硬解 MJPEG
# 只读 + 在 /tmp 写输出，不碰摄像头、不碰 service
echo "===== 0. 环境 ====="
ls -l /dev/dri/ 2>&1
echo
echo "===== 1. rockchip-mpp-demos 提供的二进制 ====="
dpkg -L rockchip-mpp-demos 2>/dev/null | grep -E 'bin/' 
echo
echo "===== 2. mpi_dec_test 无参数（看 usage） ====="
/usr/bin/mpi_dec_test 2>&1 | head -50
echo
echo "===== 3. MJPEG 的 coding enum 值（从 rk_type.h 数） ====="
sed -n '80,110p' /usr/include/rockchip/rk_type.h
echo
echo "===== 4. mpp_info_test（能力自检） ====="
if command -v mpp_info_test >/dev/null; then mpp_info_test 2>&1 | head -40; else echo "(无 mpp_info_test)"; fi
echo
echo "===== 5. 打开 MPP debug 日志，用 mpi_dec_test 解 sample.mjpg ====="
export MPP_DEBUG=0x20001
/usr/bin/mpi_dec_test -i /root/lab/sample.mjpg -o /tmp/out_mjpg.yuv -w 1280 -h 720 -t 5 -n 1 2>&1 | head -60
echo "exit=$?"
echo
echo "===== 6. 输出文件 ====="
ls -l /tmp/out_mjpg.yuv 2>&1
echo
echo "===== 7. 内核侧有没有报错 ====="
dmesg | tail -25
echo "===== DONE ====="
