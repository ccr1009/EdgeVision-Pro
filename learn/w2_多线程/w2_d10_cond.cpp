#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

int image_id = 0; // 桌子上的照片编号（0 表示没照片）
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; // 门锁
pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;  // ★ 叫号门铃（条件变量）

// 消费者：NPU 推理线程
void* npu_consumer(void* arg) {
    for (int i = 1; i <= 3; i++) {
        pthread_mutex_lock(&lock); // 1. 先锁门看桌子

        // 2. 如果桌子上没照片（image_id == 0），就躺下睡觉等门铃！
        while (image_id == 0) {
            printf("  [NPU 线程] 桌子上没照片，我先睡会儿等门铃...\n");
            pthread_cond_wait(&cond, &lock); // ★ 自动开门睡觉；被门铃叫醒后自动重新锁门！
        }

        // 3. 被门铃叫醒了！拿走照片去推理
        printf("  [NPU 线程] 叮咚醒来！拿到第 %d 号照片，完成 YOLO 推理！\n", image_id);
        image_id = 0; // 照片处理完了，清空桌子

        pthread_mutex_unlock(&lock); // 4. 开门
    }
    return NULL;
}

// 生产者：摄像头拍照线程
void* camera_producer(void* arg) {
    for (int i = 1; i <= 3; i++) {
        sleep(1); // 模拟摄像头每隔 1 秒拍出一张新照片

        pthread_mutex_lock(&lock);
        image_id = i; // 把第 i 张照片放到桌子上
        printf("[摄像头线程] 咔嚓！拍好第 %d 号照片放上桌，按响门铃！\n", i);
        pthread_cond_signal(&cond); // ★ 叮咚！按门铃叫醒正在睡觉的 NPU 线程
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

int main() {
    pthread_t cam_tid, npu_tid;

    pthread_create(&npu_tid, NULL, npu_consumer, NULL);    // NPU 先上岗等着
    pthread_create(&cam_tid, NULL, camera_producer, NULL); // 摄像头开始每隔1秒拍照

    pthread_join(cam_tid, NULL);
    pthread_join(npu_tid, NULL);

    printf("全部 3 张照片采集+推理流水线完成！\n");
    return 0;
}
