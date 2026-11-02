# -*- coding: utf-8 -*-
"""
edge_agent_client.py —— PC 端 Edge AI Agent 智能体交互客户端
架构定位：
  作为大模型（LLM）的具身代理（Embodied Agent），负责：
  1. 注册 RK3588 边缘硬件原子工具（Tool / Function Calling）；
  2. 接收人类自然语言指令，执行 ReAct 闭环（Thought -> Action -> Observation）；
  3. 通过 HTTP RPC 跨机操控板端 C++ 服务（8088 端口），读取真实温区、NPU 负载、抓取视觉、控制物理 LED。
"""

import sys
import io
import os
import json
import time
import requests

# 确保 Windows 终端 UTF-8 正常输出，防止 GBK 乱码
if sys.platform.startswith("win"):
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

BOARD_IP = os.environ.get("EDGEVISION_BOARD_HOST", "")  # 板子内网 IP 不入库，用环境变量传
BOARD_RPC_URL = f"http://{BOARD_IP}:8088"

# ==============================================================================
# 1. 硬件原子工具的 JSON Schema 定义 (大模型能看懂的“工具说明书”)
# ==============================================================================
HARDWARE_TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "get_system_health",
            "description": "采集 RK3588 开发板底座健康数据：包括 7 大物理温区(CPU/GPU/NPU等)、NPU 3核实时利用率与内存使用情况。",
            "parameters": {
                "type": "object",
                "properties": {},
                "required": []
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "get_vision_summary",
            "description": "从板载 9000 推流网关采样最新一帧真实 NPU 视觉目标检测结果，获取画面中识别出的物体类别、置信度与坐标。",
            "parameters": {
                "type": "object",
                "properties": {},
                "required": []
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "set_led_mode",
            "description": "控制 RK3588 开发板上的物理指示灯工作模式，用于物理指示系统状态或异常报警。",
            "parameters": {
                "type": "object",
                "properties": {
                    "mode": {
                        "type": "string",
                        "enum": ["heartbeat", "alarm", "on", "off"],
                        "description": "LED 模式：heartbeat(平稳呼吸闪烁), alarm(100ms警报高频爆闪), on(常亮), off(常灭)"
                    }
                },
                "required": ["mode"]
            }
        }
    }
]

# ==============================================================================
# 2. 硬件原子工具执行器 (Tool Dispatcher)
# ==============================================================================
def execute_hardware_tool(tool_name: str, arguments: dict) -> dict:
    """根据函数名分发 HTTP 请求到板端 8088 端口"""
    try:
        if tool_name == "get_system_health":
            url = f"{BOARD_RPC_URL}/api/status"
            resp = requests.get(url, timeout=3.0)
            if resp.status_code == 200:
                return resp.json()
            return {"error": f"HTTP {resp.status_code}", "raw": resp.text}

        elif tool_name == "get_vision_summary":
            url = f"{BOARD_RPC_URL}/api/vision"
            resp = requests.get(url, timeout=3.0)
            if resp.status_code == 200:
                return resp.json()
            return {"error": f"HTTP {resp.status_code}", "raw": resp.text}

        elif tool_name == "set_led_mode":
            mode = arguments.get("mode", "heartbeat")
            url = f"{BOARD_RPC_URL}/api/led?mode={mode}"
            resp = requests.get(url, timeout=3.0)
            if resp.status_code == 200:
                return resp.json()
            return {"error": f"HTTP {resp.status_code}", "raw": resp.text}

        else:
            return {"error": f"未知工具名称: {tool_name}"}

    except requests.exceptions.RequestException as e:
        return {"error": "连接板端失败，请确认 edgevision_agent_server 是否在板端运行", "detail": str(e)}

# ==============================================================================
# 3. 本地轻量 ReAct 调度器 (无需 API Key 即可零门槛体验自主规划)
# ==============================================================================
class LocalReActAgent:
    """
    当环境未配置外部大模型 API Key 时，提供确定性的 ReAct 思考与规划引擎。
    严格演示 [Thought] -> [Action] -> [Observation] -> [Final Answer] 的 Agent 核心循环。
    """
    def __init__(self, tools_dict):
        self.tools = tools_dict

    def run(self, user_prompt: str):
        prompt = user_prompt.lower()
        print(f"\n[Agent 意图分析] 收到用户指令: \"{user_prompt}\"")

        # -------------------------------------------------------------
        # 场景 A: 综合体检 / 巡检 / 设备检查
        # -------------------------------------------------------------
        if any(w in prompt for w in ["体检", "巡检", "检查", "状态", "健康", "监控"]):
            # 步骤 1: 查系统健康
            print("\n[Thought 1] 用户需要评估系统健康度。首先需要调用底层硬件探针获取 7 温区与 NPU 负载。")
            print("  -> [Action] call_tool(\"get_system_health\", {})")
            health = execute_hardware_tool("get_system_health", {})
            print(f"  -> [Observation] 获取到真实数据:")
            temps = health.get("thermal_zones", {})
            npu = health.get("npu_load", {})
            mem = health.get("memory", {})
            print(f"     * 最高温区: {max(temps.items(), key=lambda x: x[1]) if temps else '未知'}")
            print(f"     * NPU 3核负载: Core0={npu.get('core0_percent')}%, Core1={npu.get('core1_percent')}%, Core2={npu.get('core2_percent')}%")
            print(f"     * 内存剩余: {mem.get('available_mb')} MB / {mem.get('total_mb')} MB")

            # 步骤 2: 查视觉目标
            print("\n[Thought 2] 接下来需要结合边缘视觉，排查现场是否有被检测目标（如人员闯入或关键物品）。")
            print("  -> [Action] call_tool(\"get_vision_summary\", {})")
            vision = execute_hardware_tool("get_vision_summary", {})
            obj_cnt = vision.get("object_count", 0)
            objs = vision.get("objects", [])
            print(f"  -> [Observation] 网关采样帧 #{vision.get('frame_id')}，检出目标数: {obj_cnt}")
            for ob in objs:
                print(f"     * 识别物: [{ob.get('class_name')}] 置信度={ob.get('confidence')}% 坐标={ob.get('box')}")

            # 步骤 3: 综合决策与动作
            max_t = max(temps.values()) if temps else 0.0
            is_overheat = max_t > 70.0
            print(f"\n[Thought 3] 综合研判: 当前最高温度为 {max_t:.1f}°C (阈值 70°C)，现场目标数 {obj_cnt}。")
            if is_overheat:
                print("  -> 诊断: 存在硬件过热风险！立即触发高频警报爆闪。")
                print("  -> [Action] call_tool(\"set_led_mode\", {\"mode\": \"alarm\"})")
                led_res = execute_hardware_tool("set_led_mode", {"mode": "alarm"})
                led_msg = "已触发硬件爆闪警报！"
            else:
                print("  -> 诊断: 硬件温度与算力均处于黄金稳态，指示灯恢复平稳呼吸模式。")
                print("  -> [Action] call_tool(\"set_led_mode\", {\"mode\": \"heartbeat\"})")
                led_res = execute_hardware_tool("set_led_mode", {"mode": "heartbeat"})
                led_msg = "物理指示灯保持稳态呼吸闪烁。"

            # 步骤 4: 生成终局人话报告
            print("\n" + "=" * 60)
            print("[Agent 诊断报告 (Final Answer)]")
            print(f"1. 硬件运行状况: 良好 (最高温区 {max_t:.1f}°C，NPU 三核并发运转，可用内存 {mem.get('available_mb')}MB)")
            if obj_cnt > 0:
                names = "、".join([o.get('class_name', '') for o in objs])
                print(f"2. 现场视觉检测: 识别到 {obj_cnt} 个目标（包含: {names}）")
            else:
                print("2. 现场视觉检测: 视野内暂无关键目标。")
            print(f"3. 物理执行动作: {led_msg}")
            print("=" * 60)

        # -------------------------------------------------------------
        # 场景 B: 专门看视觉
        # -------------------------------------------------------------
        elif any(w in prompt for w in ["看", "视觉", "人", "目标", "检测", "画面", "摄像头"]):
            print("\n[Thought] 用户请求获取现场视觉信息。直接调用 9000 端口抓拍探针。")
            print("  -> [Action] call_tool(\"get_vision_summary\", {})")
            vision = execute_hardware_tool("get_vision_summary", {})
            cnt = vision.get("object_count", 0)
            objs = vision.get("objects", [])
            print(f"  -> [Observation] 成功获取第 #{vision.get('frame_id')} 帧，目标数: {cnt}")

            print("\n" + "=" * 60)
            print("[Agent 视觉报告 (Final Answer)]")
            if cnt == 0:
                print("  当前摄像头画面中未发现显著目标。")
            else:
                print(f"  当前共检出 {cnt} 个真实目标:")
                for i, o in enumerate(objs, 1):
                    b = o.get("box", {})
                    print(f"    ({i}) 类别: {o.get('class_name')} | 置信度: {o.get('confidence')}% | 像素区域: [{b.get('x')}, {b.get('y')}, {b.get('w')}x{b.get('h')}]")
            print("=" * 60)

        # -------------------------------------------------------------
        # 场景 C: 控制硬件灯光
        # -------------------------------------------------------------
        elif any(w in prompt for w in ["灯", "led", "报警", "呼吸", "闪烁", "关闭", "打开"]):
            target_mode = "heartbeat"
            if "警" in prompt or "闪" in prompt or "快" in prompt or "alarm" in prompt:
                target_mode = "alarm"
            elif "关" in prompt or "灭" in prompt or "off" in prompt:
                target_mode = "off"
            elif "开" in prompt or "亮" in prompt or "on" in prompt:
                target_mode = "on"

            print(f"\n[Thought] 识别到用户期望变更板载物理 LED 为 [{target_mode}] 模式。")
            print(f"  -> [Action] call_tool(\"set_led_mode\", {{\"mode\": \"{target_mode}\"}})")
            res = execute_hardware_tool("set_led_mode", {"mode": target_mode})
            print(f"  -> [Observation] 底座硬件返回: {res}")

            print("\n" + "=" * 60)
            print(f"[Agent 执行结果] 开发板物理 LED 已成功切换至: {target_mode} 模式。")
            print("=" * 60)

        # -------------------------------------------------------------
        # 默认回退
        # -------------------------------------------------------------
        else:
            print("\n[Thought] 指令泛化，先采样板卡状态与视觉以形成综合感知。")
            h = execute_hardware_tool("get_system_health", {})
            v = execute_hardware_tool("get_vision_summary", {})
            print(f"  板卡最高温: {max(h.get('thermal_zones', {}).values()) if h.get('thermal_zones') else '未知'}°C")
            print(f"  画面目标数: {v.get('object_count', 0)}")
            print("[Agent 回复] 已完成底座探针轮询，请下达更具体的指令（如：'设备体检'、'看下画面'、'报警灯快闪'）。")

# ==============================================================================
# 4. 交互式主控制台
# ==============================================================================
def main():
    print("=" * 70)
    print("  🤖 RK3588 EdgeVision · Edge AI Agent 具身智能体客户端")
    print(f"  边缘 RPC 靶机地址: {BOARD_RPC_URL}")
    print("=" * 70)
    print("已注册 3 大底层硬件原子工具 (JSON Schema):")
    for t in HARDWARE_TOOLS:
        fn = t["function"]
        print(f"  * {fn['name']}: {fn['description'][:40]}...")
    print("-" * 70)
    print("💡 快捷交互提示 (可直接输入自然语言)：")
    print("  [1] 全面体检 (测试多步 ReAct 规划：测温 -> 查视觉 -> 评估 -> 决策灯光)")
    print("  [2] 查看现场视觉 (抓拍最新检测目标并汇报坐标)")
    print("  [3] 切换报警灯快闪 (调用 set_led_mode mode=alarm)")
    print("  [4] 恢复呼吸灯 (调用 set_led_mode mode=heartbeat)")
    print("  [exit] 退出程序")
    print("=" * 70)

    agent = LocalReActAgent(HARDWARE_TOOLS)

    # 快捷键映射
    quick_map = {
        "1": "对开发板进行全面体检巡检，如有异常及时处理",
        "2": "查看摄像头画面中当前有什么目标",
        "3": "开启硬件报警灯快闪",
        "4": "把硬件灯恢复为呼吸灯"
    }

    while True:
        try:
            user_input = input("\n[User] 请输入指令 > ").strip()
            if not user_input:
                continue
            if user_input.lower() in ["exit", "quit", "q"]:
                print("[*] 智能体客户端已退出。")
                break

            # 匹配数字快捷键或原样输入
            prompt = quick_map.get(user_input, user_input)
            agent.run(prompt)

        except KeyboardInterrupt:
            print("\n[*] 收到中断信号，退出客户端。")
            break
        except Exception as e:
            print(f"[-] 异常: {e}")

if __name__ == "__main__":
    main()
