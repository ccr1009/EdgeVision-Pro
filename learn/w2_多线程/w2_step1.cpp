#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

// 工人的工作函数：格式固定是 void* 函数名(void* arg)
void* worker(void* arg) {
    int id = *(int*)arg; // 把传进来的 void* 万能指针转回 int*，再取值
    for (int i = 1; i <= 3; i++) {
        printf("[工人线程 #%d] 正在搬第 %d 块砖...\n", id, i);
        sleep(1); // 模拟干活耗时 1 秒
    }
    printf("[工人线程 #%d] 干完收工！\n", id);
    return NULL;
}

int main() {
    pthread_t tid; // 用来存工人的“工号”
    int worker_id = 888;

    printf("[主线程老板] 准备招工人干活...\n");

    // 1. 创建线程：把工号存进 &tid，默认属性 NULL，去跑 worker 函数，把 &worker_id 传给它
    int ret = pthread_create(&tid, NULL, worker, &worker_id);
    if (ret != 0) {
        printf("招工失败！错误码: %d\n", ret);
        return 1;
    }

    // 2. 老板等待工人干完活（把下面横线换成上面创建的工号变量！）
    pthread_join(______, NULL);

    printf("[主线程老板] 工人下班了，老板锁门下班！\n");
    return 0;
}


