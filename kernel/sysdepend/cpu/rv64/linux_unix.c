/* linux_unix.c — UNIX ドメインのソケット（AF_UNIX、SOCK_STREAM）（S-mode + MMU のみ）
 *
 *   Wayland の下回りの 2 つ目。Wayland のアプリは、合成役の待ち口（例 /tmp/wayland-0）につなぎ、
 *   決まった形の短いメッセージをやりとりする。窓の絵の memfd などは、fd そのものを
 *   sendmsg の SCM_RIGHTS で渡す（受け取った側に、同じファイルを指す新しい fd ができる）。
 *     ・socket(AF_UNIX)・bind（パスの名前、頭が 0 なら抽象の名前）・listen・connect・accept・socketpair
 *     ・read・write・send・recv・sendmsg・recvmsg（SCM_RIGHTS は linux_fs.c が fd の中身を写して渡す）
 *     ・getsockopt の SO_PEERCRED（相手のプロセス番号。Wayland の合成役が使う）
 *   名前はこのファイルの中の表で持つ（ファイルの木に印は作らない。stat すると「ない」）。
 *   受ける入れ物は 1 本 16KiB。渡す fd は、送ったバイトの位置と一緒に並べ、そのバイトを読んだときに渡す。
 */
#include <stdint.h>
#include <stddef.h>

extern int rv_user_copy_in(void *dst, uintptr_t uva, size_t len);
extern int rv_user_copy_out(uintptr_t udst, const void *src, size_t n);
extern int tk_dis_dsp(void);
extern int tk_ena_dsp(void);
extern int tk_slp_tsk(int tmout);
extern int tk_wup_tsk(int tskid);
extern int tk_get_tid(void);
extern int rv_linux_sig_interrupt(void);
extern void rv_linux_fd_blob_drop(void *blob);

#define NUS         32
#define UBUF        16384
#define NFDQ        28
#define FDBLOB      192
#define NBACK       8

#define SOCK_STREAM 1
#define AF_UNIX     1

#define EPERM   1
#define EINTR   4
#define EBADF   9
#define EAGAIN  11
#define ENOMEM  12
#define EFAULT  14
#define EINVAL  22
#define EMFILE  24
#define EPIPE   32
#define ENAMETOOLONG 36
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define EADDRINUSE 98
#define EISCONN 106
#define ENOTCONN 107
#define ECONNREFUSED 111
#define ETOOMANYREFS 109

enum { US_NEW, US_BOUND, US_LISTEN, US_CONN, US_CLOSED };

struct fdq { uint64_t pos; uint8_t blob[FDBLOB]; };
struct usock {
    int used, refs, state;
    char path[108];
    int pathlen;                    /* 0 = 名前なし */
    int peer;                       /* つながった相手（-1 = なし） */
    uint8_t buf[UBUF];              /* 受けたバイト */
    unsigned head, tail, count;
    uint64_t in_total, out_total;   /* これまでに入れた・読み出したバイトの数 */
    struct fdq q[NFDQ];             /* 渡された fd（pos = 送られたときの in_total） */
    int nq;
    int back[NBACK], nback;         /* 待ち口: まだ accept されていない、つながった側の番号 */
    int rwait, wwait;
    int pid, peer_pid;
    int shut_rd, shut_wr;
};
static struct usock us[NUS];
volatile unsigned long rv_usock_fds_passed;     /* テスト用 */

static void wake(int *who)
{
    if (*who > 0) {
        int t = *who;
        *who = 0;
        tk_wup_tsk(t);
    }
}

static int alloc_us(void)
{
    int i;
    for (i = 0; i < NUS; i++) {
        if (!us[i].used) {
            struct usock *u = &us[i];
            u->used = 1;
            u->refs = 1;
            u->state = US_NEW;
            u->pathlen = 0;
            u->peer = -1;
            u->head = u->tail = u->count = 0;
            u->in_total = u->out_total = 0;
            u->nq = 0;
            u->nback = 0;
            u->rwait = u->wwait = 0;
            u->pid = u->peer_pid = 0;
            u->shut_rd = u->shut_wr = 0;
            return i;
        }
    }
    return -EMFILE;
}

int rv_usock_create(int type, int pid)
{
    int i;
    if ((type & 0xf) != SOCK_STREAM) {
        return -EPROTONOSUPPORT;    /* SOCK_DGRAM・SOCK_SEQPACKET は、まだ */
    }
    tk_dis_dsp();
    i = alloc_us();
    if (i >= 0) {
        us[i].pid = pid;
    }
    tk_ena_dsp();
    return i;
}

int rv_usock_pair(int type, int pid, int *a, int *b)
{
    int i, j;
    if ((type & 0xf) != SOCK_STREAM) {
        return -EPROTONOSUPPORT;
    }
    tk_dis_dsp();
    i = alloc_us();
    j = i >= 0 ? alloc_us() : -EMFILE;
    if (j < 0) {
        if (i >= 0) {
            us[i].used = 0;
        }
        tk_ena_dsp();
        return -EMFILE;
    }
    us[i].state = us[j].state = US_CONN;
    us[i].peer = j;
    us[j].peer = i;
    us[i].pid = us[j].pid = us[i].peer_pid = us[j].peer_pid = pid;
    tk_ena_dsp();
    *a = i;
    *b = j;
    return 0;
}

void rv_usock_ref(int i)
{
    if (i >= 0 && i < NUS && us[i].used) {
        us[i].refs++;
    }
}

static void destroy(int i);

void rv_usock_unref(int i)
{
    if (i < 0 || i >= NUS || !us[i].used) {
        return;
    }
    tk_dis_dsp();
    if (--us[i].refs <= 0) {
        destroy(i);
    }
    tk_ena_dsp();
}

/* 最後の fd が閉じた（tk_dis_dsp の中で） */
static void destroy(int i)
{
    struct usock *u = &us[i];
    int k;
    if (u->peer >= 0 && us[u->peer].used) {
        struct usock *p = &us[u->peer];
        p->peer = -1;
        p->state = US_CLOSED;       /* 相手から見ると、もう送ってこない（読み切ったら 0） */
        wake(&p->rwait);
        wake(&p->wwait);
    }
    for (k = 0; k < u->nback; k++) {    /* accept されなかったつながりは切る */
        int c = u->back[k];
        us[c].refs = 1;
        destroy(c);
    }
    u->nback = 0;
    u->used = 0;
    tk_ena_dsp();
    for (k = 0; k < u->nq; k++) {       /* 読まれなかった fd を閉じる */
        rv_linux_fd_blob_drop(u->q[k].blob);
    }
    tk_dis_dsp();
    u->nq = 0;
}

/* struct sockaddr_un を読む。名前の長さ（抽象の名前は頭の 0 を含む）か負のエラー */
static int read_addr(uintptr_t uaddr, long alen, char *path)
{
    uint8_t sa[110];
    int n, k;
    if (alen < 3 || alen > 110) {
        return alen > 110 ? -ENAMETOOLONG : -EINVAL;
    }
    if (rv_user_copy_in(sa, uaddr, (size_t)alen) != 0) {
        return -EFAULT;
    }
    if ((sa[0] | (sa[1] << 8)) != AF_UNIX) {
        return -EINVAL;
    }
    n = (int)alen - 2;
    if (sa[2] != 0) {                   /* パスの名前: 0 までの長さ */
        for (k = 0; k < n && sa[2 + k]; k++) {
        }
        n = k;
    }
    for (k = 0; k < n; k++) {
        path[k] = (char)sa[2 + k];
    }
    return n;
}

static int find_name(const char *path, int n)
{
    int i, k;
    for (i = 0; i < NUS; i++) {
        if (us[i].used && us[i].pathlen == n && (us[i].state == US_BOUND || us[i].state == US_LISTEN)) {
            for (k = 0; k < n && us[i].path[k] == path[k]; k++) {
            }
            if (k == n) {
                return i;
            }
        }
    }
    return -1;
}

long rv_usock_bind(int i, uintptr_t uaddr, long alen)
{
    char path[108];
    int n = read_addr(uaddr, alen, path), k;
    if (n < 0) {
        return n;
    }
    if (n == 0 || us[i].state != US_NEW || us[i].pathlen) {
        return -EINVAL;
    }
    tk_dis_dsp();
    if (find_name(path, n) >= 0) {
        tk_ena_dsp();
        return -EADDRINUSE;
    }
    for (k = 0; k < n; k++) {
        us[i].path[k] = path[k];
    }
    us[i].pathlen = n;
    us[i].state = US_BOUND;
    tk_ena_dsp();
    return 0;
}

long rv_usock_listen(int i, long backlog)
{
    (void)backlog;
    if (us[i].state != US_BOUND && us[i].state != US_LISTEN) {
        return -EINVAL;
    }
    us[i].state = US_LISTEN;
    return 0;
}

long rv_usock_connect(int i, uintptr_t uaddr, long alen, int pid)
{
    char path[108];
    int n = read_addr(uaddr, alen, path), l, c;
    if (n < 0) {
        return n;
    }
    if (us[i].state == US_CONN) {
        return -EISCONN;
    }
    if (us[i].state != US_NEW && us[i].state != US_BOUND) {
        return -EINVAL;
    }
    tk_dis_dsp();
    l = find_name(path, n);
    if (l < 0 || us[l].state != US_LISTEN) {
        tk_ena_dsp();
        return -ECONNREFUSED;
    }
    if (us[l].nback >= NBACK) {
        tk_ena_dsp();
        return -EAGAIN;
    }
    c = alloc_us();                     /* 待ち口の側の新しい端 */
    if (c < 0) {
        tk_ena_dsp();
        return -ENOMEM;
    }
    us[c].state = US_CONN;
    us[c].peer = i;
    us[c].pid = us[l].pid;
    us[c].peer_pid = pid;
    us[c].pathlen = us[l].pathlen;
    for (n = 0; n < us[l].pathlen; n++) {
        us[c].path[n] = us[l].path[n];
    }
    us[i].state = US_CONN;
    us[i].peer = c;
    us[i].peer_pid = us[l].pid;
    us[i].pid = pid;
    us[l].back[us[l].nback++] = c;
    wake(&us[l].rwait);                 /* accept を待っている側を起こす */
    tk_ena_dsp();
    return 0;
}

/* 名前を書き出す（struct sockaddr_un） */
static long put_name(int i, uintptr_t uaddr, uintptr_t ulen)
{
    uint8_t sa[110];
    uint32_t len;
    int n = (i >= 0) ? us[i].pathlen : 0, k;
    if (uaddr == 0) {
        return 0;
    }
    if (rv_user_copy_in(&len, ulen, 4) != 0) {
        return -EFAULT;
    }
    sa[0] = AF_UNIX;
    sa[1] = 0;
    for (k = 0; k < n; k++) {
        sa[2 + k] = (uint8_t)us[i].path[k];
    }
    sa[2 + n] = 0;
    k = 2 + n + (n > 0 && us[i].path[0] != 0 ? 1 : 0);
    if (rv_user_copy_out(uaddr, sa, len < (uint32_t)k ? len : (uint32_t)k) != 0) {
        return -EFAULT;
    }
    len = (uint32_t)k;
    return rv_user_copy_out(ulen, &len, 4) == 0 ? 0 : -EFAULT;
}

long rv_usock_accept(int i, int nonblock, uintptr_t uaddr, uintptr_t ulen)
{
    int c, k;
    if (us[i].state != US_LISTEN) {
        return -EINVAL;
    }
    for (;;) {
        tk_dis_dsp();
        if (us[i].nback > 0) {
            c = us[i].back[0];
            for (k = 1; k < us[i].nback; k++) {
                us[i].back[k - 1] = us[i].back[k];
            }
            us[i].nback--;
            tk_ena_dsp();
            put_name(-1, uaddr, ulen);  /* つないできた側は名前なし */
            return c;
        }
        if (nonblock) {
            tk_ena_dsp();
            return -EAGAIN;
        }
        us[i].rwait = tk_get_tid();
        tk_ena_dsp();
        tk_slp_tsk(50);
        if (rv_linux_sig_interrupt()) {
            return -EINTR;
        }
    }
}

long rv_usock_name(int i, uintptr_t uaddr, uintptr_t ulen, int peer)
{
    if (peer) {
        if (us[i].state != US_CONN) {
            return -ENOTCONN;
        }
        return put_name(us[i].peer >= 0 && us[us[i].peer].state == US_CONN && us[i].pathlen == 0 ? -1 : i,
                        uaddr, ulen);
    }
    return put_name(us[i].state == US_CONN && us[i].peer >= 0 && us[us[i].peer].pathlen && !us[i].pathlen ? -1 : i,
                    uaddr, ulen);
}

/* SO_PEERCRED: struct ucred { pid, uid, gid } */
long rv_usock_peercred(int i, uintptr_t uval, uintptr_t ulen)
{
    int32_t cred[3];
    uint32_t len;
    if (rv_user_copy_in(&len, ulen, 4) != 0) {
        return -EFAULT;
    }
    cred[0] = us[i].state == US_CONN || us[i].state == US_CLOSED ? us[i].peer_pid : 0;
    cred[1] = 0;
    cred[2] = 0;
    if (len > sizeof(cred)) {
        len = sizeof(cred);
    }
    if (rv_user_copy_out(uval, cred, len) != 0) {
        return -EFAULT;
    }
    return rv_user_copy_out(ulen, &len, 4) == 0 ? 0 : -EFAULT;
}

long rv_usock_shutdown(int i, long how)
{
    tk_dis_dsp();
    if (how == 0 || how == 2) {
        us[i].shut_rd = 1;
        wake(&us[i].rwait);
    }
    if (how == 1 || how == 2) {
        us[i].shut_wr = 1;
        if (us[i].peer >= 0) {
            us[us[i].peer].shut_rd = 2;     /* 相手は読み切ったら 0 */
            wake(&us[us[i].peer].rwait);
        }
    }
    tk_ena_dsp();
    return 0;
}

/* 送る。blobs は渡す fd の中身（nb 個）。送れたバイト数か負のエラー。
 * fd だけを渡すことはできない（Linux と同じく、1 バイト以上と一緒に） */
long rv_usock_send(int i, uintptr_t ubuf, size_t len, int nonblock, const void *blobs, int nb)
{
    struct usock *u = &us[i];
    size_t done = 0;
    int attached = 0;

    if (u->state != US_CONN && u->state != US_CLOSED) {
        return -ENOTCONN;
    }
    if (nb > 0 && len == 0) {
        return -EINVAL;
    }
    while (done < len) {
        struct usock *p;
        size_t n, k;
        tk_dis_dsp();
        if (u->shut_wr || u->peer < 0 || !us[u->peer].used) {
            tk_ena_dsp();
            return done > 0 ? (long)done : -EPIPE;
        }
        p = &us[u->peer];
        if (!attached && nb > 0) {
            int m;
            if (p->nq + nb > NFDQ) {
                tk_ena_dsp();
                return -ETOOMANYREFS;
            }
            for (m = 0; m < nb; m++) {
                const uint8_t *src = (const uint8_t *)blobs + (size_t)m * FDBLOB;
                for (k = 0; k < FDBLOB; k++) {
                    p->q[p->nq].blob[k] = src[k];
                }
                p->q[p->nq].pos = p->in_total;
                p->nq++;
            }
            attached = 1;
            rv_usock_fds_passed += (unsigned)nb;
        }
        n = UBUF - p->count;
        if (n == 0) {
            if (nonblock) {
                tk_ena_dsp();
                return done > 0 ? (long)done : -EAGAIN;
            }
            u->wwait = tk_get_tid();
            tk_ena_dsp();
            tk_slp_tsk(50);
            if (rv_linux_sig_interrupt()) {
                return done > 0 ? (long)done : -EINTR;
            }
            continue;
        }
        if (n > len - done) {
            n = len - done;
        }
        if (n > 512) {
            n = 512;                        /* 割込みを長く止めないよう、少しずつ */
        }
        tk_ena_dsp();
        {
            uint8_t tmp[512];
            if (rv_user_copy_in(tmp, ubuf + done, n) != 0) {
                return done > 0 ? (long)done : -EFAULT;
            }
            tk_dis_dsp();
            if (u->peer < 0 || &us[u->peer] != p || !p->used) {
                tk_ena_dsp();
                return done > 0 ? (long)done : -EPIPE;
            }
            if (n > UBUF - p->count) {
                n = UBUF - p->count;
            }
            for (k = 0; k < n; k++) {
                p->buf[p->head] = tmp[k];
                p->head = (p->head + 1) % UBUF;
            }
            p->count += (unsigned)n;
            p->in_total += n;
            wake(&p->rwait);
            tk_ena_dsp();
        }
        done += n;
    }
    return (long)done;
}

/* 受ける。読めたバイト数か負のエラー。渡された fd は blobs に（最大 maxb 個、*nb に数）。
 * peek なら読み出さない（MSG_PEEK） */
long rv_usock_recv(int i, uintptr_t ubuf, size_t len, int nonblock, int peek, void *blobs, int maxb, int *nb)
{
    struct usock *u = &us[i];
    size_t n, k;
    uint8_t tmp[512];
    int got = 0;

    *nb = 0;
    if (u->state != US_CONN && u->state != US_CLOSED) {
        return -ENOTCONN;
    }
    for (;;) {
        tk_dis_dsp();
        if (u->count > 0) {
            break;
        }
        if (u->state == US_CLOSED || u->shut_rd || u->peer < 0) {
            tk_ena_dsp();
            return 0;                       /* 相手が閉じた */
        }
        if (nonblock) {
            tk_ena_dsp();
            return -EAGAIN;
        }
        u->rwait = tk_get_tid();
        tk_ena_dsp();
        tk_slp_tsk(50);
        if (rv_linux_sig_interrupt()) {
            return -EINTR;
        }
    }
    /* ここは tk_dis_dsp の中 */
    n = len < u->count ? len : u->count;
    if (n > sizeof(tmp)) {
        n = sizeof(tmp);
    }
    for (k = 0; k < n; k++) {
        tmp[k] = u->buf[(u->tail + k) % UBUF];
    }
    if (!peek) {
        /* 読むバイトと一緒に送られた fd（送ったときの位置が、読み終わる位置より前のもの） */
        uint64_t end = u->out_total + n;
        int m = 0, w = 0;
        for (m = 0; m < u->nq; m++) {
            if (u->q[m].pos < end && got < maxb) {
                uint8_t *dst = (uint8_t *)blobs + (size_t)got * FDBLOB;
                for (k = 0; k < FDBLOB; k++) {
                    dst[k] = u->q[m].blob[k];
                }
                got++;
            } else {
                if (w != m) {
                    u->q[w] = u->q[m];
                }
                w++;
            }
        }
        u->nq = w;
        u->tail = (u->tail + (unsigned)n) % UBUF;
        u->count -= (unsigned)n;
        u->out_total += n;
        if (u->peer >= 0) {
            wake(&us[u->peer].wwait);
        }
    }
    tk_ena_dsp();
    *nb = got;
    if (rv_user_copy_out(ubuf, tmp, n) != 0) {
        return -EFAULT;
    }
    return (long)n;
}

/* poll の印（POLLIN 1・POLLOUT 4・POLLHUP 0x10） */
int rv_usock_events(int i)
{
    struct usock *u = &us[i];
    int ev = 0;
    if (u->state == US_LISTEN) {
        return u->nback > 0 ? 1 : 0;
    }
    if (u->count > 0) {
        ev |= 1;
    }
    if (u->state == US_CLOSED || u->peer < 0 || u->shut_rd) {
        ev |= (u->state == US_CONN || u->state == US_CLOSED) ? 0x10 | 1 : 0;
    } else if (us[u->peer].count < UBUF && !u->shut_wr) {
        ev |= 4;
    }
    if (u->state == US_NEW || u->state == US_BOUND) {
        ev |= 4;
    }
    return ev;
}

/* テスト用: 使っているソケットの数 */
int rv_usock_count(void)
{
    int i, n = 0;
    for (i = 0; i < NUS; i++) {
        n += us[i].used;
    }
    return n;
}
