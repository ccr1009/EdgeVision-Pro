#!/bin/bash
echo "=== IM_SYNC define ==="
grep -rn "define IM_SYNC\|IM_SYNC " /usr/include/rga/*.h | head -5
echo "=== releasebuffer_handle ==="
grep -rn "releasebuffer_handle" /usr/include/rga/*.h | head -5
echo "=== imcvtcolor enough? ==="
grep -rn "imresize\|imcvtcolor\|imfill" /usr/include/rga/im2d.h | head -8
echo "=== imcheck decl ==="
grep -rn "imcheck" /usr/include/rga/im2d.hpp | head -5
