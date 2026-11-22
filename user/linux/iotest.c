/* iotest.c — 安全の決まり 6・7 の試験（M39）。Tamazusa の /dev/io・/dev/iolog を使う
 *
 *   6. 誰が動かせるか: root は動かせる。uid 1000 は読めるが動かせない（止めることはできる）。
 *      io のグループ（20）に入れば動かせる。ネットワークのソケットを開いたプロセスは動かせない。
 *      setuid したあと root には戻れない。
 *   7. 記録: /dev/iolog に、解除・出力（値・名前・プログラム・uid）・停止・断ったことが残っている。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <grp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>

struct io_req {                 /* kernel/io/iomgr.c と同じ形（56 バイト） */
    char name[32];
    int32_t id, kind, speed, flags;
    int64_t value;              /* ミリ単位 */
};
#define IOC_LOOKUP 0x4f01
#define IOC_GET    0x4f02
#define IOC_SET    0x4f03
#define IOC_SAFE   0x4f04
#define P_ESTOP    0x3f

static int fails;

static void check(int ok, const char *what)
{
    printf("IOTEST %s %s\n", ok ? "ok" : "NG", what);
    if (!ok) {
        fails++;
    }
}

static int heater(int fd, struct io_req *q)
{
    memset(q, 0, sizeof(*q));
    strcpy(q->name, "HEATER");
    return ioctl(fd, IOC_LOOKUP, q);
}

static int set_heater(int fd, int64_t milli)
{
    struct io_req q;
    if (heater(fd, &q) != 0) {
        return -1000;
    }
    q.value = milli;
    return ioctl(fd, IOC_SET, &q) == 0 ? 0 : -errno;
}

static int get_heater(int fd, int64_t *v)
{
    struct io_req q;
    if (heater(fd, &q) != 0) {
        return -1000;
    }
    if (ioctl(fd, IOC_GET, &q) != 0) {
        return -errno;
    }
    *v = q.value;
    return 0;
}

static int estop(int fd, int32_t v)
{
    return pwrite(fd, &v, 4, P_ESTOP * 4) == 4 ? 0 : -errno;
}

/* 子で fn を動かし、終了コードを返す */
static int in_child(int (*fn)(int), int fd)
{
    int st = -1;
    pid_t p = fork();
    if (p == 0) {
        _exit(fn(fd));
    }
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

/* uid 1000: 読める・動かせない・止められる・root に戻れない */
static int as_user(int fd)
{
    int64_t v = -1;
    if (setgid(100) != 0 || setuid(1000) != 0) return 1;
    if (getuid() != 1000 || geteuid() != 1000 || getgid() != 100) return 2;
    if (get_heater(fd, &v) != 0 || v != 30000) return 3;            /* 読める */
    if (set_heater(fd, 50000) != -EACCES) return 4;                  /* 動かせない */
    if (estop(fd, 0) != -EACCES) return 5;                           /* 解除もできない */
    if (setuid(0) == 0) return 6;                                    /* root には戻れない */
    if (estop(fd, 1) != 0) return 7;                                 /* 止めることはできる */
    return 0;
}

/* io のグループに入った uid 1000: 動かせる */
static int as_io_member(int fd)
{
    gid_t g = 20;
    if (setgroups(1, &g) != 0 || setgid(100) != 0 || setuid(1000) != 0) return 1;
    if (set_heater(fd, 40000) != 0) return 2;
    return 0;
}

/* ネットワークのソケットを開いた root: 読める・動かせない・安全な状態へはできる */
static int as_network(int fd)
{
    int64_t v = -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 1;
    if (get_heater(fd, &v) != 0) return 2;
    if (set_heater(fd, 60000) != -EACCES) return 3;
    {
        pid_t p = fork();                       /* その子も動かせない */
        int st = -1;
        if (p == 0) {
            _exit(set_heater(fd, 60000) == -EACCES ? 0 : 1);
        }
        waitpid(p, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return 4;
    }
    if (ioctl(fd, IOC_SAFE, 0) != 0) return 5;
    close(s);
    return 0;
}

int main(void)
{
    int fd = open("/dev/io", O_RDWR | O_CLOEXEC), lf, r;
    int64_t v = -1;
    static char log[16384];
    ssize_t n, total = 0;

    if (fd < 0) {
        printf("IOTEST NG open /dev/io\n");
        return 1;
    }
    check(estop(fd, 0) == 0 && set_heater(fd, 30000) == 0 && get_heater(fd, &v) == 0 && v == 30000,
          "root: ESTOP 0, set HEATER 30 and read it back");
    r = in_child(as_user, fd);
    printf("IOTEST user step %d\n", r);
    check(r == 0, "uid 1000: can read, cannot set or arm (EACCES), cannot become root again, can stop");
    check(estop(fd, 0) == 0 && in_child(as_io_member, fd) == 0 && get_heater(fd, &v) == 0 && v == 40000,
          "uid 1000 in group io (20): can set HEATER 40");
    r = in_child(as_network, fd);
    printf("IOTEST network step %d\n", r);
    check(r == 0 && get_heater(fd, &v) == 0 && v == 0,
          "after opening an AF_INET socket (and its child): can read, cannot set; safe state is allowed");
    estop(fd, 1);

    lf = open("/dev/iolog", O_RDONLY | O_CLOEXEC);
    while (lf >= 0 && (n = read(lf, log + total, sizeof(log) - 1 - (size_t)total)) > 0) {
        total += n;
    }
    log[total] = 0;
    check(lf >= 0 && strstr(log, "iotest uid 0 ARM") && strstr(log, "iotest uid 0 SET HEATER 30.000")
          && strstr(log, "iotest uid 1000 DENY-PERM HEATER") && strstr(log, "iotest uid 1000 STOP")
          && strstr(log, "iotest uid 1000 SET HEATER 40.000") && strstr(log, "iotest uid 0 net DENY-NET HEATER")
          && strstr(log, "iotest uid 0 net SAFE") && strstr(log, "+09:00 pid "),
          "/dev/iolog: time, pid, program, uid, ARM / SET value / DENY-PERM / DENY-NET / STOP / SAFE");
    {
        char *last = strstr(log, "SET HEATER 30.000");
        if (last) {
            char *eol = strchr(last, '\n');
            if (eol) *eol = 0;
            while (last > log && last[-1] != '\n') last--;
            printf("IOTEST sample: %s\n", last);
        }
    }
    close(lf);
    close(fd);
    printf("IOTEST done, %d failed\n", fails);
    return fails ? 1 : 0;
}
