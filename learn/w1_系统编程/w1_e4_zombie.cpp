#include <iostream>
#include <unistd.h>     // fork, sleep, getpid
#include <sys/wait.h>   // waitpid
#include <cstdlib>      // exit

int main() {
    pid_t pid = fork();

    if (pid == 0) {
        // 【子进程】刚出生 1 秒钟就光速自杀，并留下遗言退出码 88
        std::cout << "[子进程] 我出生了，但我马上自杀！PID = " << getpid() << std::endl;
        exit(88); 
    } else {
        // 【父进程】故意不管孩子，呼呼大睡 10 秒钟，不收尸！
        std::cout << "[父进程] 子进程已死，但我故意不 wait 它！我睡 10 秒，现在是僵尸时间..." << std::endl;
        sleep(10);

        // 10 秒后，父进程醒来，终于肯调用 waitpid 收尸了
        int status;
        waitpid(pid, &status, 0);
        std::cout << "[父进程] 我醒了，收尸完毕！子进程退出码 = " << WEXITSTATUS(status) << std::endl;
        std::cout << "[父进程] 僵尸已被超度消灭！" << std::endl;
    }

    return 0;
}
