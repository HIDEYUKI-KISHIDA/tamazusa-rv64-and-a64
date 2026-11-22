/* ptytest.c — M26: 疑似端末（/dev/ptmx・/dev/pts/N）の確認 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int ok_count, ng_count;
static void check(int cond, const char *what)
{
    printf("ptytest %2d: %s %s\n", ok_count + ng_count + 1, cond ? "OK" : "NG", what);
    fflush(stdout);
    if (cond) ok_count++; else ng_count++;
}

/* マスターから、want を含むまで（最大 2 秒）読む */
static int read_until(int m, char *buf, int max, const char *want)
{
    int n = 0, tries;
    fcntl(m, F_SETFL, O_NONBLOCK);
    for (tries = 0; tries < 200; tries++) {
        int r = (int)read(m, buf + n, max - 1 - n);
        if (r > 0) {
            n += r;
            buf[n] = 0;
            if (strstr(buf, want)) {
                break;
            }
        } else {
            usleep(10000);
        }
    }
    buf[n] = 0;
    fcntl(m, F_SETFL, 0);
    return n;
}

int main(void)
{
    char name[64], buf[512];
    int m, s, st;
    pid_t pid;
    struct winsize ws = { 30, 100, 0, 0 }, ws2;
    struct termios t;

    /* 1. 開く */
    m = posix_openpt(O_RDWR | O_NOCTTY);
    {
        unsigned int num = 999;
        /* ioctl() は番号を int で受けるので、TIOCGPTN（0x80045430）は符号が広がって届く（openpty と同じ道） */
        check(m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0 && ptsname_r(m, name, sizeof(name)) == 0
              && strncmp(name, "/dev/pts/", 9) == 0 && ioctl(m, TIOCGPTN, &num) == 0 && num < 999,
              "posix_openpt, unlockpt, ptsname, ioctl(TIOCGPTN)");
    }

    /* 2. 端末の大きさ */
    s = open(name, O_RDWR | O_NOCTTY);
    ioctl(m, TIOCSWINSZ, &ws);
    check(s >= 0 && ioctl(s, TIOCGWINSZ, &ws2) == 0 && ws2.ws_row == 30 && ws2.ws_col == 100 && isatty(s),
          "TIOCSWINSZ on master, TIOCGWINSZ on slave, isatty");
    {
        char tn[64];
        check(ttyname_r(s, tn, sizeof(tn)) == 0 && strcmp(tn, name) == 0, "ttyname gives /dev/pts/N");
    }
    close(s);

    /* 4. 行ごとの入力: Backspace で消せる、エコー、改行は CR LF */
    pid = fork();
    if (pid == 0) {
        int k;
        setsid();
        k = open(name, O_RDWR);                 /* セッションの長が開く = 制御端末 */

        {
            char line[64];
            int n = (int)read(k, line, sizeof(line) - 1);
            line[n > 0 ? n : 0] = 0;
            dprintf(k, "got:%s", line);
        }
        pause();                                /* Ctrl-C を待つ */
        _exit(0);
    }
    usleep(100000);
    write(m, "abc\x7f" "d\r", 6);
    read_until(m, buf, sizeof(buf), "got:abd\r\n");
    check(strstr(buf, "ab\b \bd") != NULL || strstr(buf, "abc\b \bd") != NULL, "echo with backspace erase");
    check(strstr(buf, "got:abd\r\n") != NULL, "canonical read got 'abd\\n', output newline became CR LF");

    /* 6. Ctrl-C で前面のグループに SIGINT */
    write(m, "\x03", 1);
    waitpid(pid, &st, 0);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGINT, "Ctrl-C on the master kills the foreground job (SIGINT)");

    /* 7. 生の入力（ICANON・ECHO なし） */
    s = open(name, O_RDWR | O_NOCTTY);
    tcgetattr(s, &t);
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(s, TCSANOW, &t);
    write(m, "xy", 2);
    {
        int n = (int)read(s, buf, sizeof(buf));
        check(n == 2 && buf[0] == 'x' && buf[1] == 'y', "raw mode: read returns at once, no echo needed");
    }

    /* 8. スレーブがすべて閉じると、マスターの read は EIO */
    close(s);
    {
        int i, r = 0;
        for (i = 0; i < 10; i++) {          /* 残っている出力（^C のエコーなど）を読み切ってから */
            errno = 0;
            r = (int)read(m, buf, sizeof(buf));
            if (r <= 0) {
                break;
            }
        }
        check(r == -1 && errno == EIO, "master read -> EIO after the slave closed (after the remaining output)");
    }
    close(m);

    printf("ptytest: %s (%d OK, %d NG)\n", ng_count == 0 ? "all checks passed" : "FAILED", ok_count, ng_count);
    return ng_count == 0 ? 0 : 1;
}
