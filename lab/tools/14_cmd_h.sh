#!/bin/bash
echo "===== rk_mpi_cmd.h 60-140 行（DEC 控制命令定义） ====="
sed -n '60,140p' /usr/include/rockchip/rk_mpi_cmd.h
echo
echo "===== rk_vdec_cfg.h 全文 ====="
cat /usr/include/rockchip/rk_vdec_cfg.h
echo
echo "===== strace mpi_dec_test（看它到底卡在哪） ====="
strace -f -e trace=openat,ioctl,write /usr/bin/mpi_dec_test -i /root/lab/sample.mjpg -o /tmp/o.yuv -w 1280 -h 720 -t 8 -n 1 2>&1 | head -60
echo "--- exit=$? ---"
echo
echo "===== 用 ltrace 看看是不是日志函数没输出 ====="
/usr/bin/mpi_dec_test -i /root/lab/sample.mjpg -o /tmp/o2.yuv -w 1280 -h 720 -t 8 -n 1 > /tmp/d_out.txt 2>/tmp/d_err.txt
echo "rc=$?"
echo "--- stdout ---"; cat /tmp/d_out.txt
echo "--- stderr ---"; cat /tmp/d_err.txt
echo "--- 输出文件 ---"; ls -l /tmp/o2.yuv 2>&1
echo "===== DONE ====="
