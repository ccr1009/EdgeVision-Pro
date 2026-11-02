#!/bin/bash
echo "===== 1. rockchip 头文件清单 ====="
ls -la /usr/include/rockchip/ 2>&1
echo
echo "===== 2. 搜 MPP_DEC_SET_PARSER_SPLIT_MODE 是否真实存在 ====="
grep -rn 'PARSER_SPLIT_MODE' /usr/include/rockchip/ 2>&1
echo "  (空=不存在)"
echo
echo "===== 3. 搜 MPP_DEC_SET_EXT_BUF_GROUP / IMMEDIATE_OUT / OUTPUT_FORMAT ====="
grep -rn 'MPP_DEC_SET_EXT_BUF_GROUP\|MPP_DEC_SET_IMMEDIATE_OUT\|MPP_DEC_SET_OUTPUT_FORMAT\|MPP_DEC_SET_CFG\|MPP_DEC_GET_CFG\|MPP_DEC_SET_INFO_CHANGE_READY' /usr/include/rockchip/ 2>&1
echo
echo "===== 4. MppDecCfg / mpp_dec_cfg 接口 ====="
grep -rn 'mpp_dec_cfg_init\|mpp_dec_cfg_set_u32\|mpp_dec_cfg_get_u32\|MppDecCfg' /usr/include/rockchip/ 2>&1
echo
echo "===== 5. mpp_dec_cfg.h 内容（若有） ====="
cat /usr/include/rockchip/mpp_dec_cfg.h 2>/dev/null | head -60
echo
echo "===== 6. MppDecCfgBase 结构体定义 ====="
grep -rn -A 20 'typedef struct MppDecCfgBase_t' /usr/include/rockchip/*.h 2>&1
echo
echo "===== 7. split_parse 字符串 ====="
grep -rn 'split_parse' /usr/include/rockchip/ 2>&1
echo
echo "===== 8. library 里的符号（看 cfg 接口在不在 .so 里） ====="
nm -D /usr/lib/aarch64-linux-gnu/librockchip_mpp.so.0 2>/dev/null | grep -E 'mpp_dec_cfg' 
echo
echo "===== 9. 环境：strace 有没有 ====="
command -v strace; command -v gdb; g++ --version | head -1
echo "===== DONE ====="
