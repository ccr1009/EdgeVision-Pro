import sys

file_path = r'D:\a研究生\项目\rk3588项目\edgevision-rk3588\deploy\cpp\src\edgevision_dashboard_main.cpp'
with open(file_path, 'r', encoding='utf-8') as f:
    content = f.read()

# 1. Update args parsing for --cam
old_arg = '        else if (a == "--video") args.video = next("--video");'
new_arg = '''        else if (a == "--video") args.video = next("--video");
        else if (a == "--cam") args.video = next("--cam");'''
if old_arg in content:
    content = content.replace(old_arg, new_arg, 1)
    print('Updated args parsing.')

# 2. Update usage text
old_usage_target = '--video'
for line in content.split('\n'):
    if '--video' in line and 'highway_traffic.mp4' in line:
        new_line = '    printf("  --video PATH     视频文件或摄像头设备路径 (如 /dev/video83 或 data/videos/highway_traffic.mp4)\\n");\n    printf("  --cam DEV        摄像头设备节点别名 (等价于 --video /dev/videoXX)\\n");'
        content = content.replace(line, new_line, 1)
        print('Updated usage text.')
        break

# 3. Update VideoCapture open
idx_open = content.find('cv::VideoCapture cap(args.video);')
if idx_open != -1:
    idx_step1 = content.rfind('// ---------------- 1.', 0, idx_open)
    idx_step2 = content.find('// ---------------- 2.', idx_open)

    new_step1 = '''// ---------------- 1. 视频源 / 摄像头 ----------------
    bool is_cam = (args.video.rfind("/dev/video", 0) == 0);
    cv::VideoCapture cap;
    int src_w = 1280, src_h = 720;
    double src_fps = 30.0;
    int src_frames = 0;

    if (is_cam) {
        cap.open(args.video, cv::CAP_V4L2);
        if (!cap.isOpened()) {
            fprintf(stderr, "[FAIL] 无法打开摄像头设备: %s\\n", args.video.c_str());
            return 1;
        }
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);
        cap.set(cv::CAP_PROP_FPS, 30);

        char cmd[128];
        snprintf(cmd, sizeof(cmd), "v4l2-ctl -d %s --set-ctrl=exposure_auto_priority=0 >/dev/null 2>&1", args.video.c_str());
        system(cmd);

        // 硬件预热 15 帧确保自动曝光收敛
        cv::Mat warm;
        for (int i = 0; i < 15; ++i) {
            cap.read(warm);
        }
        src_w = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
        src_h = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
        printf("[+] 摄像头就绪: %s\\n", args.video.c_str());
        printf("    硬件采集 %dx%d @ 30.0 fps (MJPG 硬件流已锁定)\\n", src_w, src_h);
    } else {
        cap.open(args.video);
        if (!cap.isOpened()) {
            fprintf(stderr, "[FAIL] 打不开视频: %s\\n", args.video.c_str());
            return 1;
        }
        src_w = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
        src_h = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
        src_fps = cap.get(cv::CAP_PROP_FPS);
        src_frames = (int)cap.get(cv::CAP_PROP_FRAME_COUNT);
        printf("[+] 视频文件: %s\\n", args.video.c_str());
        printf("    源 %dx%d @ %.1f fps, %d 帧, 循环播放\\n", src_w, src_h, src_fps, src_frames);
    }

    '''
    content = content[:idx_step1] + new_step1 + content[idx_step2:]
    print('Updated step 1 VideoCapture open.')

# 4. Update frame read
idx_read = content.find('if (!cap.read(raw) || raw.empty()) {')
if idx_read != -1:
    idx_read_close = content.find('capture_ms = now_ms() - t0;', idx_read)
    new_read = '''if (!cap.read(raw) || raw.empty()) {
            if (is_cam) {
                usleep(5000);
                continue;
            }
            // 读完一圈 -> 回到第 0 帧
            cap.set(cv::CAP_PROP_POS_FRAMES, 0);
            ++loop_index;
            if (!cap.read(raw) || raw.empty()) {
                fprintf(stderr, "[FAIL] 读取视频帧失败，重新回到开头依然失败，退出\\n");
                break;
            }
        }
        '''
    content = content[:idx_read] + new_read + content[idx_read_close:]
    print('Updated frame read logic.')

# 5. Update pacing
idx_pace = content.find('if (args.pace) {')
if idx_pace != -1:
    content = content[:idx_pace] + 'if (!is_cam && args.pace) {' + content[idx_pace+len('if (args.pace) {'):]
    print('Updated pacing check.')

with open(file_path, 'w', encoding='utf-8') as f:
    f.write(content)
print('Successfully saved updated edgevision_dashboard_main.cpp')
