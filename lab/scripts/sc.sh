#!/bin/bash
echo "=== imcheck signature ==="
grep -n "imcheck" /usr/include/rga/im2d.hpp /usr/include/rga/im2d.h | head -5
echo "=== imfill_t signature ==="
grep -n "imfill_t" /usr/include/rga/im2d.hpp | head -3
