/* proctest.c — Linux 互換層のプロセスを確かめる（M21）
 *   fork・wait・メモリの複製・パイプ・execve・getppid・WNOHANG・繰り返しの fork
 *   すべて合えば「proctest: all N checks passed」と出して 0、だめなら失敗した番号で終わる
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>

static int n_ok;
static volatile int shared_value = 100;

static void check(int ok, int no, const char *what)
{
    if (!ok) {
        printf("proctest: check %d failed: %s (errno=%d)\n", no, what, errno);
        exit(no);
    }
    n_ok++;
}

int main(void)
{
    pid_t me = getpid(), c, w;
    int st, fds[2], fds2[2], i;
    char buf[64];
    static char big[10000];

    /* 1. fork / exit / wait */
    c = fork();
    if (c == 0) {
        _exit(5);
    }
    check(c > 0 && c != me, 1, "fork returns the child pid");
    w = waitpid(c, &st, 0);
    check(w == c && WIFEXITED(st) && WEXITSTATUS(st) == 5, 2, "wait gets exit code 5");

    /* 2. メモリは複製（親と子で別々） */
    c = fork();
    if (c == 0) {
        shared_value = 200;             /* 子だけが変える */
        _exit(getppid() == me ? (shared_value == 200 ? 0 : 3) : 4);
    }
    waitpid(c, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, 3, "child sees its own copy and getppid = parent");
    check(shared_value == 100, 4, "parent memory unchanged by the child");

    /* 3. パイプ（親 → 子） */
    check(pipe(fds) == 0, 5, "pipe");
    c = fork();
    if (c == 0) {
        int n;
        close(fds[1]);
        n = read(fds[0], buf, sizeof(buf) - 1);
        buf[n > 0 ? n : 0] = 0;
        printf("proctest child got: %s\n", buf);
        _exit(strcmp(buf, "hello pipe") == 0 ? 0 : 6);
    }
    close(fds[0]);
    check(write(fds[1], "hello pipe", 10) == 10, 6, "write to pipe");
    close(fds[1]);
    waitpid(c, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, 7, "child read 'hello pipe'");

    /* 4. 入れ物より大きいデータ（子 → 親、10000 バイト） */
    check(pipe(fds2) == 0, 8, "pipe 2");
    c = fork();
    if (c == 0) {
        close(fds2[0]);
        for (i = 0; i < (int)sizeof(big); i++) {
            big[i] = (char)('a' + i % 26);
        }
        _exit(write(fds2[1], big, sizeof(big)) == (ssize_t)sizeof(big) ? 0 : 9);
    }
    close(fds2[1]);
    {
        size_t got = 0;
        ssize_t n;
        int ok = 1;
        memset(big, 0, sizeof(big));
        while ((n = read(fds2[0], big + got, sizeof(big) - got)) > 0) {
            got += (size_t)n;
        }
        for (i = 0; i < (int)sizeof(big); i++) {
            if (big[i] != (char)('a' + i % 26)) {
                ok = 0;
            }
        }
        check(got == sizeof(big) && ok, 9, "10000 bytes through a 4 KiB pipe, then end of data");
    }
    close(fds2[0]);
    waitpid(c, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, 10, "writer child finished");

    /* 5. execve */
    c = fork();
    if (c == 0) {
        char *argv[] = { "hello_musl", "from-exec", 0 };
        char *envp[] = { "HOME=/exec", 0 };
        execve("/hello_musl.elf", argv, envp);
        _exit(99);                      /* exec できなかった */
    }
    waitpid(c, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 7, 11, "execve hello_musl -> exit 7");
    check(execve("/nothing.elf", (char *[]){ "x", 0 }, (char *[]){ 0 }) < 0 && errno == ENOENT,
          12, "execve of a missing program -> ENOENT");

    /* 6. WNOHANG */
    c = fork();
    if (c == 0) {
        struct timespec t = { 0, 100 * 1000 * 1000 };
        nanosleep(&t, 0);
        _exit(1);
    }
    check(waitpid(c, &st, WNOHANG) == 0, 13, "WNOHANG while the child sleeps -> 0");
    check(waitpid(c, &st, 0) == c && WEXITSTATUS(st) == 1, 14, "then wait gets it");
    check(waitpid(-1, &st, 0) < 0 && errno == ECHILD, 15, "no children -> ECHILD");

    /* 7. 繰り返しの fork（後始末の確認） */
    for (i = 0; i < 12; i++) {
        c = fork();
        if (c == 0) {
            _exit(i);
        }
        if (waitpid(c, &st, 0) != c || WEXITSTATUS(st) != i) {
            break;
        }
    }
    check(i == 12, 16, "12 fork/wait rounds");

    printf("proctest: all %d checks passed\n", n_ok);
    return 0;
}
