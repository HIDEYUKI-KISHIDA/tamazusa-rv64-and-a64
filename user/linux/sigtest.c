/* sigtest.c — M23: シグナルの確認（musl で静的リンク、Linux 互換層で動かす） */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>

static int ok_count, ng_count;
static void check(int cond, const char *what)
{
    printf("sigtest %2d: %s %s\n", ok_count + ng_count + 1, cond ? "OK" : "NG", what);
    if (cond) ok_count++; else ng_count++;
}

static volatile sig_atomic_t got_usr1, got_info_signo, got_chld, got_alrm_like;
static volatile double fp_in_handler;

static void on_usr1(int s) { got_usr1 = s; fp_in_handler = 1.25 * 4.0; }
static void on_info(int s, siginfo_t *si, void *uc) { (void)uc; got_info_signo = si->si_signo == s ? s : -1; }
static void on_chld(int s) { (void)s; got_chld++; }
static void on_usr2(int s) { (void)s; got_alrm_like++; }

int main(void)
{
    struct sigaction sa;
    sigset_t set, old, pend;
    int st, p[2], fd;
    pid_t pid;
    volatile double x = 3.5;
    char c;

    /* 1. 処理関数が呼ばれ、戻ってくる。浮動小数点のレジスタも元のまま */
    signal(SIGUSR1, on_usr1);
    x = x * 2.0;
    raise(SIGUSR1);
    check(got_usr1 == SIGUSR1 && fp_in_handler == 5.0 && x == 7.0, "handler runs and returns (registers kept)");

    /* 2. SA_SIGINFO */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_info;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &sa, NULL);
    kill(getpid(), SIGUSR2);
    check(got_info_signo == SIGUSR2, "SA_SIGINFO handler gets siginfo");

    /* 3. マスクで止めると保留になり、外すと届く */
    signal(SIGUSR1, on_usr1);
    got_usr1 = 0;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &old);
    raise(SIGUSR1);
    sigpending(&pend);
    check(got_usr1 == 0 && sigismember(&pend, SIGUSR1), "blocked signal stays pending");
    sigprocmask(SIG_SETMASK, &old, NULL);
    check(got_usr1 == SIGUSR1, "delivered when unblocked");

    /* 4. 無視 */
    signal(SIGUSR1, SIG_IGN);
    raise(SIGUSR1);
    check(1, "ignored signal does nothing");

    /* 5. 子を kill すると、wait の状態がシグナルになる */
    pid = fork();
    if (pid == 0) {
        for (;;) { }
    }
    usleep(30000);          /* 子が計算し続けている間に、眠った親が起きて動ける（時分割） */
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "child killed by SIGTERM (busy loop)");

    /* 6. 眠っている子も起きて終わる */
    pid = fork();
    if (pid == 0) {
        sleep(30);
        _exit(0);
    }
    usleep(50000);
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "sleeping child killed by SIGKILL");

    /* 7. SIGCHLD の処理関数 */
    signal(SIGCHLD, on_chld);
    pid = fork();
    if (pid == 0) {
        _exit(3);
    }
    waitpid(pid, &st, 0);
    check(got_chld >= 1 && WIFEXITED(st) && WEXITSTATUS(st) == 3, "SIGCHLD handler on child exit");
    signal(SIGCHLD, SIG_DFL);

    /* 8. パイプを読んで眠っているとき、SA_RESTART なしなら EINTR */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr2;
    sigaction(SIGUSR2, &sa, NULL);
    pipe(p);
    pid = fork();
    if (pid == 0) {
        usleep(50000);
        kill(getppid(), SIGUSR2);
        _exit(0);
    }
    errno = 0;
    st = (int)read(p[0], &c, 1);
    check(st == -1 && errno == EINTR && got_alrm_like == 1, "read interrupted -> EINTR");
    waitpid(pid, NULL, 0);

    /* 9. SA_RESTART なら読み直す */
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, NULL);
    pid = fork();
    if (pid == 0) {
        usleep(50000);
        kill(getppid(), SIGUSR2);
        usleep(50000);
        write(p[1], "z", 1);
        _exit(0);
    }
    st = (int)read(p[0], &c, 1);
    check(st == 1 && c == 'z' && got_alrm_like == 2, "SA_RESTART restarts read");
    waitpid(pid, NULL, 0);
    close(p[0]);
    close(p[1]);

    /* 10. sigsuspend */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, &old);
    pid = fork();
    if (pid == 0) {
        usleep(30000);
        kill(getppid(), SIGUSR2);
        _exit(0);
    }
    sigemptyset(&set);
    st = sigsuspend(&set);
    sigprocmask(SIG_SETMASK, NULL, &pend);
    check(st == -1 && errno == EINTR && got_alrm_like == 3 && sigismember(&pend, SIGUSR2),
          "sigsuspend waits, then mask comes back");
    sigprocmask(SIG_SETMASK, &old, NULL);
    waitpid(pid, NULL, 0);

    /* 11. 読む側のないパイプへ書くと SIGPIPE で終わる */
    pipe(p);
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        write(p[1], "x", 1);
        _exit(9);
    }
    close(p[0]);
    close(p[1]);
    waitpid(pid, &st, 0);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE, "write to closed pipe -> SIGPIPE");

    /* 12. プロセスグループへ送る */
    pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        pause();
        _exit(0);
    }
    setpgid(pid, pid);
    usleep(30000);
    kill(-pid, SIGINT);
    waitpid(pid, &st, 0);
    check(getpgid(pid) == -1 && WIFSIGNALED(st) && WTERMSIG(st) == SIGINT, "kill(-pgid) reaches the group");

    /* 13. /dev/null と /dev/zero */
    fd = open("/dev/null", O_WRONLY);
    st = (int)write(fd, "abc", 3);
    close(fd);
    fd = open("/dev/zero", O_RDONLY);
    c = 1;
    read(fd, &c, 1);
    close(fd);
    check(st == 3 && c == 0, "/dev/null and /dev/zero");

    printf("sigtest: %s (%d OK, %d NG)\n", ng_count == 0 ? "all checks passed" : "FAILED", ok_count, ng_count);
    return ng_count == 0 ? 0 : 1;
}
