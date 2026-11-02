#include <stdio.h>
#include <pthread.h>

int count = 0;
// ★ 1. 在仓库门口装一把互斥锁
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void* add_job(void* arg) {
    for (int i = 0; i < 1000000; i++) {
        pthread_mutex_lock(&lock);   // ★ 2. 改账本前：锁门！
        count++;
        pthread_mutex_unlock(&lock); // ★ 3. 改完账本后：开门！
    }
    return NULL;
}

int main() {
    pthread_t tid1, tid2;

    pthread_create(&tid1, NULL, add_job, NULL);
    pthread_create(&tid2, NULL, add_job, NULL);

    pthread_join(tid1, NULL);
    pthread_join(tid2, NULL);

    printf("加锁后，实际算出来的是: %d\n", count);
    return 0;
}
