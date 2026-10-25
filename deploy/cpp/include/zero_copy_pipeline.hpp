// ============================================================================
//  ⚠️ 本文件已废弃 (2026-10-24) —— 请勿使用 / DO NOT USE
//
//  原因: 这曾是纯仿真桩。DMABufDescriptor.fd 是硬编码的 1000+i / 2000+i,
//  没有任何真实 MPP/RGA/DMA-BUF 内核交互; rga_hardware_transform() 只是
//  改了个 bool 标志位就宣称完成"硬件变换"; 主程序据此打印的
//  "Memory Bandwidth Saved" 全是编造数字, 违反 AGENTS.md 工作约定 3
//  (不许用假数据冒充结果)。
//
//  真实替代品:
//    - MPP 硬解:   include/mpp_jpeg_decoder.h  (edgevision::MppJpegDecoder,
//                  实测 1280x720 MJPEG->NV12 p50 1.03ms, 9/9 PSNR 验证)
//    - 真实主程序: src/edgevision_pro_main.cpp (MPP 硬解 -> RKNN -> BEV)
//
//  本文件保留空壳仅作历史标记, 内容已清空, 编译不依赖它。
// ============================================================================
#pragma once
