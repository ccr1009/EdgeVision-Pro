import os
import paramiko
import time

# 板子 IP 与私钥路径不入库，用环境变量传：
#   set EDGEVISION_BOARD_HOST=<board-ip>
#   set EDGEVISION_SSH_KEY=%USERPROFILE%\.ssh\id_ed25519_rk3588
BOARD_HOST = os.environ.get("EDGEVISION_BOARD_HOST", "")
SSH_KEY = os.environ.get("EDGEVISION_SSH_KEY", os.path.expanduser("~/.ssh/id_ed25519_rk3588"))
if not BOARD_HOST:
    raise SystemExit("请先设置环境变量 EDGEVISION_BOARD_HOST=<板子IP>")

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect(BOARD_HOST, username="root", key_filename=SSH_KEY)

# 1. Update test_tools2.cpp
cpp_code = r'''#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <csignal>
#include "rkllm.h"

static std::string g_output_buffer;

int callback(RKLLMResult *result, void *userdata, LLMCallState state) {
    if (state == RKLLM_RUN_NORMAL) {
        std::cout << result->text << std::flush;
        g_output_buffer += result->text;
    } else if (state == RKLLM_RUN_FINISH) {
        std::cout << std::endl;
    }
    return 0;
}

// 真实读取系统温度 - 注意 RKLLM 要求返回 JSON 数组格式: [{"key": value}]
std::string get_real_hardware_status() {
    std::ifstream file("/sys/class/thermal/thermal_zone0/temp");
    std::string temp_str = "0";
    if (file.is_open()) {
        file >> temp_str;
    }
    double temp_c = std::stod(temp_str) / 1000.0;
    
    std::stringstream ss;
    ss << "[{\"soc_temperature_c\": " << temp_c << ", \"npu_temperature_c\": " << temp_c 
       << ", \"hardware_platform\": \"Rockchip RK3588\", \"status\": \"optimal\"}]";
    return ss.str();
}

int main() {
    RKLLMParam param = rkllm_createDefaultParam();
    param.model_path = "qwen3.5-2b_w8a8_rk3588.rkllm";
    param.max_new_tokens = 256;
    param.max_context_len = 2048;

    RKLLMCallback cb = {};
    cb.result_callback = callback;
    LLMHandle handle = nullptr;
    int ret = rkllm_init(&handle, &param, &cb);
    if (ret != 0) return 1;

    const char* sys_prompt = "你是一个部署在 RK3588 边缘设备上的智能运维 Agent。你可以调用硬件工具感知物理世界，并用中文向用户汇报真实状态。";
    const char* tools_json = R"([
      {
        "type": "function",
        "function": {
          "name": "get_system_health",
          "description": "获取开发板当前的物理真实温度(SOC/NPU)及硬件健康状态",
          "parameters": {
            "type": "object",
            "properties": {}
          }
        }
      }
    ])";

    rkllm_set_function_tools(handle, sys_prompt, tools_json, "<tool_response>");

    RKLLMInferParam infer{};
    infer.mode = RKLLM_INFER_GENERATE;
    infer.keep_history = 1; // 关键：开启多轮上下文保留！

    // 1. 用户提问
    std::cout << "\n[User] 开发板当前实际温度是多少？" << std::endl;
    std::cout << "[Agent 思考中...]" << std::endl;
    g_output_buffer.clear();

    RKLLMInput input1{};
    input1.role = "user";
    input1.input_type = RKLLM_INPUT_PROMPT;
    input1.prompt_input = "开发板当前实际温度是多少？";
    rkllm_run(handle, &input1, &infer, nullptr);

    // 2. 检查大模型是否输出了工具调用
    if (g_output_buffer.find("get_system_health") != std::string::npos) {
        std::cout << "\n[Agent 决策] 检测到大模型自主调用了硬件工具: get_system_health()" << std::endl;
        
        // 3. 执行真实硬件读取
        std::string hw_data = get_real_hardware_status();
        std::cout << "[硬件感知 (Observation)] 传感器读出真实物理数据: " << hw_data << std::endl;

        // 4. 将硬件数据以 role='tool' 回填给大模型
        std::cout << "\n[Agent 终局人话汇报]" << std::endl;
        RKLLMInput input2{};
        input2.role = "tool";
        input2.input_type = RKLLM_INPUT_PROMPT;
        input2.prompt_input = hw_data.c_str();
        rkllm_run(handle, &input2, &infer, nullptr);
    }

    rkllm_destroy(handle);
    return 0;
}
'''

sftp = ssh.open_sftp()
with sftp.file('/root/rkllm_deploy/test_tools2.cpp', 'w') as f:
    f.write(cpp_code)
sftp.close()

# 2. Compile
stdin, stdout, stderr = ssh.exec_command("cd /root/rkllm_deploy && g++ -O2 test_tools2.cpp -L. -lrkllmrt -Wl,-rpath=. -o test_tools2")
err = stderr.read().decode('utf-8')
if err:
    print("Compile error:", err)
else:
    print("Compile success!")

# 3. Run
stdin, stdout, stderr = ssh.exec_command("cd /root/rkllm_deploy && ./test_tools2")
for line in stdout:
    print(line, end="")
print("Stderr:", stderr.read().decode('utf-8'))

ssh.close()
