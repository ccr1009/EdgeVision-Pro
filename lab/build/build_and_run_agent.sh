#!/bin/bash
set -e
cd /root/rkllm_deploy
g++ -O2 test_tools2.cpp -L. -lrkllmrt -Wl,-rpath=. -o test_tools2
./test_tools2
