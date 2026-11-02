#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <queue> // C++ 标准队列（传送带）

// 模拟一帧摄像头的图像数据包
struct ImageFrame {
    int frame_id;
    int width;
    int height;
};

std::queue<ImageFrame> frame_queue; // 工业级公共传送带队列
int total_frames = 9;               // 今天总共拍 9 帧
int finished_frames = 0;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;

// 3 个 NPU 核心线程
void* npu_core_worker(void* arg) {
    int core_id = *(int*)arg;

    while (1) {
        pthread_mutex_lock(&lock);

        // 如果传送带空了，且还没全部完工，就休眠等门铃
        while (frame_queue.empty() && finished_frames < total_frames) {
            pthread_cond_wait(&cond, &lock);
        }

        // 完工下班
        if (finished_frames >= total_frames && frame_queue.empty()) {
            pthread_mutex_unlock(&lock);
            break;
        }

        // ★ 从传送带头端取出一帧
        ImageFrame frame = frame_queue.front();
        frame_queue.pop();
        
        pthread_mutex_unlock(&lock); // 取完立刻开门！

        // 模拟硬件 NPU 推理（Core 0 / 1 / 2 真正并行）
        printf("  [RK3588 NPU Core #%d] 正在推理 Frame #%d (%dx%d)...\n", 
               core_id, frame.frame_id, frame.width, frame.height);
        usleep(400000); // 模拟推理 0.4 秒

        pthread_mutex_lock(&lock);
        finished_frames++;
        pthread_mutex_unlock(&lock);
    }

    printf("  [RK3588 NPU Core #%d] 下班打卡！\n", core_id);
    return NULL;
}

int main() {
    pthread_t cores[3];
    int core_ids[3] = {0, 1, 2}; // 3 个 NPU 物理核

    for (int i = 0; i < 3; i++) {
        pthread_create(&cores[i], NULL, npu_core_worker, &core_ids[i]);
    }

    // 摄像头线程：连续快拍 9 帧，塞入传送带
    printf("[摄像头采集] 开始高帧率推送图像流...\n");
    for (int i = 1; i <= total_frames; i++) {
        usleep(150000); // 摄像头拍照快 (0.15秒一张)，NPU 推理慢 (0.4秒一张)

        pthread_mutex_lock(&lock);
        ImageFrame img = {i, 640, 640};
        frame_queue.push(img); // 塞进队列缓冲池
        printf("[摄像头采集] 拍摄 Frame #%d -> 塞入传送带 (当前队列堆积: %lu 张) -> 唤醒 NPU\n", 
               i, frame_queue.size());
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&lock);
    }

    for (int i = 0; i < 3; i++) {
        pthread_join(cores[i], NULL);
    }

    printf(">>> 全部 9 帧视频流由 RK3588 三核调度池完整消解！<<<\n");
    return 0;
}
