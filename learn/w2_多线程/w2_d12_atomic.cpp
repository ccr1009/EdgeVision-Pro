#include <stdio.h>
#include <pthread.h>
#include <atomic> // ★ 引入原子操作头文件

// ★ 把普通的 int 换成原子 int，连 mutex 锁都不需要定义了！
std::atomic<int> count(0);

void* add_job(void* arg) {
    for (int i = 0; i < 1000000; i++) {
        count++; // ★ 硬件级一步到位，不可被打断！
    }
    return NULL;
}

int main() {
    pthread_t tid1, tid2;

    pthread_create(&tid1, NULL, add_job, NULL);
    pthread_create(&tid2, NULL, add_job, NULL);

    pthread_join(tid1, NULL);
    pthread_join(tid2, NULL);

    printf("不用 mutex 锁，用 atomic 算出来的是: %d\n", count.load());
    return 0;
}
