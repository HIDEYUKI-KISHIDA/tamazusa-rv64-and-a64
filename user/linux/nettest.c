/* nettest.c — M25: ソケットの確認（ループバック 127.0.0.1 だけで完結する） */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int ok_count, ng_count;
static void check(int cond, const char *what)
{
    printf("nettest %2d: %s %s\n", ok_count + ng_count + 1, cond ? "OK" : "NG", what);
    fflush(stdout);
    if (cond) ok_count++; else ng_count++;
}

static struct sockaddr_in addr(const char *ip, int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

int main(void)
{
    struct sockaddr_in a = addr("127.0.0.1", 7001), peer;
    socklen_t pl = sizeof(peer);
    char buf[20000];
    int ls, cs, s, st, n, total, i;
    pid_t pid;

    /* 1-3. TCP: listen / connect / accept / 送受信（ループバック） */
    ls = socket(AF_INET, SOCK_STREAM, 0);
    check(ls >= 0 && bind(ls, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(ls, 4) == 0, "socket, bind, listen");
    pid = fork();
    if (pid == 0) {
        struct sockaddr_in c = addr("127.0.0.1", 7001);
        int k = socket(AF_INET, SOCK_STREAM, 0), j;
        if (connect(k, (struct sockaddr *)&c, sizeof(c)) != 0) {
            _exit(10);
        }
        write(k, "hello tcp", 9);
        for (j = 0; j < 20000; j++) {
            buf[j] = (char)(j * 7);
        }
        for (j = 0; j < 20000; ) {
            int w = (int)write(k, buf + j, 20000 - j);
            if (w <= 0) {
                _exit(11);
            }
            j += w;
        }
        n = (int)read(k, buf, sizeof(buf));
        _exit(n == 4 && memcmp(buf, "done", 4) == 0 ? 0 : 12);
    }
    s = accept(ls, (struct sockaddr *)&peer, &pl);
    check(s >= 0 && peer.sin_family == AF_INET, "accept a connection");
    n = (int)read(s, buf, 9);
    total = 0;
    while (total < 20000) {
        int r = (int)read(s, buf + total, sizeof(buf) - total);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    for (i = 0; i < total && buf[i] == (char)(i * 7); i++) {
    }
    check(n == 9 && total == 20000 && i == 20000, "receive 9 + 20000 bytes in order (bigger than the window)");
    write(s, "done", 4);
    waitpid(pid, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "client got the reply and exited 0");
    n = (int)read(s, buf, 10);
    check(n == 0, "read returns 0 after the peer closed");
    close(s);

    /* 6. つながっていないポートへは ECONNREFUSED */
    cs = socket(AF_INET, SOCK_STREAM, 0);
    a = addr("127.0.0.1", 7999);
    errno = 0;
    check(connect(cs, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == ECONNREFUSED, "connect to a closed port -> ECONNREFUSED");
    close(cs);

    /* 7. 待たない accept は EAGAIN、poll で待てる */
    fcntl(ls, F_SETFL, O_NONBLOCK);
    errno = 0;
    check(accept(ls, NULL, NULL) == -1 && errno == EAGAIN, "non-blocking accept -> EAGAIN");
    pid = fork();
    if (pid == 0) {
        struct sockaddr_in c = addr("127.0.0.1", 7001);
        int k = socket(AF_INET, SOCK_STREAM, 0);
        usleep(50000);
        connect(k, (struct sockaddr *)&c, sizeof(c));
        write(k, "x", 1);
        usleep(50000);
        _exit(0);
    }
    {
        struct pollfd pf = { ls, POLLIN, 0 };
        n = poll(&pf, 1, 3000);
        check(n == 1 && (pf.revents & POLLIN), "poll wakes up for a new connection");
    }
    s = accept(ls, NULL, NULL);
    close(s);
    waitpid(pid, NULL, 0);
    close(ls);

    /* 9. UDP（ループバック） */
    {
        int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in ua = addr("127.0.0.1", 7002), from;
        socklen_t fl = sizeof(from);
        bind(u1, (struct sockaddr *)&ua, sizeof(ua));
        sendto(u2, "udp!", 4, 0, (struct sockaddr *)&ua, sizeof(ua));
        n = (int)recvfrom(u1, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        check(n == 4 && memcmp(buf, "udp!", 4) == 0 && from.sin_port != 0, "UDP sendto / recvfrom");
        close(u1);
        close(u2);
    }

    /* 10. getsockname */
    {
        struct sockaddr_in me;
        socklen_t ml = sizeof(me);
        int k = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in b = addr("0.0.0.0", 7003);
        bind(k, (struct sockaddr *)&b, sizeof(b));
        listen(k, 1);
        check(getsockname(k, (struct sockaddr *)&me, &ml) == 0 && ntohs(me.sin_port) == 7003, "getsockname");
        close(k);
    }

    printf("nettest: %s (%d OK, %d NG)\n", ng_count == 0 ? "all checks passed" : "FAILED", ok_count, ng_count);
    return ng_count == 0 ? 0 : 1;
}
