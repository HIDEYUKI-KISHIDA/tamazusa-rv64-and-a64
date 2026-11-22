/* wltest.c — Wayland の下回りの試験（M38）。Linux の上でもそのまま動く（同じ結果になる）
 *
 *   Wayland の本物のライブラリは使わず、合成役とアプリがすることを、同じシステムコールで小さくまねる。
 *   1. memfd_create・ftruncate・mmap(MAP_SHARED)。fork した子と同じページ
 *   2. AF_UNIX: 合成役（親）が bind・listen、アプリ（子）が connect。SO_PEERCRED で相手のプロセス番号
 *   3. アプリが絵の memfd を作って描き、sendmsg の SCM_RIGHTS で fd を渡す。合成役が受け取って mmap し、
 *      絵を読む。合成役が書いた返事がアプリの側の写しにも見える（同じページ）
 *   4. epoll（待ち口・つながり・timerfd・eventfd をまとめて待つ）
 *   5. mremap で写した所を広げる（中身は残り、広げた所もファイルと共有）
 *   6. /dev/input/event0・event1: 名前と種類（ioctl）、キーと位置のできごと（カーネルの試験が 1 秒後に入れる）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/time.h>

/* Linux の <linux/input.h> から、使うところだけ（musl には Linux のヘッダがない） */
struct input_event { struct timeval time; unsigned short type, code; int value; };
struct input_absinfo { int value, minimum, maximum, fuzz, flat, resolution; };
#define EV_KEY 1
#define EV_ABS 3
#define ABS_X 0
#define ABS_Y 1
#define KEY_A 30
#define BTN_LEFT 0x110
#define EVIOCGNAME(len)      _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCGBIT(ev, len)   _IOC(_IOC_READ, 'E', 0x20 + (ev), len)
#define EVIOCGABS(abs)       _IOR('E', 0x40 + (abs), struct input_absinfo)

static int fails;

static void check(int ok, const char *what)
{
    printf("WLTEST %s %s\n", ok ? "ok" : "NG", what);
    if (!ok) {
        fails++;
    }
}

static int my_memfd(const char *name)
{
    return (int)syscall(SYS_memfd_create, name, 1 /* MFD_CLOEXEC */);
}

static int send_fd(int s, int fd, const char *msg)
{
    struct msghdr mh;
    struct iovec iov;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *c;
    memset(&mh, 0, sizeof(mh));
    memset(cbuf, 0, sizeof(cbuf));
    iov.iov_base = (void *)msg;
    iov.iov_len = strlen(msg) + 1;
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return (int)sendmsg(s, &mh, MSG_NOSIGNAL);
}

static int recv_fd(int s, char *buf, size_t len, int *fd)
{
    struct msghdr mh;
    struct iovec iov;
    char cbuf[CMSG_SPACE(sizeof(int) * 4)];
    struct cmsghdr *c;
    int n;
    memset(&mh, 0, sizeof(mh));
    iov.iov_base = buf;
    iov.iov_len = len;
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    *fd = -1;
    n = (int)recvmsg(s, &mh, MSG_CMSG_CLOEXEC);
    for (c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            memcpy(fd, CMSG_DATA(c), sizeof(int));
        }
    }
    return n;
}

#define W 64
#define H 64
#define SOCK_PATH "/tmp/wltest-0"

/* アプリ（子）: つないで、絵を描いた memfd を渡し、返事を待つ */
static int client(void)
{
    struct sockaddr_un sa;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0), fd, i;
    uint32_t *px;
    char buf[64];
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, SOCK_PATH);
    for (i = 0; i < 100 && connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0; i++) {
        usleep(10000);                          /* 合成役の待ち口ができるまで */
    }
    fd = my_memfd("wl-buffer");
    if (s < 0 || fd < 0 || ftruncate(fd, W * H * 4) != 0) {
        return 10;
    }
    px = mmap(NULL, W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        return 11;
    }
    for (i = 0; i < W * H; i++) {
        px[i] = 0xff000000u | (uint32_t)(i * 2654435761u >> 8);   /* 決まった模様 */
    }
    if (send_fd(s, fd, "attach 64x64") <= 0) {
        return 12;
    }
    close(fd);                                  /* 渡したので自分の fd は閉じてよい（写しは残る） */
    if (read(s, buf, sizeof(buf)) <= 0 || strcmp(buf, "release") != 0) {
        return 13;
    }
    if (px[0] != 0x12345678u) {                 /* 合成役が書いたものが見える */
        return 14;
    }
    munmap(px, W * H * 4);
    close(s);
    return 0;
}

int main(void)
{
    int ok;

    /* 1. memfd と MAP_SHARED、fork */
    {
        int fd = my_memfd("shared");
        uint32_t *m;
        pid_t pid;
        int st = -1;
        ok = fd >= 0 && ftruncate(fd, 8192) == 0;
        m = ok ? mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        ok = ok && m != MAP_FAILED;
        if (ok) {
            m[0] = 1;
            m[1024] = 2;                         /* 2 ページ目 */
            pid = fork();
            if (pid == 0) {
                m[0] = m[1024] + 40;             /* 子が書く */
                _exit(m[0] == 42 ? 0 : 1);
            }
            waitpid(pid, &st, 0);
            {
                uint32_t v = 0;
                pread(fd, &v, 4, 0);             /* ファイルとして読んでも同じ */
                ok = WIFEXITED(st) && WEXITSTATUS(st) == 0 && m[0] == 42 && v == 42;
            }
            munmap(m, 8192);
        }
        if (fd >= 0) {
            close(fd);
        }
        check(ok, "memfd + mmap MAP_SHARED: the child writes, the parent and the file see it");
    }
    /* 1b. 無名の MAP_SHARED */
    {
        volatile int *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        int st = -1;
        pid_t pid;
        ok = m != MAP_FAILED;
        if (ok) {
            *m = 5;
            pid = fork();
            if (pid == 0) {
                *m = 7;
                _exit(0);
            }
            waitpid(pid, &st, 0);
            ok = *m == 7;
            munmap((void *)m, 4096);
        }
        check(ok, "anonymous MAP_SHARED is shared with a forked child");
    }

    /* 2〜4. 合成役（親）とアプリ（子） */
    {
        struct sockaddr_un sa;
        int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        int ep = epoll_create1(EPOLL_CLOEXEC);
        int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        struct epoll_event ev, out[8];
        struct itimerspec its;
        pid_t pid;
        int cs = -1, st = -1, n, k, got_listen = 0, got_client = 0, got_timer = 0, got_event = 0, bfd = -1;
        char buf[64];
        uint32_t *px = MAP_FAILED;

        unlink(SOCK_PATH);
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, SOCK_PATH);
        ok = ls >= 0 && bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(ls, 4) == 0
             && ep >= 0 && tfd >= 0 && efd >= 0;
        check(ok, "AF_UNIX socket, bind, listen; epoll_create1, timerfd_create, eventfd");
        ev.events = EPOLLIN;
        ev.data.u64 = 1;
        epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev);
        ev.data.u64 = 3;
        epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
        ev.data.u64 = 4;
        epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);
        memset(&its, 0, sizeof(its));
        its.it_value.tv_nsec = 20 * 1000000;
        its.it_interval.tv_nsec = 20 * 1000000;
        timerfd_settime(tfd, 0, &its, NULL);
        {
            uint64_t one = 3;
            write(efd, &one, 8);
        }

        pid = fork();
        if (pid == 0) {
            _exit(client());
        }
        /* epoll で待つ: 待ち口 → accept、つながり → 絵を受け取る、timerfd・eventfd */
        for (k = 0; k < 200 && !(got_client && got_timer && got_event); k++) {
            n = epoll_wait(ep, out, 8, 50);
            for (int i = 0; i < n; i++) {
                if (out[i].data.u64 == 1) {
                    struct ucred cr;
                    socklen_t cl = sizeof(cr);
                    cs = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
                    got_listen = cs >= 0;
                    check(got_listen && getsockopt(cs, SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0 && cr.pid == pid,
                          "accept4 via epoll; SO_PEERCRED gives the client's process id");
                    ev.events = EPOLLIN;
                    ev.data.u64 = 2;
                    epoll_ctl(ep, EPOLL_CTL_ADD, cs, &ev);
                } else if (out[i].data.u64 == 2 && !got_client) {
                    n = recv_fd(cs, buf, sizeof(buf), &bfd);
                    got_client = 1;
                    check(n > 0 && strcmp(buf, "attach 64x64") == 0 && bfd >= 0 && fcntl(bfd, F_GETFD) == FD_CLOEXEC,
                          "recvmsg: message and a file descriptor (SCM_RIGHTS, MSG_CMSG_CLOEXEC)");
                } else if (out[i].data.u64 == 3 && !got_timer) {
                    uint64_t t = 0;
                    got_timer = read(tfd, &t, 8) == 8 && t >= 1;
                } else if (out[i].data.u64 == 4 && !got_event) {
                    uint64_t v = 0;
                    got_event = read(efd, &v, 8) == 8 && v == 3;
                }
            }
        }
        check(got_timer && got_event, "epoll_wait wakes for timerfd (20 ms period) and eventfd (value 3)");
        ok = 0;
        if (bfd >= 0) {
            px = mmap(NULL, W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);
            if (px != MAP_FAILED) {
                ok = 1;
                for (int i = 0; i < W * H; i++) {
                    if (px[i] != (0xff000000u | (uint32_t)(i * 2654435761u >> 8))) {
                        ok = 0;
                        break;
                    }
                }
                px[0] = 0x12345678u;            /* 返事: アプリの側の写しにも見えるはず */
            }
        }
        check(ok, "the compositor maps the received buffer and reads the client's picture");
        if (cs >= 0) {
            send(cs, "release", 8, MSG_NOSIGNAL);
        }
        waitpid(pid, &st, 0);
        check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "the client saw the compositor's write in its own mapping, exit 0");

        /* 5. mremap: 受け取ったファイルを広げて、写した所も広げる */
        ok = 0;
        if (px != MAP_FAILED && ftruncate(bfd, W * H * 8) == 0) {
            uint32_t *q = mremap(px, W * H * 4, W * H * 8, MREMAP_MAYMOVE);
            if (q != MAP_FAILED) {
                uint32_t v = 0xabcdef01u, r = 0;
                q[W * H + 5] = v;               /* 広げた所に書く */
                pread(bfd, &r, 4, (W * H + 5) * 4);
                ok = q[0] == 0x12345678u && r == v;
                px = q;
            }
        }
        check(ok, "mremap grows the shared mapping; old contents kept, new part shared with the file");
        if (px != MAP_FAILED) {
            munmap(px, W * H * 8);
        }
        if (bfd >= 0) close(bfd);
        if (cs >= 0) close(cs);
        close(ls);
        close(ep);
        close(tfd);
        close(efd);
    }

    /* 6. 入力のできごと */
    {
        int kb = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        int pt = open("/dev/input/event1", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        char name[64] = "";
        unsigned long evbits = 0, absbits = 0;
        struct input_absinfo ax;
        struct input_event e[64];
        struct pollfd pf[2];
        int key_down = 0, key_up = 0, gx = -1, gy = -1, btn = 0, k, n;

        ok = kb >= 0 && pt >= 0 && ioctl(pt, EVIOCGNAME(sizeof(name)), name) > 0
             && ioctl(pt, EVIOCGBIT(0, sizeof(evbits)), &evbits) >= 0
             && ioctl(pt, EVIOCGBIT(EV_ABS, sizeof(absbits)), &absbits) >= 0
             && ioctl(pt, EVIOCGABS(ABS_X), &ax) == 0
             && (evbits & (1 << EV_ABS)) && (absbits & 3) == 3 && ax.maximum == 1023
             && read(kb, e, sizeof(e)) < 0 && errno == EAGAIN;
        check(ok, "evdev: open, EVIOCGNAME, EVIOCGBIT, EVIOCGABS (0..1023), nonblocking read gives EAGAIN");
        printf("WLTEST input ready (%s)\n", name);
        fflush(stdout);
        pf[0].fd = kb;
        pf[0].events = POLLIN;
        pf[1].fd = pt;
        pf[1].events = POLLIN;
        for (k = 0; k < 50 && !(key_up && btn && gx >= 0 && gy >= 0); k++) {
            if (poll(pf, 2, 100) <= 0) {
                continue;
            }
            n = (int)read(kb, e, sizeof(e));
            for (int i = 0; i < n / (int)sizeof(e[0]); i++) {
                if (e[i].type == EV_KEY && e[i].code == KEY_A) {
                    if (e[i].value == 1) key_down = 1;
                    if (e[i].value == 0 && key_down) key_up = 1;
                }
            }
            n = (int)read(pt, e, sizeof(e));
            for (int i = 0; i < n / (int)sizeof(e[0]); i++) {
                if (e[i].type == EV_ABS && e[i].code == ABS_X) gx = e[i].value;
                if (e[i].type == EV_ABS && e[i].code == ABS_Y) gy = e[i].value;
                if (e[i].type == EV_KEY && e[i].code == BTN_LEFT && e[i].value == 1) btn = 1;
            }
        }
        check(key_down && key_up && gx == 100 && gy == 200 && btn,
              "evdev events: KEY_A down and up; ABS_X 100, ABS_Y 200, BTN_LEFT");
        if (kb >= 0) close(kb);
        if (pt >= 0) close(pt);
    }
    unlink(SOCK_PATH);
    printf("WLTEST done, %d failed\n", fails);
    return fails ? 1 : 0;
}
