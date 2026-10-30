// lab/ipc/spin_cost.cpp —— 等待策略的 CPU 代价(公平对照版)
//
// 场景对齐真实链路:
//   生产者 = 摄像头(持续出帧,想尽快塞进队列)
//   消费者 = NPU(每帧要花 work_ms 推理,期间完全不碰共享内存)
//
// 缓冲区有限(默认 16 帧),所以生产者一定会「等」。等待有四种写法:
//   sem      : sem_wait        → 睡进内核,不占 CPU,每次唤醒一次系统调用
//   spin     : 纯自旋           → 不睡,CPU 100% 烧着等
//   yield    : 自旋一阵后让出   → 折中
//   adaptive : 先自旋,超阈值转睡 → 短等待省系统调用,长等待不烧 CPU
//
// ⚠ 关键设计(上一版错了,这里必须讲清):
//   环形缓冲区要三个计数器,不是两个:
//     head       生产者写,写之前 head++
//     tail_claim 消费者认领(仅用于寻址读哪个槽)
//     tail_done  消费者**处理完**后才推进 ← 生产者看这个才知道"有空位"
//   如果生产者看 tail_claim 判空位,那"空位"在消费者读到数据的一瞬间就有了,
//   等于完全不等待 —— 这和 sem 版(处理完才 post)不是同一个语义,
//   对照组就废了。上一版正是踩了这个坑:spin 的 wall 反而比 sem 长。
//
// 编译: g++ -O3 -std=c++17 -pthread spin_cost.cpp -o spin_cost -lrt
// 用法: ./spin_cost [work_ms] [msgs] [slot] [cap]
//
// 输出: producer_cpu_ms 越低越好;cpu_share_pct=100 表示生产者一直在烧 CPU。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static double now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static double cpu_ms(const struct rusage& ru) {
    return (double)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 +
           (double)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
}

static inline void cpu_relax() {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    asm volatile("pause" ::: "memory");
#endif
}

// 环形缓冲区布局(与 ipc_bench.cpp 的字段顺序保持一致,只是多了 tail_done)
constexpr size_t HDR = 256;
constexpr size_t OFF_SLOT = 0;
constexpr size_t OFF_CAP = 8;
constexpr size_t OFF_CCPU = 16;   // 回传消费者 CPU 时间(us)
constexpr size_t OFF_HEAD = 64;   // 生产者写
constexpr size_t OFF_TCLAIM = 128; // 消费者写(寻址)
constexpr size_t OFF_TDONE = 192;  // 消费者写(处理完)← 生产者看这个

struct Ctx {
    size_t slot = 0;
    size_t cap = 0;
    uint64_t* head = nullptr;
    uint64_t* tclaim = nullptr;
    uint64_t* tdone = nullptr;
    uint64_t* ccpu = nullptr;
    char* data = nullptr;
    sem_t* sem_space = nullptr;
    sem_t* sem_data = nullptr;
    char* slot_ptr(uint64_t i) { return data + (size_t)(i % cap) * slot; }
};

enum Strategy { SEM_WAIT, PURE_SPIN, SPIN_YIELD, ADAPTIVE };
static const char* strategy_name(Strategy s) {
    switch (s) {
        case SEM_WAIT: return "sem";
        case PURE_SPIN: return "spin";
        case SPIN_YIELD: return "yield";
        case ADAPTIVE: return "adaptive";
    }
    return "?";
}
static bool uses_sem(Strategy s) { return s == SEM_WAIT || s == ADAPTIVE; }

// 生产者等「有空位」。判据统一用 tail_done —— 与 sem 版语义一致。
static bool wait_space(Ctx& c, Strategy s, uint64_t head) {
    double t0 = now_us();
    switch (s) {
        case SEM_WAIT:
            while (sem_wait(c.sem_space) != 0) {
                if (errno != EINTR) return false;
            }
            return true;

        case PURE_SPIN:
            for (;;) {
                uint64_t d = __atomic_load_n(c.tdone, __ATOMIC_ACQUIRE);
                if (head - d < c.cap) return true;
                cpu_relax();
                if (now_us() - t0 > 3e6) return false;
            }

        case SPIN_YIELD:
            for (int i = 0;; i++) {
                uint64_t d = __atomic_load_n(c.tdone, __ATOMIC_ACQUIRE);
                if (head - d < c.cap) return true;
                if (i < 64) {
                    cpu_relax();
                } else {
                    sched_yield();
                }
                if ((i & 0x3FF) == 0 && now_us() - t0 > 3e6) return false;
            }

        case ADAPTIVE:
            for (int i = 0; i < 2000; i++) {
                uint64_t d = __atomic_load_n(c.tdone, __ATOMIC_ACQUIRE);
                if (head - d < c.cap) return true;
                cpu_relax();
            }
            while (sem_wait(c.sem_space) != 0) {
                if (errno != EINTR) return false;
            }
            return true;
    }
    return false;
}

struct Outcome {
    bool ok = false;
    double producer_cpu_ms = 0;
    double producer_wall_ms = 0;
    double consumer_cpu_ms = 0;
    bool producer_blocked = false;
};

static bool make_sem(sem_t** out, const char* name, unsigned init) {
    sem_unlink(name);
    sem_t* s = sem_open(name, O_CREAT | O_EXCL, 0600, init);
    if (s == SEM_FAILED) return false;
    *out = s;
    return true;
}

static Outcome run_once(Strategy s, int msgs, double work_ms, size_t slot, size_t cap) {
    Outcome out;
    size_t total = HDR + cap * slot;

    char shm_name[64], sn_spc[72], sn_dat[72];
    snprintf(shm_name, sizeof shm_name, "/spincost_shm_%d", (int)getpid());
    snprintf(sn_spc, sizeof sn_spc, "/spincost_spc_%d", (int)getpid());
    snprintf(sn_dat, sizeof sn_dat, "/spincost_dat_%d", (int)getpid());

    shm_unlink(shm_name);
    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return out;
    if (ftruncate(fd, (off_t)total) != 0) {
        close(fd);
        shm_unlink(shm_name);
        return out;
    }
    void* p = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        shm_unlink(shm_name);
        return out;
    }
    char* base = (char*)p;
    memset(base, 0, HDR);
    *(uint64_t*)(base + OFF_SLOT) = (uint64_t)slot;
    *(uint64_t*)(base + OFF_CAP) = (uint64_t)cap;

    sem_t* sem_space = nullptr;
    sem_t* sem_data = nullptr;
    if (!make_sem(&sem_space, sn_spc, (unsigned)cap)) {
        munmap(p, total);
        shm_unlink(shm_name);
        return out;
    }
    if (!make_sem(&sem_data, sn_dat, 0)) {
        munmap(p, total);
        shm_unlink(shm_name);
        sem_unlink(sn_spc);
        return out;
    }

    Ctx c;
    c.slot = slot;
    c.cap = cap;
    c.head = (uint64_t*)(base + OFF_HEAD);
    c.tclaim = (uint64_t*)(base + OFF_TCLAIM);
    c.tdone = (uint64_t*)(base + OFF_TDONE);
    c.ccpu = (uint64_t*)(base + OFF_CCPU);
    c.data = base + HDR;
    c.sem_space = sem_space;
    c.sem_data = sem_data;

    pid_t pid = fork();
    if (pid == 0) {
        // ---- 消费者:模拟 NPU ----
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        struct rusage r0;
        getrusage(RUSAGE_SELF, &r0);
        std::vector<char> buf(slot, 0);

        for (int i = 0; i < msgs; i++) {
            if (uses_sem(s)) {
                while (sem_wait(sem_data) != 0) {
                    if (errno != EINTR) _exit(5);
                }
            } else {
                double t0 = now_us();
                for (;;) {
                    uint64_t h = __atomic_load_n(c.head, __ATOMIC_ACQUIRE);
                    uint64_t tc = __atomic_load_n(c.tclaim, __ATOMIC_RELAXED);
                    if (h != tc) break;
                    cpu_relax();
                    if (now_us() - t0 > 3e6) _exit(6);
                }
            }

            // 认领:推进 tclaim(只用于寻址,不表示"空位已还")
            uint64_t tc = __atomic_load_n(c.tclaim, __ATOMIC_RELAXED);
            memcpy(buf.data(), c.slot_ptr(tc), slot);
            __atomic_store_n(c.tclaim, tc + 1, __ATOMIC_RELAXED);

            // 假装在推理:纯计算忙等,真实占着 CPU
            double w0 = now_us();
            while (now_us() - w0 < work_ms * 1000.0) {
                cpu_relax();
            }

            // 处理完,才还空位。sem 与 spin 都在这里发布,语义完全一致。
            __atomic_store_n(c.tdone, tc + 1, __ATOMIC_RELEASE);
            if (uses_sem(s)) sem_post(sem_space);
        }

        struct rusage r1;
        getrusage(RUSAGE_SELF, &r1);
        __atomic_store_n(c.ccpu, (uint64_t)((cpu_ms(r1) - cpu_ms(r0)) * 1000.0), __ATOMIC_RELEASE);
        _exit(0);
    }
    if (pid < 0) {
        munmap(p, total);
        shm_unlink(shm_name);
        sem_unlink(sn_spc);
        sem_unlink(sn_dat);
        return out;
    }

    // ---- 生产者:摄像头 ----
    struct rusage pr0;
    getrusage(RUSAGE_SELF, &pr0);
    std::vector<char> buf(slot, 0xCD);
    double wall0 = now_us();
    bool good = true;
    int blocked = 0;

    for (int i = 0; i < msgs; i++) {
        uint64_t h = __atomic_load_n(c.head, __ATOMIC_RELAXED);
        uint64_t d = __atomic_load_n(c.tdone, __ATOMIC_ACQUIRE);
        if (h - d >= cap) {
            blocked++;
            if (!wait_space(c, s, h)) {
                good = false;
                break;
            }
        }
        memcpy(c.slot_ptr(h), buf.data(), slot);
        __atomic_store_n(c.head, h + 1, __ATOMIC_RELEASE);
        if (uses_sem(s)) sem_post(sem_data);
    }
    double wall1 = now_us();

    if (!good) kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    struct rusage pr1;
    getrusage(RUSAGE_SELF, &pr1);

    if (good && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        out.ok = true;
        out.producer_cpu_ms = cpu_ms(pr1) - cpu_ms(pr0);
        out.producer_wall_ms = (wall1 - wall0) / 1000.0;
        out.consumer_cpu_ms = (double)__atomic_load_n(c.ccpu, __ATOMIC_ACQUIRE) / 1000.0;
        out.producer_blocked = blocked > 0;
    }

    munmap(p, total);
    shm_unlink(shm_name);
    sem_unlink(sn_spc);
    sem_unlink(sn_dat);
    return out;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    double work_ms = argc > 1 ? atof(argv[1]) : 20.0;
    int msgs = argc > 2 ? atoi(argv[2]) : 100;
    size_t slot = argc > 3 ? (size_t)atol(argv[3]) : 248;
    size_t cap = argc > 4 ? (size_t)atol(argv[4]) : 16;
    if (msgs < 4) msgs = 4;
    if (cap < 2) cap = 2;

    printf("# 生产者(摄像头)灌 %d × %zuB, 消费者(NPU)每帧 %.1f ms, 环形缓冲 %zu 帧\n",
           msgs, slot, work_ms, cap);
    printf("# 判据:producer_cpu_ms 越低越好;cpu_share_pct=100 表示生产者一直烧 CPU\n\n");
    printf("strategy\tok\tblocked\tproducer_cpu_ms\tproducer_wall_ms\tconsumer_cpu_ms\tcpu_share_pct\tus_per_msg\n");

    for (Strategy s : {SEM_WAIT, PURE_SPIN, SPIN_YIELD, ADAPTIVE}) {
        Outcome o = run_once(s, msgs, work_ms, slot, cap);
        if (!o.ok) {
            printf("%s\t0\t-\t-\t-\t-\t-\t-\n", strategy_name(s));
            fflush(stdout);
            continue;
        }
        double share = o.producer_wall_ms > 0 ? o.producer_cpu_ms / o.producer_wall_ms * 100.0 : 0;
        double us_per = (double)msgs > 0 ? o.producer_wall_ms * 1000.0 / (double)msgs : 0;
        printf("%s\t1\t%d\t%.2f\t%.1f\t%.2f\t%.1f\t%.2f\n",
               strategy_name(s), o.producer_blocked ? 1 : 0,
               o.producer_cpu_ms, o.producer_wall_ms,
               o.consumer_cpu_ms, share, us_per);
        fflush(stdout);
    }
    return 0;
}
