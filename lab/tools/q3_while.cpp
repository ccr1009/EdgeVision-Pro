// ============================================================
//  q3_spurious.cpp —— 亲眼看见「virtual wakeup」把队列搞崩
//
//  这个程序和 D20 网关里的 network_streamer 一模一样,
//  唯一区别: 第 43 行用的是 if 而不是 while。
//
//  编译: g++ -O2 -std=c++11 -pthread q3_spurious.cpp -o q3_spurious
//  运行: ./q3_spurious
// ============================================================
#include <cstdio>
#include <queue>
#include <pthread.h>
#include <unistd.h>

std::queue<int> q;                                    // 空队列
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;

// 消费者: 从队列里取任务
void* consumer(void* arg) {
    pthread_mutex_lock(&lock);

    // ★★★ 就是这一行 ★★★
    // if  = 醒来后只检查一次 -> 空队列也往下走
    // while= 醒来后不满足就继续睡
    while (q.empty()) {
        printf("[消费者] 队列空, 我先睡了...\n");
        fflush(stdout);
        pthread_cond_wait(&cond, &lock);
        printf("[消费者] 我被叫醒了!\n");
    }

    // 醒过来就直接去拿队头 —— 没人检查队列到底空不空
    int task = q.front();
    printf("[消费者] 拿到任务: %d\n", task);
    q.pop();

    pthread_mutex_unlock(&lock);
    return NULL;
}

// 生产者: 一个任务都不放, 只按一次空门铃
void* producer(void* arg) {
    sleep(1);                                          // 等消费者先睡着
    pthread_mutex_lock(&lock);
    printf("[生产者] 叮! 按了一下门铃(但一个任务都没放)\n");
    fflush(stdout);
    pthread_cond_signal(&cond);                        // 空信号
    pthread_mutex_unlock(&lock);
    return NULL;
}

int main() {
    pthread_t c, p;
    pthread_create(&c, NULL, consumer, NULL);
    pthread_create(&p, NULL, producer, NULL);
    pthread_join(p, NULL);
    pthread_join(c, NULL);
    printf("[主线程] 正常退出\n");
    return 0;
}
