#include <iostream>
#include <unistd.h>     // fork, execvp, getpid, getppid
#include <sys/wait.h>   // waitpid, WIFEXITED, WEXITSTATUS
#include <cstdio>       // perror

int main() {
    std::cout << "[父进程] 我是主进程，PID = " << getpid() << "，准备生一个子进程..." << std::endl;

    pid_t pid = fork();

    if (pid < 0) {
        // 分身失败
        perror("fork 失败");
        return -1;
    } else if (pid == 0) {
        // ---------------- 这里是子进程的代码 ----------------
        std::cout << "  [子进程] 我出生了！我的 PID = " << getpid() 
                  << "，我的父进程 PID = " << getppid() << std::endl;
        std::cout << "  [子进程] 我准备变身为 'uname -m' 命令..." << std::endl;

        // 准备命令行参数：uname -m
        char* args[] = {(char*)"uname", (char*)"-m", NULL};
        
        // execvp 会直接替换当前进程的代码！
        execvp("uname", args);

        // 注意：如果 execvp 执行成功，下面的代码绝对不会被执行！
        // 只有失败了才会走到这里
        perror("execvp 失败");
        return -1;
    } else {
        // ---------------- 这里是父进程的代码 ----------------
        std::cout << "[父进程] 我成功生了子进程，它的 PID = " << pid << "，我正在等待它完工..." << std::endl;

        int status;
        // 等待子进程退出
        waitpid(pid, &status, 0);

        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            std::cout << "[父进程] 子进程已正常退出，收到它的退出码 = " << exit_code << std::endl;
        } else {
            std::cout << "[父进程] 子进程异常退出！" << std::endl;
        }
    }

    return 0;
}
