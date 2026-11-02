#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

int task_id = 0;       // 任务编号（相当于第几张待推理的图片）
int total_tasks = 6;   // 今天一共要处理 6 张图片
int finished_tasks = 0;// 已经处理完的图片数

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;

// 3 个 NPU 工人线程共享这份干活代码
void* npu_worker(void* arg) {
    int core_id = *(int*)arg; // 拿到自己的核心工号：0, 1, 2

    while (1) {
        pthread_mutex_lock(&lock);

        // 没新任务，且还没全部收工，就睡觉等门铃
        while (task_id == 0 && finished_tasks < total_tasks) {
            pthread_cond_wait(&cond, &lock);
        }

        // 如果全部 6 张图都处理完了，工人就可以光荣下班了
        if (finished_tasks >= total_tasks) {
            pthread_mutex_unlock(&lock);
            break;
        }

        // 抢到了这张任务！
        int current_img = task_id;
        task_id = 0; // 抢走后，桌子变空
        pthread_mutex_unlock(&lock); // ★ 抢到手后立刻开门！不要霸占着门去推理！

        // 模拟 NPU 推理耗时（在门外并行推理，互不耽误！）
        printf("  [NPU Core #%d] 抢到第 %d 号图片，正在并行推理中...\n", core_id, current_img);
        usleep(500000); // 假装推理耗时 0.5 秒

        pthread_mutex_lock(&lock);
        finished_tasks++;
        pthread_mutex_unlock(&lock);
    }

    printf("  [NPU Core #%d] 今日全部任务完成，打卡下班！\n", core_id);
    return NULL;
}

int main() {
    pthread_t workers[3];
    int core_ids[3] = {0, 1, 2}; // 对应 RK3588 的 3 个 NPU 核心

    // 1. 派出 3 个 NPU 工人就位！
    for (int i = 0; i < 3; i++) {
        pthread_create(&workers[i], NULL, npu_worker, &core_ids[i]);
    }

    // 2. 模拟摄像头每隔 0.3 秒拍一张，连续派发 6 张图片
    for (int i = 1; i <= total_tasks; i++) {
        usleep(300000); // 0.3 秒拍一张
        pthread_mutex_lock(&lock);
        task_id = i;
        printf("[摄像头] 派发第 %d 号图片 -> 按响门铃叫醒空闲核心！\n", i);
        pthread_cond_signal(&cond); // 唤醒一个空闲核心抢单
        pthread_mutex_unlock(&lock);
    }

    // 3. 等 3 个核心全部干完下班
    for (int i = 0; i < 3; i++) {
        pthread_join(workers[i], NULL);
    }

    printf(">>> 全部 6 张图片由 3 个 NPU 核心并行搞定！<<<\n");
    return 0;
}
