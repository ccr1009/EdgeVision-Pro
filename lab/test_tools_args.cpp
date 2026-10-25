#include <iostream>
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

    const char* sys_prompt = "你是一个部署在 RK3588 边缘设备上的智能具身运维 Agent。你可以调用硬件工具感知物理世界和控制硬件执行器。";
    const char* tools_json = R"RAW([
      {
        "type": "function",
        "function": {
          "name": "control_led",
          "description": "控制开发板上的物理 LED 状态",
          "parameters": {
            "type": "object",
            "properties": {
              "mode": {
                "type": "string",
                "enum": ["heartbeat", "alarm", "on", "off"],
                "description": "LED工作模式: heartbeat(呼吸闪烁), alarm(急闪报警), on(常亮), off(关闭)"
              }
            },
            "required": ["mode"]
          }
        }
      }
    ])RAW";

    ret = rkllm_set_function_tools(handle, sys_prompt, tools_json, "<tool_response>");

    RKLLMInferParam infer{};
    infer.mode = RKLLM_INFER_GENERATE;
    infer.keep_history = 1;

    std::cout << "\n[User] 请帮我把开发板上的 LED 设置为急闪报警模式！" << std::endl;
    std::cout << "[Agent 思考与输出:]" << std::endl;
    g_output_buffer.clear();

    RKLLMInput input1{};
    input1.role = "user";
    input1.input_type = RKLLM_INPUT_PROMPT;
    input1.prompt_input = "请帮我把开发板上的 LED 设置为急闪报警模式！";
    rkllm_run(handle, &input1, &infer, nullptr);

    rkllm_destroy(handle);
    return 0;
}
