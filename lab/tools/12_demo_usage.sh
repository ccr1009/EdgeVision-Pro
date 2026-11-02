#!/bin/bash
cd /tmp
echo "===== A. mpi_dec_test -h 分开捕获 ====="
/usr/bin/mpi_dec_test -h > /tmp/u_out.txt 2> /tmp/u_err.txt; echo "rc(-h)=$?"
echo "--- stdout ($(wc -l < /tmp/u_out.txt) lines) ---"; head -80 /tmp/u_out.txt
echo "--- stderr ($(wc -l < /tmp/u_err.txt) lines) ---"; head -80 /tmp/u_err.txt

echo
echo "===== B. 无参 ====="
/usr/bin/mpi_dec_test > /tmp/n_out.txt 2> /tmp/n_err.txt; echo "rc(noarg)=$?"
echo "--- stdout ($(wc -l < /tmp/n_out.txt) lines) ---"; head -100 /tmp/n_out.txt
echo "--- stderr ($(wc -l < /tmp/n_err.txt) lines) ---"; head -60 /tmp/n_err.txt

echo
echo "===== C. 依赖检查 ====="
ldd /usr/bin/mpi_dec_test 2>&1 | head -20
echo "--- libmpp ---"
ls -l /usr/lib/aarch64-linux-gnu/librockchip_mpp* 2>&1

echo
echo "===== D. 二进制里的 usage 字符串 ====="
strings /usr/bin/mpi_dec_test | grep -iE 'usage|mjpeg|input_file|output_file|frame_num|split|dec_cfg' | head -40

echo
echo "===== E. MPP_DEC_SET_* 控制命令全集 ====="
grep -n 'MPP_DEC_SET_' /usr/include/rockchip/rk_mpi.h 2>/dev/null | head -60

echo
echo "===== F. mpp_info_test 直接跑 ====="
/usr/bin/mpp_info_test > /tmp/i_out.txt 2>&1; echo "rc=$?"
head -40 /tmp/i_out.txt
echo "===== DONE ====="
