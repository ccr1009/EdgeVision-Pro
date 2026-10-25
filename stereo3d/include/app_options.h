// ============================================================================
//  main.cpp —— 双目 3D / 人脸 3D 定位 demo 主程序
//  实现文件：src/main.cpp  （owner: Lead）
// ============================================================================
#pragma once
#include "stereo_types.h"
#include <string>

namespace s3d {

// 命令行解析；失败返回 false 并填 err
bool parseArgs(int argc, char** argv, AppOptions& opt, std::string& err, std::string& help);

// 打印帮助
void printHelp(const char* prog);

}  // namespace s3d
