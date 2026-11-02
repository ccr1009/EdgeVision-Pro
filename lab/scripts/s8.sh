#!/bin/bash
echo "=== improcess signatures ==="
sed -n '325,345p' /usr/include/rga/im2d.hpp
echo "=== IM_SYNC / imStrError ==="
grep -n "IM_SYNC\|imStrError" /usr/include/rga/im2d.h | head -5
echo "=== releasebuffer_handle decl ==="
grep -rn "releasebuffer_handle" /usr/include/rga/im2d.hpp | head -3
echo "=== imcheck ==="
grep -n "IM_API IM_STATUS imcheck" /usr/include/rga/im2d.hpp | head
