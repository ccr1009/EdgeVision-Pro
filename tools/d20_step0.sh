#!/bin/bash
# ============================================================
#  D20 第 0 步: 亲眼看见 if 和 while 的区别
#  用法:  bash d20_step0.sh
#  做的事: 把 if 版复制一份改成 while 版, 两个都编译, 两个都跑
# ============================================================
set -u
cd /root/lab || exit 1
sed -i 's/\r$//' q3_spurious.cpp 2>/dev/null

echo "=============================================================="
echo "  A. 当前写法 (if) —— 空队列也往下走"
echo "=============================================================="
g++ -O2 -std=c++11 -pthread q3_spurious.cpp -o q3_if || exit 2
timeout 5 ./q3_if
echo "退出码 = $?"
echo

echo "=============================================================="
echo "  B. 改成 while —— 醒来发现还是空的, 继续睡"
echo "=============================================================="
sed 's/if (q.empty())/while (q.empty())/' q3_spurious.cpp > q3_while.cpp
grep -n 'while (q.empty())' q3_while.cpp
g++ -O2 -std=c++11 -pthread q3_while.cpp -o q3_while || exit 2
timeout 5 ./q3_while
RC=$?
echo "退出码 = $RC   (124 = 被 timeout 掐断, 说明它卡住了 —— 这正是预期)"
