import socket
import struct
import time
import sys
import os

# 避免 Windows 终端 GBK 编码报错
if sys.platform == "win32":
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

BOARD_IP = os.environ.get("EDGEVISION_BOARD_HOST", "")  # 板子内网 IP 不入库，用环境变量传
PORT = 9000

# -------------------------------------------------------------
# 协议格式 (与开发板 C++ 结构体 #pragma pack(1) 严格对齐)：
# DetectionHeader: frame_id (uint32), bbox_count (uint32) -> 8 字节
# BBox: class_id (int32), score (float), x (float), y (float), w (float), h (float) -> 24 字节
# MAX_BBOXES = 10
# FrameResult 总大小 = 8 + 10 * 24 = 248 字节
# -------------------------------------------------------------
MAX_BBOXES = 10
PACKET_SIZE = 8 + MAX_BBOXES * 24  # 248 字节
HEADER_FMT = "<II"                  # 2 个 uint32
BBOX_FMT = "<ifffff"                # 1 个 int32 + 5 个 float

COCO_CLASSES = [
    "人", "自行车", "汽车", "摩托车", "飞机", "公交车", "火车", "卡车", "船", "红绿灯",
    "消防栓", "停止标志", "停车计时器", "长凳", "鸟", "猫", "狗", "马", "羊", "牛",
    "大象", "熊", "斑马", "长颈鹿", "背包", "雨伞", "手提包", "领带", "手提箱", "飞盘",
    "滑雪板", "单板滑雪", "运动球", "风筝", "棒球棒", "棒球手套", "滑板", "冲浪板", "网球拍", "瓶子",
    "红酒杯", "水杯", "叉子", "刀", "勺子", "碗", "香蕉", "苹果", "三明治", "橙子",
    "西兰花", "胡萝卜", "热狗", "披萨", "甜甜圈", "蛋糕", "椅子", "沙发", "盆栽", "床",
    "餐桌", "马桶", "电视/显示器", "笔记本电脑", "鼠标", "遥控器", "键盘", "手机", "微波炉", "烤箱",
    "烤面包机", "水槽", "冰箱", "书", "时钟", "花瓶", "剪刀", "泰迪熊", "吹风机", "牙刷"
]

def get_class_name(cid: int) -> str:
    if 0 <= cid < len(COCO_CLASSES):
        return COCO_CLASSES[cid]
    return f"未知({cid})"

def main():
    print("=" * 66)
    print("  [EdgeVision] PC 上位机实时解析接收器 V2.0")
    print(f"  目标开发板 IP: {BOARD_IP}:{PORT}")
    print(f"  推流协议规格: 248 字节定长帧 (最大 {MAX_BBOXES} 目标, 支持 COCO 80 类)")
    print("=" * 66)

    while True:
        try:
            print(f"[*] 正在尝试连接 RK3588 网关 ({BOARD_IP}:{PORT})...")
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect((BOARD_IP, PORT))
            print("[+] 连接成功！开始实时接收板端 3核 NPU 真实目标推流...\n")

            frame_count = 0
            t_start = time.time()

            while True:
                # 准确接收定长 248 字节结构体数据
                data = b""
                while len(data) < PACKET_SIZE:
                    chunk = sock.recv(PACKET_SIZE - len(data))
                    if not chunk:
                        raise ConnectionResetError("开发板断开连接")
                    data += chunk

                # 解包帧头 (8 字节)
                frame_id, bbox_count = struct.unpack_from(HEADER_FMT, data, 0)
                frame_count += 1

                # 解包有效目标框 (上限为 MAX_BBOXES)
                boxes = []
                offset = 8
                valid_boxes = min(bbox_count, MAX_BBOXES)
                for _ in range(valid_boxes):
                    cid, score, x, y, w, h = struct.unpack_from(BBOX_FMT, data, offset)
                    boxes.append((cid, score, x, y, w, h))
                    offset += 24

                fps = frame_count / (time.time() - t_start + 1e-5)

                if valid_boxes == 0:
                    print(f"  [帧 #{frame_id:05d} | {fps:4.1f} FPS] 目标: (无检出)")
                else:
                    box_str = " | ".join([
                        f"[{get_class_name(c)}: {s*100:.0f}%, 框=({x:.0f},{y:.0f},{w:.0f},{h:.0f})]"
                        for c, s, x, y, w, h in boxes
                    ])
                    print(f"  [帧 #{frame_id:05d} | {fps:4.1f} FPS] 检出 {valid_boxes} 个 -> {box_str}")

        except (ConnectionRefusedError, TimeoutError):
            print("[-] 连接失败，等待 2 秒后重试...")
            time.sleep(2)
        except (ConnectionResetError, BrokenPipeError):
            print("\n[-] 板端推流服务断开，等待重新连接...")
            time.sleep(2)
        except KeyboardInterrupt:
            print("\n[*] 上位机客户端已手动退出。")
            break

if __name__ == "__main__":
    main()
