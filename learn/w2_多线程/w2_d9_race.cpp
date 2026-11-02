#include <stdio.h>
#include <pthread.h>

int count = 0; // 仓库里的公共账本（所有线程共享）

void* add_job(void* arg) {
    for (int i = 0; i < 1000000; i++) {
        count++; // 往账本上 +1，循环 100 万次
    }
    return NULL;
}

int main() {
    pthread_t tid1, tid2;

    // 同时派出 2 个工人去记账
    pthread_create(&tid1, NULL, add_job, NULL);
    pthread_create(&tid2, NULL, add_job, NULL);

    // 等 2 个工人都干完
    pthread_join(tid1, NULL);
    pthread_join(tid2, NULL);

    printf("理论结果应该是: 2000000\n");
    printf("实际算出来的是: %d\n", count);
    return 0;
}
