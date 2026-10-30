// lab/ipc/ipc_bench.cpp —— 进程间通信机制延迟/吞吐/CPU 成本对比
// 实测结果与完整分析见同目录 README.md,原始数据 results_20261029.txt
//
// 负载口径刻意对齐 9000 网关的真实帧:
//   248 B    = DetectionHeader(8B) + 10 × BBox(24B)
//   4096 B   = 一页
//   921600 B = 1280×720 灰度整帧
//
// 设计要点(这是本实验最该看懂的地方):
//   所有共享内存机制共用**同一个 SPSC 环形缓冲区数据结构**,只有「等待可读/可写」的方式不同:
//     - shm_sem : 命名信号量(sem_wait/sem_post)  → 每条消息 2 次系统调用
//     - shm_spin: 用户态原子自旋(head/tail)       → 0 次系统调用
//   两者结构完全相同 → 测出的差值就是「系统调用 + 调度」的纯代价。
//
// 编译: g++ -O3 -std=c++17 -pthread ipc_bench.cpp -o ipc_bench -lrt
// 用法: ./ipc_bench
//       ./ipc_bench --quick
//       ./ipc_bench --mech pipe,shm_sem --mode lat --bytes 248
//
// 输出: 制表符分隔,首行为表头。
//
// 安全:子进程设 PR_SET_PDEATHSIG,父死子必死;自旋带 5s 超时,不会挂住板子。

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

// ---------------------------------------------------------------- 基础工具

static double now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static double cpu_ms(const struct rusage& ru) {
    return (double)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 +
           (double)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
}

static size_t pct_idx(size_t n, double p) {
    if (n == 0) return 0;
    size_t i = (size_t)(p * (double)(n - 1) + 0.5);
    return i >= n ? n - 1 : i;
}

struct Stats {
    double mn = 0, p50 = 0, p95 = 0, p99 = 0, mean = 0;
};

static Stats summarize(std::vector<double> v) {
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.mn = v.front();
    s.p50 = v[pct_idx(v.size(), 0.50)];
    s.p95 = v[pct_idx(v.size(), 0.95)];
    s.p99 = v[pct_idx(v.size(), 0.99)];
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / (double)v.size();
    return s;
}

static inline void cpu_relax() {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
    asm volatile("pause" ::: "memory");
#endif
}

static bool wr_full(int fd, const void* buf, size_t n) {
    const char* p = (const char*)buf;
    while (n > 0) {
        ssize_t k = write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (k == 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static bool rd_full(int fd, void* buf, size_t n) {
    char* p = (char*)buf;
    while (n > 0) {
        ssize_t k = read(fd, p, n);
        if (k < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (k == 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static bool snd_full(int fd, const void* buf, size_t n) {
    const char* p = (const char*)buf;
    while (n > 0) {
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (k == 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static bool rcv_full(int fd, void* buf, size_t n) {
    char* p = (char*)buf;
    while (n > 0) {
        ssize_t k = recv(fd, p, n, 0);
        if (k < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (k == 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

// ---------------------------------------------------------------- 通道抽象

struct Chan {
    std::function<bool(const void*, size_t)> send;
    std::function<bool(void*, size_t)> recv;
    bool ok = false;
};

constexpr int WARMUP = 50;

struct Cfg {
    size_t bytes = 248;
    int iters = 2000;
    bool thr = false;
};

static int total_msgs(const Cfg& c) { return c.thr ? c.iters : c.iters + WARMUP; }

struct Result {
    std::string mech, mode;
    size_t bytes = 0;
    int iters = 0;
    bool ok = false;
    const char* note = "";
    Stats rtt;                       // 延迟模式:一次往返 us
    double ns_per_msg = 0;
    double msgs_per_s = 0;
    double cpu_ms_total = 0, wall_ms_total = 0;
};

static Result bad_result(const char* mech, const Cfg& c, const char* note = "") {
    Result r;
    r.mech = mech;
    r.mode = c.thr ? "thr" : "lat";
    r.bytes = c.bytes;
    r.iters = c.iters;
    r.note = note;
    return r;
}

// 子进程统一执行
static void child_loop(Chan& a, Chan& b, const Cfg& c) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);  // 父死则子必死,避免孤儿空转
    if (getppid() == 1) _exit(9);

    std::vector<char> buf(c.bytes ? c.bytes : 1, 0);
    int total = total_msgs(c);
    if (!c.thr) {
        for (int i = 0; i < total; i++) {
            if (!a.recv(buf.data(), c.bytes)) _exit(2);
            if (!b.send(buf.data(), c.bytes)) _exit(3);
        }
    } else {
        for (int i = 0; i < total; i++) {
            if (!a.recv(buf.data(), c.bytes)) _exit(2);
        }
        if (!b.send(buf.data(), 1)) _exit(3);
    }
    _exit(0);
}

// 父进程统一驱动 + 计时
static Result parent_drive(const char* mech, Chan& a, Chan& b, pid_t pid, const Cfg& c) {
    Result r = bad_result(mech, c);

    struct rusage ru0;
    getrusage(RUSAGE_SELF, &ru0);

    std::vector<char> buf(c.bytes ? c.bytes : 1, 0xAB);
    int total = total_msgs(c);
    std::vector<double> samples;
    if (!c.thr) samples.reserve((size_t)c.iters);
    bool good = true;

    double wall0 = now_us();
    if (!c.thr) {
        for (int i = 0; i < total; i++) {
            double t0 = now_us();
            if (!a.send(buf.data(), c.bytes)) { good = false; break; }
            if (!b.recv(buf.data(), c.bytes)) { good = false; break; }
            double t1 = now_us();
            if (i >= WARMUP) samples.push_back(t1 - t0);
        }
    } else {
        for (int i = 0; i < total; i++) {
            if (!a.send(buf.data(), c.bytes)) { good = false; break; }
        }
        if (good && !b.recv(buf.data(), 1)) good = false;
    }
    double wall1 = now_us();

    if (!good) kill(pid, SIGKILL);

    int status = 0;
    struct rusage ru1, ruc;
    getrusage(RUSAGE_SELF, &ru1);
    pid_t w = wait4(pid, &status, 0, &ruc);
    if (w < 0) good = false;
    if (good && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) good = false;

    r.ok = good;
    r.wall_ms_total = (wall1 - wall0) / 1000.0;
    r.cpu_ms_total = cpu_ms(ru1) - cpu_ms(ru0) + cpu_ms(ruc);

    if (good) {
        if (!c.thr) {
            r.rtt = summarize(samples);
            r.msgs_per_s = r.rtt.mean > 0 ? 1e6 / r.rtt.mean : 0;
        } else {
            double dt_us = wall1 - wall0;
            if (dt_us > 0) {
                r.ns_per_msg = dt_us * 1000.0 / (double)c.iters;
                r.msgs_per_s = (double)c.iters * 1e6 / dt_us;
            }
        }
    }
    return r;
}

// ---------------------------------------------------------------- 字节流类

static Result bench_pipe(const Cfg& c) {
    int p2c[2], c2p[2];
    if (pipe(p2c) != 0) return bad_result("pipe", c);
    if (pipe(c2p) != 0) return bad_result("pipe", c);

    Chan a, b;
    a.send = [f = p2c[1]](const void* p, size_t n) { return wr_full(f, p, n); };
    a.recv = [f = p2c[0]](void* p, size_t n) { return rd_full(f, p, n); };
    b.send = [f = c2p[1]](const void* p, size_t n) { return wr_full(f, p, n); };
    b.recv = [f = c2p[0]](void* p, size_t n) { return rd_full(f, p, n); };

    pid_t pid = fork();
    if (pid == 0) child_loop(a, b, c);
    if (pid < 0) return bad_result("pipe", c);
    return parent_drive("pipe", a, b, pid, c);
}

static Result bench_fifo(const Cfg& c) {
    char pa[64], pb[64];
    snprintf(pa, sizeof pa, "/tmp/ipcbench_fifo_a_%d", (int)getpid());
    snprintf(pb, sizeof pb, "/tmp/ipcbench_fifo_b_%d", (int)getpid());
    unlink(pa);
    unlink(pb);
    if (mkfifo(pa, 0600) != 0) return bad_result("fifo", c);
    if (mkfifo(pb, 0600) != 0) { unlink(pa); return bad_result("fifo", c); }

    // 先 O_RDWR 建立两端,后续 O_RDONLY/O_WRONLY 才不会互相阻塞
    int k1 = open(pa, O_RDWR);
    int k2 = open(pb, O_RDWR);
    int ar = k1 >= 0 ? open(pa, O_RDONLY) : -1;
    int aw = k1 >= 0 ? open(pa, O_WRONLY) : -1;
    int br = k2 >= 0 ? open(pb, O_RDONLY) : -1;
    int bw = k2 >= 0 ? open(pb, O_WRONLY) : -1;
    if (k1 >= 0) close(k1);
    if (k2 >= 0) close(k2);
    if (ar < 0 || aw < 0 || br < 0 || bw < 0) {
        unlink(pa); unlink(pb);
        return bad_result("fifo", c);
    }

    Chan a, b;
    a.send = [f = aw](const void* p, size_t n) { return wr_full(f, p, n); };
    a.recv = [f = ar](void* p, size_t n) { return rd_full(f, p, n); };
    b.send = [f = bw](const void* p, size_t n) { return wr_full(f, p, n); };
    b.recv = [f = br](void* p, size_t n) { return rd_full(f, p, n); };

    pid_t pid = fork();
    if (pid == 0) child_loop(a, b, c);
    if (pid < 0) { unlink(pa); unlink(pb); return bad_result("fifo", c); }
    Result r = parent_drive("fifo", a, b, pid, c);
    unlink(pa);
    unlink(pb);
    return r;
}

static Result bench_unix_stream(const Cfg& c) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return bad_result("unix_stream", c);

    Chan a, b;
    a.send = [f = sv[0]](const void* p, size_t n) { return snd_full(f, p, n); };
    a.recv = [f = sv[1]](void* p, size_t n) { return rcv_full(f, p, n); };
    b.send = [f = sv[1]](const void* p, size_t n) { return snd_full(f, p, n); };
    b.recv = [f = sv[0]](void* p, size_t n) { return rcv_full(f, p, n); };

    pid_t pid = fork();
    if (pid == 0) child_loop(a, b, c);
    if (pid < 0) return bad_result("unix_stream", c);
    return parent_drive("unix_stream", a, b, pid, c);
}

static Result bench_unix_dgram(const Cfg& c) {
    if (c.bytes > 60000) return bad_result("unix_dgram", c, "payload_too_large");
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) return bad_result("unix_dgram", c);

    Chan a, b;
    a.send = [f = sv[0]](const void* p, size_t n) { return send(f, p, n, MSG_NOSIGNAL) == (ssize_t)n; };
    a.recv = [f = sv[1]](void* p, size_t n) { return recv(f, p, n, 0) == (ssize_t)n; };
    b.send = [f = sv[1]](const void* p, size_t n) { return send(f, p, n, MSG_NOSIGNAL) == (ssize_t)n; };
    b.recv = [f = sv[0]](void* p, size_t n) { return recv(f, p, n, 0) == (ssize_t)n; };

    pid_t pid = fork();
    if (pid == 0) child_loop(a, b, c);
    if (pid < 0) return bad_result("unix_dgram", c);
    return parent_drive("unix_dgram", a, b, pid, c);
}

static Result bench_tcp(const Cfg& c) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return bad_result("tcp", c);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(lfd, (struct sockaddr*)&sa, sizeof sa) != 0) { close(lfd); return bad_result("tcp", c); }
    if (listen(lfd, 1) != 0) { close(lfd); return bad_result("tcp", c); }
    socklen_t sl = sizeof sa;
    if (getsockname(lfd, (struct sockaddr*)&sa, &sl) != 0) { close(lfd); return bad_result("tcp", c); }

    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) _exit(4);
        if (connect(fd, (struct sockaddr*)&sa, sizeof sa) != 0) _exit(4);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        Chan a2, b2;
        a2.send = [fd](const void* p, size_t n) { return snd_full(fd, p, n); };
        a2.recv = [fd](void* p, size_t n) { return rcv_full(fd, p, n); };
        b2.send = [fd](const void* p, size_t n) { return snd_full(fd, p, n); };
        b2.recv = [fd](void* p, size_t n) { return rcv_full(fd, p, n); };
        child_loop(a2, b2, c);
    }
    if (pid < 0) { close(lfd); return bad_result("tcp", c); }

    int fd = accept(lfd, nullptr, nullptr);
    close(lfd);
    if (fd < 0) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        return bad_result("tcp", c);
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    Chan a, b;
    a.send = [fd](const void* p, size_t n) { return snd_full(fd, p, n); };
    a.recv = [fd](void* p, size_t n) { return rcv_full(fd, p, n); };
    b.send = [fd](const void* p, size_t n) { return snd_full(fd, p, n); };
    b.recv = [fd](void* p, size_t n) { return rcv_full(fd, p, n); };
    return parent_drive("tcp", a, b, pid, c);
}

// ---------------------------------------------------------------- 共享内存环形缓冲区
//
// 内存布局(每块共享内存一个方向):
//   [0    ) uint64 slot_bytes
//   [8    ) uint64 cap
//   [64   ) uint64 head   —— 只由生产者写
//   [128  ) uint64 tail   —— 只由消费者写
//   [192  ) 数据区 cap × slot_bytes
//
// SPSC 环形缓冲区:生产者写 head 槽后 head++,消费者读 tail 槽后 tail++。
// head==tail 空;head-tail==cap 满。计数器单调递增,不取模,取模只用于寻址
// —— 这样避免了 ABA/回绕问题,也让「满」与「空」不再歧义。

constexpr size_t RING_HDR = 192;

// 记录本进程创建过的命名对象,退出时统一 unlink(否则 shm/sem 会一直留在系统里)
static std::vector<std::string>& cleanup_names() {
    static std::vector<std::string> v;
    return v;
}

static void register_cleanup(const char* name) {
    cleanup_names().push_back(name);
    if (cleanup_names().size() == 1) {
        atexit([] {
            for (const auto& n : cleanup_names()) {
                sem_unlink(n.c_str());
                shm_unlink(n.c_str());
            }
        });
    }
}

struct Ring {
    char* base = nullptr;
    size_t slot = 0;
    size_t cap = 0;
    uint64_t* head = nullptr;
    uint64_t* tail = nullptr;
    char* data = nullptr;
    size_t map_sz = 0;

    bool init(char* p, size_t total) {
        base = p;
        slot = *(uint64_t*)(p + 0);
        cap = *(uint64_t*)(p + 8);
        map_sz = total;
        if (cap == 0) return false;
        head = (uint64_t*)(p + 64);
        tail = (uint64_t*)(p + 128);
        __atomic_store_n(head, 0ull, __ATOMIC_RELAXED);
        __atomic_store_n(tail, 0ull, __ATOMIC_RELAXED);
        data = p + RING_HDR;
        return true;
    }

    char* slot_ptr(uint64_t idx) { return data + (size_t)(idx % cap) * slot; }
};

// 计算环形缓冲区该给多大:容量自适应,限制总内存 <= 8MB
static size_t ring_cap_for(size_t slot) {
    if (slot == 0) slot = 1;
    size_t cap = 8ull * 1024 * 1024 / slot;
    if (cap < 2) cap = 2;
    if (cap > 256) cap = 256;
    return cap;
}

static size_t ring_total_for(size_t slot) {
    if (slot == 0) slot = 1;
    return RING_HDR + ring_cap_for(slot) * slot;
}

// 建立一块命名共享内存并写好头部,返回映射指针
static char* shm_ring_create(const char* name, size_t slot, size_t* out_total) {
    size_t total = ring_total_for(slot);
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return nullptr;
    register_cleanup(name);
    if (ftruncate(fd, (off_t)total) != 0) { close(fd); shm_unlink(name); return nullptr; }
    void* p = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { shm_unlink(name); return nullptr; }
    *(uint64_t*)((char*)p + 0) = (uint64_t)slot;
    *(uint64_t*)((char*)p + 8) = (uint64_t)ring_cap_for(slot);
    *out_total = total;
    return (char*)p;
}

// ---- 变体 A:信号量阻塞(2 次系统调用/消息)----

static Chan make_shm_sem_chan(const char* tag, int idx, size_t slot) {
    Chan ch;
    char nm[96], sn1[112], sn2[112];
    snprintf(nm, sizeof nm, "/ipcbench_sem_%s_%d_%d", tag, (int)getpid(), idx);
    snprintf(sn1, sizeof sn1, "/ipcbench_semdata_%s_%d_%d", tag, (int)getpid(), idx);
    snprintf(sn2, sizeof sn2, "/ipcbench_semspc_%s_%d_%d", tag, (int)getpid(), idx);

    size_t total = 0;
    char* p = shm_ring_create(nm, slot, &total);
    if (!p) return ch;
    Ring ring;
    if (!ring.init(p, total)) { munmap(p, total); return ch; }

    sem_unlink(sn1);
    sem_unlink(sn2);
    sem_t* sem_data = sem_open(sn1, O_CREAT | O_EXCL, 0600, 0);
    sem_t* sem_space = sem_open(sn2, O_CREAT | O_EXCL, 0600, (unsigned)ring.cap);
    register_cleanup(sn1);
    register_cleanup(sn2);
    if (sem_data == SEM_FAILED || sem_space == SEM_FAILED) {
        munmap(p, total);
        return ch;
    }

    ch.send = [ring, sem_space, sem_data](const void* src, size_t n) mutable {
        while (sem_wait(sem_space) != 0) {
            if (errno != EINTR) return false;
        }
        uint64_t h = __atomic_load_n(ring.head, __ATOMIC_RELAXED);
        memcpy(ring.slot_ptr(h), src, n);
        __atomic_store_n(ring.head, h + 1, __ATOMIC_RELEASE);
        if (sem_post(sem_data) != 0) return false;
        return true;
    };
    ch.recv = [ring, sem_space, sem_data](void* dst, size_t n) mutable {
        while (sem_wait(sem_data) != 0) {
            if (errno != EINTR) return false;
        }
        uint64_t t = __atomic_load_n(ring.tail, __ATOMIC_RELAXED);
        memcpy(dst, ring.slot_ptr(t), n);
        __atomic_store_n(ring.tail, t + 1, __ATOMIC_RELEASE);
        if (sem_post(sem_space) != 0) return false;
        return true;
    };
    ch.ok = true;
    return ch;
}

// ---- 变体 B:用户态自旋(0 次系统调用)----

static Chan make_shm_spin_chan(const char* tag, int idx, size_t slot) {
    Chan ch;
    char nm[96];
    snprintf(nm, sizeof nm, "/ipcbench_spin_%s_%d_%d", tag, (int)getpid(), idx);

    size_t total = 0;
    char* p = shm_ring_create(nm, slot, &total);
    if (!p) return ch;
    Ring ring;
    if (!ring.init(p, total)) { munmap(p, total); return ch; }

    ch.send = [ring](const void* src, size_t n) mutable {
        uint64_t h = __atomic_load_n(ring.head, __ATOMIC_RELAXED);
        double t0 = now_us();
        // 等待有空位;每 4096 次检查一次超时,防止死等挂住板子
        for (int spin = 0;; spin++) {
            uint64_t t = __atomic_load_n(ring.tail, __ATOMIC_ACQUIRE);
            if (h - t < ring.cap) break;
            cpu_relax();
            if ((spin & 0xFFF) == 0xFFF && now_us() - t0 > 5e6) return false;
        }
        memcpy(ring.slot_ptr(h), src, n);
        __atomic_store_n(ring.head, h + 1, __ATOMIC_RELEASE);
        return true;
    };
    ch.recv = [ring](void* dst, size_t n) mutable {
        uint64_t t = __atomic_load_n(ring.tail, __ATOMIC_RELAXED);
        double t0 = now_us();
        for (int spin = 0;; spin++) {
            uint64_t h = __atomic_load_n(ring.head, __ATOMIC_ACQUIRE);
            if (h != t) break;
            cpu_relax();
            if ((spin & 0xFFF) == 0xFFF && now_us() - t0 > 5e6) return false;
        }
        memcpy(dst, ring.slot_ptr(t), n);
        __atomic_store_n(ring.tail, t + 1, __ATOMIC_RELEASE);
        return true;
    };
    ch.ok = true;
    return ch;
}

static Result bench_shm_sem(const Cfg& c) {
    size_t slot = c.bytes ? c.bytes : 1;
    char tag[16];
    snprintf(tag, sizeof tag, "s%d", (int)(getpid() & 0xFFFFFF));
    Chan a = make_shm_sem_chan(tag, 0, slot);
    Chan b = make_shm_sem_chan(tag, 1, slot);
    if (!a.ok || !b.ok) return bad_result("shm_sem", c, "setup_failed");

    pid_t pid = fork();
    if (pid == 0) child_loop(a, b, c);
    if (pid < 0) return bad_result("shm_sem", c);
    return parent_drive("shm_sem", a, b, pid, c);
}

static Result bench_shm_spin(const Cfg& c) {
    size_t slot = c.bytes ? c.bytes : 1;
    char tag[16];
    snprintf(tag, sizeof tag, "p%d", (int)(getpid() & 0xFFFFFF));
    Chan a = make_shm_spin_chan(tag, 0, slot);
    Chan b = make_shm_spin_chan(tag, 1, slot);
    if (!a.ok || !b.ok) return bad_result("shm_spin", c, "setup_failed");

    pid_t pid = fork();
    if (pid == 0) child_loop(a, b, c);
    if (pid < 0) return bad_result("shm_spin", c);
    return parent_drive("shm_spin", a, b, pid, c);
}

// ---- 变体 C:SysV shm + SysV 信号量(老接口,对照 POSIX)----

// 每个方向一个 shmid + 一个含 2 个信号量的 semid:
//   索引 0 = 数据可用(初始 0),索引 1 = 空位(初始 cap)
static Chan make_sysv_chan(int shmid, int semid, size_t slot, size_t total) {
    Chan ch;
    char* p = (char*)shmat(shmid, nullptr, 0);
    if (p == (void*)-1) return ch;

    // SysV 共享内存是清零的,头部必须由我们写进去
    *(uint64_t*)(p + 0) = (uint64_t)slot;
    *(uint64_t*)(p + 8) = (uint64_t)ring_cap_for(slot);

    Ring ring;
    if (!ring.init(p, total)) { shmdt(p); return ch; }

    const unsigned short IDX_DATA = 0;
    const unsigned short IDX_SPACE = 1;

    auto sem_op = [semid](unsigned short idx, int delta) -> bool {
        struct sembuf op;
        op.sem_num = idx;
        op.sem_op = (short)delta;
        // 刻意不用 SEM_UNDO:它会给每个进程记一份「崩溃补偿表」,
        // 每做一次 semop 就多一条,内核上限 32767 条(SEMVMX),
        // 单向 20 万条消息必然溢出 → semop 返回 ERANGE,整个通道静默失败。
        // 这是 SysV 信号量相对 POSIX 命名信号量特有的坑。
        op.sem_flg = 0;
        while (semop(semid, &op, 1) != 0) {
            if (errno != EINTR) return false;
        }
        return true;
    };

    ch.send = [ring, sem_op](const void* src, size_t n) mutable {
        if (!sem_op(IDX_SPACE, -1)) return false;
        uint64_t h = __atomic_load_n(ring.head, __ATOMIC_RELAXED);
        memcpy(ring.slot_ptr(h), src, n);
        __atomic_store_n(ring.head, h + 1, __ATOMIC_RELEASE);
        return sem_op(IDX_DATA, 1);
    };
    ch.recv = [ring, sem_op](void* dst, size_t n) mutable {
        if (!sem_op(IDX_DATA, -1)) return false;
        uint64_t t = __atomic_load_n(ring.tail, __ATOMIC_RELAXED);
        memcpy(dst, ring.slot_ptr(t), n);
        __atomic_store_n(ring.tail, t + 1, __ATOMIC_RELEASE);
        return sem_op(IDX_SPACE, 1);
    };
    ch.ok = true;
    return ch;
}

static Result bench_sysv_shm(const Cfg& c) {
    size_t slot = c.bytes ? c.bytes : 1;
    size_t cap = ring_cap_for(slot);
    size_t total = RING_HDR + cap * slot;

    int sh0 = shmget(IPC_PRIVATE, total, 0600 | IPC_CREAT);
    int sh1 = shmget(IPC_PRIVATE, total, 0600 | IPC_CREAT);
    if (sh0 < 0 || sh1 < 0) {
        if (sh0 >= 0) shmctl(sh0, IPC_RMID, nullptr);
        if (sh1 >= 0) shmctl(sh1, IPC_RMID, nullptr);
        return bad_result("sysv_shm", c);
    }
    // 每个方向两个信号量:数据可用(0)、空位(cap)
    int semid0 = semget(IPC_PRIVATE, 2, 0600 | IPC_CREAT);
    int semid1 = semget(IPC_PRIVATE, 2, 0600 | IPC_CREAT);
    if (semid0 < 0 || semid1 < 0) {
        shmctl(sh0, IPC_RMID, nullptr);
        shmctl(sh1, IPC_RMID, nullptr);
        return bad_result("sysv_shm", c);
    }
    unsigned short vals0[2] = {0, (unsigned short)cap};
    unsigned short vals1[2] = {0, (unsigned short)cap};
    semctl(semid0, 0, SETALL, vals0);
    semctl(semid1, 0, SETALL, vals1);

    // 方向 a 走 sh0+semid0,方向 b 走 sh1+semid1
    Chan a = make_sysv_chan(sh0, semid0, slot, total);
    Chan b = make_sysv_chan(sh1, semid1, slot, total);
    if (!a.ok || !b.ok) {
        shmctl(sh0, IPC_RMID, nullptr);
        shmctl(sh1, IPC_RMID, nullptr);
        semctl(semid0, 0, IPC_RMID);
        semctl(semid1, 0, IPC_RMID);
        return bad_result("sysv_shm", c);
    }

    pid_t pid = fork();
    if (pid == 0) child_loop(a, b, c);
    if (pid < 0) {
        shmctl(sh0, IPC_RMID, nullptr);
        shmctl(sh1, IPC_RMID, nullptr);
        semctl(semid0, 0, IPC_RMID);
        semctl(semid1, 0, IPC_RMID);
        return bad_result("sysv_shm", c);
    }
    Result r = parent_drive("sysv_shm", a, b, pid, c);
    shmctl(sh0, IPC_RMID, nullptr);
    shmctl(sh1, IPC_RMID, nullptr);
    semctl(semid0, 0, IPC_RMID);
    semctl(semid1, 0, IPC_RMID);
    return r;
}

// ---------------------------------------------------------------- 主程序

struct MechEntry {
    const char* name;
    Result (*fn)(const Cfg&);
};

static const MechEntry kMechs[] = {
    {"pipe", bench_pipe},
    {"fifo", bench_fifo},
    {"unix_stream", bench_unix_stream},
    {"unix_dgram", bench_unix_dgram},
    {"tcp", bench_tcp},
    {"shm_sem", bench_shm_sem},
    {"shm_spin", bench_shm_spin},
    {"sysv_shm", bench_sysv_shm},
};

static void print_row(const Result& r) {
    printf("%s\t%s\t%zu\t%d\t%s\t%.3f\t%.3f\t%.3f\t%.3f\t%.1f\t%.3f\t%.3f\t%s\n",
           r.mech.c_str(), r.mode.c_str(), r.bytes, r.iters, r.ok ? "1" : "0",
           r.rtt.p50, r.rtt.p95, r.rtt.p99, r.rtt.mean, r.msgs_per_s,
           r.cpu_ms_total, r.wall_ms_total, r.note);
    fflush(stdout);
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    bool quick = false;
    std::string only_mech;
    std::string only_mode;
    std::vector<size_t> sizes;
    int lat_iters = 2000;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--quick") {
            quick = true;
        } else if (a == "--mech" && i + 1 < argc) {
            only_mech = argv[++i];
        } else if (a == "--mode" && i + 1 < argc) {
            only_mode = argv[++i];
        } else if (a == "--iters" && i + 1 < argc) {
            lat_iters = atoi(argv[++i]);
        } else if (a == "--bytes" && i + 1 < argc) {
            sizes.push_back((size_t)atol(argv[++i]));
        } else if (a == "--help" || a == "-h") {
            printf("用法: %s [--quick] [--mech a,b] [--mode lat|thr] [--bytes N] [--iters N]\n", argv[0]);
            return 0;
        }
    }

    std::vector<size_t> lat_sizes = sizes.empty() ? std::vector<size_t>{248, 4096} : sizes;
    std::vector<size_t> thr_sizes = sizes.empty() ? std::vector<size_t>{248, 4096, 921600} : sizes;
    if (quick) {
        lat_sizes = sizes.empty() ? std::vector<size_t>{248} : sizes;
        thr_sizes = sizes.empty() ? std::vector<size_t>{248} : sizes;
        lat_iters = 1000;
    }
    bool do_lat = only_mode.empty() || only_mode == "lat" || only_mode == "both";
    bool do_thr = only_mode.empty() || only_mode == "thr" || only_mode == "both";

    printf("mech\tmode\tbytes\titers\tok\tp50_us\tp95_us\tp99_us\tmean_us\tmsgs_per_s\tcpu_ms\twall_ms\tnote\n");
    fflush(stdout);

    for (const auto& m : kMechs) {
        if (!only_mech.empty() && only_mech.find(m.name) == std::string::npos) continue;

        if (do_lat) {
            for (size_t b : lat_sizes) {
                Cfg c;
                c.bytes = b;
                c.iters = lat_iters;
                c.thr = false;
                print_row(m.fn(c));
            }
        }
        if (do_thr) {
            for (size_t b : thr_sizes) {
                Cfg c;
                c.bytes = b;
                size_t budget = 256ull * 1024 * 1024;
                int it = (int)(budget / (b ? b : 1));
                if (it < 200) it = 200;
                if (it > 200000) it = 200000;
                c.iters = it;
                c.thr = true;
                print_row(m.fn(c));
            }
        }
    }
    return 0;
}
