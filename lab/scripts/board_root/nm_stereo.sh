#!/bin/bash
cd /root/stereo3d || exit 1
echo "=== 已定义符号 (T = text/函数) ==="
nm -C /tmp/sp.o | grep -E ' T ' | grep -i 'StereoPipeline' | sed 's/^[0-9a-f]* T //'
echo
echo "=== 未定义符号里跟 StereoPipeline 有关的（应为 0）==="
nm -C /tmp/sp.o | grep -E ' U ' | grep -i 'StereoPipeline' || echo "(none) OK"
echo
echo "=== 头文件声明的方法数 ==="
grep -cE '^\s+[A-Za-z_].*\(' include/stereo_pipeline.h