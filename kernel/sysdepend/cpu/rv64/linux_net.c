/*
 * linux_net.c — Linux 互換層のソケット（AF_INET の TCP・UDP・ICMP）（S-mode + MMU のみ）
 *
 *   ソケットは表（socks）の 1 項目で、fd（linux_fs.c の FD_SOCK）が番号を持つ。
 *   fork・dup で参照が増え、最後の fd が閉じると TCP を閉じる（sshd などは
 *   ソケットを子に渡すので、参照で数える）。
 *   下の TCP（kernel/net/net_tcp.c）・UDP・ICMP（kernel/net/net_ip.c）は待たない作りなので、
 *   ここで「できるまで net_wait で眠って見直す」形にする（O_NONBLOCK なら -EAGAIN）。
 *   眠っている間にシグナルが来たら -EINTR。
 *   127.x.x.x は自分のアドレス（10.0.2.15）として扱う（ループバック）。
 */
#include <stdint.h>
#include <stddef.h>
#include <libstr.h>
#include "../../../net/net_ipv4.h"
#include "../../../net/net_tcp.h"

struct rv_uproc;
extern int  rv_user_copy_in(void *dst, uintptr_t uva, size_t len);
extern int  rv_user_copy_out(uintptr_t uva, const void *src, size_t len);
extern int  rv_linux_sig_interrupt(void);
extern void rv_linux_signal_self(int sig);
extern void net_kick(void);

#define EAGAIN      11
#define EINTR       4
#define EFAULT      14
#define EINVAL      22
#define EMFILE      24
#define EPIPE       32
#define ENOTSOCK    88
#define EDESTADDRREQ 89
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP  95
#define EAFNOSUPPORT 97
#define EISCONN     106
#define ENOTCONN    107
#define EINPROGRESS 115
#define EALREADY    114
#define ENOPROTOOPT 92

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#define SOCK_NONBLOCK 04000
#define MSG_PEEK    0x2
#define MSG_DONTWAIT 0x40
#define MSG_WAITALL 0x100
#define MSG_NOSIGNAL 0x4000

enum { SK_FREE = 0, SK_TCP, SK_UDP, SK_ICMP };

#define NSOCK 32
struct lsock {
    int kind;
    int refs;
    int id;                         /* TCP の pcb / UDP / ICMP の番号（-1 = まだ） */
    int listening;
    int nonblock;
    int connecting;
    uint32_t bip;                   /* bind したアドレス・ポート */
    uint16_t bport;
    uint32_t pip;                   /* UDP の connect の相手 */
    uint16_t pport;
    int connected;
};
static struct lsock socks[NSOCK];

static struct lsock *sk(int i)
{
    return (i >= 0 && i < NSOCK && socks[i].kind != SK_FREE) ? &socks[i] : NULL;
}

/* struct sockaddr_in { u16 family; u16 port（ネットワーク順）; u32 addr（線の順）; u8 zero[8]; } */
static int get_addr(uintptr_t ua, long len, uint32_t *ip, uint16_t *port)
{
    uint8_t sa[16];
    if (ua == 0 || len < 8 || rv_user_copy_in(sa, ua, 8) != 0) {
        return -EFAULT;
    }
    if ((sa[0] | (sa[1] << 8)) != AF_INET) {
        return -EAFNOSUPPORT;
    }
    *port = (uint16_t)((sa[2] << 8) | sa[3]);
    *ip = (uint32_t)sa[4] | ((uint32_t)sa[5] << 8) | ((uint32_t)sa[6] << 16) | ((uint32_t)sa[7] << 24);
    return 0;
}

static int put_addr(uintptr_t ua, uintptr_t ulen, uint32_t ip, uint16_t port)
{
    uint8_t sa[16] = { 0 };
    uint32_t len;
    if (ua == 0) {
        return 0;
    }
    if (ulen == 0 || rv_user_copy_in(&len, ulen, 4) != 0) {
        return -EFAULT;
    }
    sa[0] = AF_INET;
    sa[2] = (uint8_t)(port >> 8);
    sa[3] = (uint8_t)port;
    sa[4] = (uint8_t)ip; sa[5] = (uint8_t)(ip >> 8); sa[6] = (uint8_t)(ip >> 16); sa[7] = (uint8_t)(ip >> 24);
    if (rv_user_copy_out(ua, sa, len < 16 ? len : 16) != 0) {
        return -EFAULT;
    }
    len = 16;
    return rv_user_copy_out(ulen, &len, 4) == 0 ? 0 : -EFAULT;
}

/* 眠って見直す。シグナルなら -EINTR、それ以外 0 */
static int wait_net(void)
{
    net_kick();
    net_wait(10);
    return rv_linux_sig_interrupt() ? -EINTR : 0;
}

/* ---- 作る・参照・閉じる ---- */

int rv_sock_create(long domain, long type, long proto)
{
    int i, kind;
    long base = type & 0xf;

    if (domain != AF_INET) {
        return -EAFNOSUPPORT;       /* AF_INET6 や AF_UNIX はまだ（プログラムは IPv4 に戻る） */
    }
    if (base == SOCK_STREAM && (proto == 0 || proto == 6)) {
        kind = SK_TCP;
    } else if (base == SOCK_DGRAM && (proto == 0 || proto == 17)) {
        kind = SK_UDP;
    } else if ((base == SOCK_DGRAM || base == SOCK_RAW) && proto == 1) {
        kind = SK_ICMP;
    } else {
        return -EPROTONOSUPPORT;
    }
    for (i = 0; i < NSOCK; i++) {
        if (socks[i].kind == SK_FREE) {
            memset(&socks[i], 0, sizeof(socks[i]));
            socks[i].kind = kind;
            socks[i].refs = 1;
            socks[i].id = -1;
            socks[i].nonblock = (type & SOCK_NONBLOCK) ? 1 : 0;
            net_lock();
            if (kind == SK_UDP) {
                socks[i].id = udp_open();
            } else if (kind == SK_ICMP) {
                socks[i].id = icmp_open(base == SOCK_RAW);
            }
            net_unlock();
            if (kind != SK_TCP && socks[i].id < 0) {
                socks[i].kind = SK_FREE;
                return -EMFILE;
            }
            return i;
        }
    }
    return -EMFILE;
}

void rv_sock_ref(int i)
{
    if (sk(i)) {
        socks[i].refs++;
    }
}

void rv_sock_unref(int i)
{
    struct lsock *s = sk(i);
    if (s == NULL || --s->refs > 0) {
        return;
    }
    net_lock();
    if (s->kind == SK_TCP && s->id >= 0) {
        tcp_close(s->id);
    } else if (s->kind == SK_UDP) {
        udp_close(s->id);
    } else if (s->kind == SK_ICMP) {
        icmp_close(s->id);
    }
    net_unlock();
    net_kick();
    s->kind = SK_FREE;
}

void rv_sock_set_nonblock(int i, int on)
{
    if (sk(i)) {
        socks[i].nonblock = on;
    }
}

int rv_sock_get_nonblock(int i)
{
    return sk(i) ? socks[i].nonblock : 0;
}

/* poll の形の状態 */
int rv_sock_events(int i)
{
    struct lsock *s = sk(i);
    int ev = 0, f;
    if (s == NULL) {
        return 0x20;
    }
    net_lock();
    if (s->kind == SK_TCP) {
        if (s->id < 0) {
            ev = 0x4 | 0x10;            /* まだつながっていない */
        } else {
            f = tcp_poll_flags(s->id);
            if (f & TCPF_IN)    ev |= 0x1;
            if (f & TCPF_OUT)   ev |= 0x4;
            if (f & TCPF_ERR)   ev |= 0x8;
            if (f & TCPF_HUP)   ev |= 0x10;
            if (f & TCPF_RDHUP) ev |= 0x2000 | 0x1;
        }
    } else if (s->kind == SK_UDP) {
        ev = 0x4 | (udp_readable(s->id) ? 0x1 : 0);
    } else {
        ev = 0x4 | (icmp_readable(s->id) ? 0x1 : 0);
    }
    net_unlock();
    return ev;
}

/* ---- bind・listen・accept・connect ---- */

long rv_sock_bind(int i, uintptr_t ua, long len)
{
    struct lsock *s = sk(i);
    uint32_t ip;
    uint16_t port;
    long r;
    if (s == NULL) {
        return -ENOTSOCK;
    }
    r = get_addr(ua, len, &ip, &port);
    if (r != 0) {
        return r;
    }
    ip = net_map_loopback(ip);
    s->bip = ip;
    s->bport = port;
    if (s->kind == SK_UDP) {
        net_lock();
        r = udp_bind(s->id, ip, port);
        net_unlock();
        return r;
    }
    return 0;
}

long rv_sock_listen(int i, long backlog)
{
    struct lsock *s = sk(i);
    int id;
    if (s == NULL) {
        return -ENOTSOCK;
    }
    if (s->kind != SK_TCP) {
        return -EOPNOTSUPP;
    }
    if (s->listening) {
        return 0;
    }
    net_lock();
    id = tcp_listen(s->bip, s->bport, backlog > 0 ? (int)backlog : 4);
    net_unlock();
    if (id < 0) {
        return id;
    }
    s->id = id;
    s->listening = 1;
    return 0;
}

/* 戻り値: 新しいソケットの番号 / 負のエラー */
long rv_sock_accept(int i, uintptr_t ua, uintptr_t ulen, long flags)
{
    struct lsock *s = sk(i);
    uint32_t rip;
    uint16_t rport;
    int id, k, r;
    if (s == NULL) {
        return -ENOTSOCK;
    }
    if (!s->listening) {
        return -EINVAL;
    }
    for (;;) {
        net_lock();
        id = tcp_accept(s->id, &rip, &rport);
        net_unlock();
        if (id != -EAGAIN) {
            break;
        }
        if (s->nonblock) {
            return -EAGAIN;
        }
        if ((r = wait_net()) != 0) {
            return r;
        }
    }
    if (id < 0) {
        return id;
    }
    for (k = 0; k < NSOCK; k++) {
        if (socks[k].kind == SK_FREE) {
            memset(&socks[k], 0, sizeof(socks[k]));
            socks[k].kind = SK_TCP;
            socks[k].refs = 1;
            socks[k].id = id;
            socks[k].nonblock = (flags & SOCK_NONBLOCK) ? 1 : 0;
            r = put_addr(ua, ulen, rip, rport);
            return r < 0 ? r : k;
        }
    }
    net_lock();
    tcp_close(id);
    net_unlock();
    return -EMFILE;
}

long rv_sock_connect(int i, uintptr_t ua, long len)
{
    struct lsock *s = sk(i);
    uint32_t ip;
    uint16_t port;
    long r;
    int st;
    if (s == NULL) {
        return -ENOTSOCK;
    }
    r = get_addr(ua, len, &ip, &port);
    if (r != 0) {
        return r;
    }
    ip = net_map_loopback(ip);
    if (s->kind != SK_TCP) {
        s->pip = ip;                /* UDP・ICMP: 決まった相手 */
        s->pport = port;
        s->connected = 1;
        return 0;
    }
    if (s->id >= 0 && !s->connecting) {
        return -EISCONN;
    }
    if (s->id < 0) {
        net_lock();
        s->id = tcp_connect(ip, port, s->bport);
        net_unlock();
        if (s->id < 0) {
            r = s->id;
            s->id = -1;
            return r;
        }
        s->connecting = 1;
        net_kick();
    }
    for (;;) {
        net_lock();
        st = tcp_connect_status(s->id);
        net_unlock();
        if (st != -EINPROGRESS) {
            s->connecting = 0;
            return st;
        }
        if (s->nonblock) {
            return -EINPROGRESS;
        }
        if ((r = wait_net()) != 0) {
            return r;
        }
    }
}

long rv_sock_name(int i, uintptr_t ua, uintptr_t ulen, int peer)
{
    struct lsock *s = sk(i);
    uint32_t lip = 0, rip = 0;
    uint16_t lport = 0, rport = 0;
    if (s == NULL) {
        return -ENOTSOCK;
    }
    net_lock();
    if (s->kind == SK_TCP && s->id >= 0) {
        tcp_get_addrs(s->id, &lip, &lport, &rip, &rport);
        if (s->listening) {
            lip = s->bip;
            lport = s->bport;
        }
    } else if (s->kind == SK_UDP) {
        udp_local(s->id, &lip, &lport);
        rip = s->pip;
        rport = s->pport;
    } else {
        lip = s->bip;
        lport = s->bport;
    }
    net_unlock();
    if (peer) {
        if ((s->kind == SK_TCP && (s->id < 0 || s->listening)) || (s->kind != SK_TCP && !s->connected)) {
            return -ENOTCONN;
        }
        return put_addr(ua, ulen, rip, rport);
    }
    return put_addr(ua, ulen, lip, lport);
}

/* ---- 送る・受け取る ---- */

long rv_sock_send(int i, uintptr_t ubuf, size_t len, long flags, uintptr_t ua, long alen)
{
    struct lsock *s = sk(i);
    uint8_t chunk[1472];
    size_t done = 0;
    long r;
    if (s == NULL) {
        return -ENOTSOCK;
    }
    if (s->kind != SK_TCP) {
        uint32_t ip = s->pip;
        uint16_t port = s->pport;
        if (ua != 0) {
            if ((r = get_addr(ua, alen, &ip, &port)) != 0) {
                return r;
            }
            ip = net_map_loopback(ip);
        } else if (!s->connected) {
            return -EDESTADDRREQ;
        }
        if (len > sizeof(chunk)) {
            return -90;                 /* EMSGSIZE */
        }
        if (rv_user_copy_in(chunk, ubuf, len) != 0) {
            return -EFAULT;
        }
        net_lock();
        r = (s->kind == SK_UDP) ? udp_sendto(s->id, ip, port, chunk, len) : icmp_sendto(s->id, ip, chunk, len);
        net_unlock();
        net_kick();
        return r;
    }
    if (s->id < 0 || s->listening) {
        return -ENOTCONN;
    }
    while (done < len) {
        size_t n = len - done;
        int k;
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        if (rv_user_copy_in(chunk, ubuf + done, n) != 0) {
            return done ? (long)done : -EFAULT;
        }
        net_lock();
        k = tcp_send(s->id, chunk, n);
        net_unlock();
        net_kick();
        if (k > 0) {
            done += (size_t)k;
            continue;
        }
        if (k == -EAGAIN) {
            if (s->nonblock || (flags & MSG_DONTWAIT)) {
                return done ? (long)done : -EAGAIN;
            }
            if ((r = wait_net()) != 0) {
                return done ? (long)done : r;
            }
            continue;
        }
        if (k == -EPIPE && !(flags & MSG_NOSIGNAL)) {
            rv_linux_signal_self(13);   /* SIGPIPE */
        }
        return done ? (long)done : k;
    }
    return (long)done;
}

long rv_sock_recv(int i, uintptr_t ubuf, size_t len, long flags, uintptr_t ua, uintptr_t ulen)
{
    struct lsock *s = sk(i);
    uint8_t chunk[1500];
    size_t done = 0;
    long r;
    if (s == NULL) {
        return -ENOTSOCK;
    }
    if (len == 0) {
        return 0;
    }
    if (s->kind != SK_TCP) {
        uint32_t ip = 0;
        uint16_t port = 0;
        int k;
        for (;;) {
            net_lock();
            k = (s->kind == SK_UDP)
                ? udp_recv(s->id, chunk, len < sizeof(chunk) ? len : sizeof(chunk), &ip, &port, (flags & MSG_PEEK) != 0)
                : icmp_recv(s->id, chunk, len < sizeof(chunk) ? len : sizeof(chunk), &ip, (flags & MSG_PEEK) != 0);
            net_unlock();
            if (k != -EAGAIN) {
                break;
            }
            if (s->nonblock || (flags & MSG_DONTWAIT)) {
                return -EAGAIN;
            }
            if ((r = wait_net()) != 0) {
                return r;
            }
        }
        if (k < 0) {
            return k;
        }
        if (rv_user_copy_out(ubuf, chunk, (size_t)k) != 0) {
            return -EFAULT;
        }
        r = put_addr(ua, ulen, ip, port);
        return r < 0 ? r : k;
    }
    if (s->id < 0 || s->listening) {
        return -ENOTCONN;
    }
    for (;;) {
        size_t want = len - done;
        int k;
        if (want > sizeof(chunk)) {
            want = sizeof(chunk);
        }
        net_lock();
        k = tcp_recv(s->id, chunk, want, (flags & MSG_PEEK) != 0);
        net_unlock();
        if (k > 0) {
            net_kick();                 /* 窓が開いたことを相手へ */
            if (rv_user_copy_out(ubuf + done, chunk, (size_t)k) != 0) {
                return done ? (long)done : -EFAULT;
            }
            done += (size_t)k;
            if (done >= len || !(flags & MSG_WAITALL) || (flags & MSG_PEEK)) {
                return (long)done;
            }
            continue;
        }
        if (k == 0) {
            return (long)done;          /* 相手が閉じた */
        }
        if (k != -EAGAIN) {
            return done ? (long)done : k;
        }
        if (done > 0 && !(flags & MSG_WAITALL)) {
            return (long)done;
        }
        if (s->nonblock || (flags & MSG_DONTWAIT)) {
            return done ? (long)done : -EAGAIN;
        }
        if ((r = wait_net()) != 0) {
            return done ? (long)done : r;
        }
    }
}

long rv_sock_shutdown(int i, long how)
{
    struct lsock *s = sk(i);
    long r;
    if (s == NULL) {
        return -ENOTSOCK;
    }
    if (s->kind != SK_TCP || s->id < 0) {
        return -ENOTCONN;
    }
    net_lock();
    r = tcp_shutdown(s->id, (int)how);
    net_unlock();
    net_kick();
    return r;
}

long rv_sock_getsockopt(int i, long level, long name, uintptr_t uval, uintptr_t ulen)
{
    struct lsock *s = sk(i);
    int32_t v = 0;
    uint32_t len = 4;
    if (s == NULL) {
        return -ENOTSOCK;
    }
    if (level == 1 && name == 4) {          /* SOL_SOCKET, SO_ERROR */
        if (s->kind == SK_TCP && s->id >= 0) {
            net_lock();
            v = tcp_take_error(s->id);
            net_unlock();
        }
    } else if (level == 1 && name == 3) {   /* SO_TYPE */
        v = (s->kind == SK_TCP) ? SOCK_STREAM : SOCK_DGRAM;
    } else if (level == 1 && (name == 7 || name == 8)) {   /* SO_SNDBUF / SO_RCVBUF */
        v = 4096;
    } else if (level == 1 && name == 30) {  /* SO_ACCEPTCONN */
        v = s->listening;
    }
    if (rv_user_copy_out(uval, &v, 4) != 0 || (ulen && rv_user_copy_out(ulen, &len, 4) != 0)) {
        return -EFAULT;
    }
    return 0;
}

int rv_sock_rx_avail(int i)
{
    struct lsock *s = sk(i);
    int n = 0;
    if (s == NULL) {
        return 0;
    }
    net_lock();
    if (s->kind == SK_TCP && s->id >= 0 && !s->listening) {
        n = tcp_rx_avail(s->id);
    } else if (s->kind == SK_UDP && udp_readable(s->id)) {
        n = 1;
    }
    net_unlock();
    return n;
}

int rv_sock_count(void)
{
    int i, n = 0;
    for (i = 0; i < NSOCK; i++) {
        n += socks[i].kind != SK_FREE;
    }
    return n;
}
