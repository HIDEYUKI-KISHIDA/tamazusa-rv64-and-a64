/*
 * linux_fs.c — Linux 互換層のファイル（S-mode + MMU のみ）
 *
 *   ディスク（tar、読み取り専用）のファイルを、すべて「/」の直下にあるものとして見せる。
 *   開いたファイルの表（fd）はプログラムごとに 32 個。0・1・2 はコンソール。
 *
 *   openat / close / read / readv / pread64 / write / writev / lseek / fstat / newfstatat /
 *   faccessat / faccessat2 / getdents64 / getcwd / chdir / readlinkat / fcntl / dup / dup3 / ioctl
 *
 *   パス: "/名前"・"名前"・"./名前" はディスクのファイル。"/"・"." はルートのフォルダ。
 *   それ以外のフォルダはない（ENOENT）。書き込みで開こうとすると EROFS。
 *   /dev/fb0 は画面（virtio_gpu.c）。640×400、1 点 4 バイト（B G R 0）。書いた行がそのまま窓に出る。
 *   /dev/mouse は USB のマウス・タブレット（kernel/usb/xhci.c）。読むと今の位置（画面の 640×400 の座標）・
 *   ボタン・変わった回数（int32 を 4 つ、16 バイト）が返る。待たない。
 *   /dev/dsp は音の装置（virtio_snd.c）。22050Hz・16 ビット・モノラルで書くと鳴る。
 *   ただし /tmp はメモリの上の書き込めるフォルダ（linux_tmpfs.c）。作る・書く・消す・名前を変えるができる。
 *
 *   パイプ（pipe2）: 4KiB の輪の形の入れ物。読む側は空なら眠り、書く側は満杯なら眠る。
 *   書く側がすべて閉じれば読む側は 0（終わり）、読む側がすべて閉じれば書く側は EPIPE。
 *   fork で fd の表を子に写し（パイプの参照の数を増やす）、execve では
 *   「exec で閉じる」印（O_CLOEXEC / FD_CLOEXEC）の fd を閉じる。
 */
#include <stdint.h>
#include <stddef.h>
#include <rv_priv.h>
#include "../../../fs/tarfs.h"
#include "../../../fs/ext2.h"


struct rv_uproc;
extern int  rv_uproc_slot(const struct rv_uproc *p);
extern int  rv_user_copy_in(void *dst, uintptr_t uva, size_t len);
extern int  rv_user_copy_out(uintptr_t uva, const void *src, size_t len);
extern size_t rv_user_copy_str(char *dst, uintptr_t uva, size_t max);
extern void rv_console_puts(const char *s);
extern int  rv_console_getc(void);
extern int  rv_console_getc_intr(int (*intr)(void));
extern int  rv_tmp_lookup(const char *name);
extern int  rv_tmp_create(const char *name);
extern void rv_tmp_ref(int i);
extern void rv_tmp_unref(int i);
extern uint64_t rv_tmp_size(int i);
extern const char *rv_tmp_name(int i);
extern long rv_tmp_read(int i, uint64_t off, uintptr_t ubuf, size_t len);
extern long rv_tmp_write(int i, uint64_t off, uintptr_t ubuf, size_t len);
extern int  rv_tmp_truncate(int i, uint64_t len);
extern int  rv_tmp_unlink(const char *name);
extern int  rv_tmp_rename(const char *from, const char *to);
extern int  rv_tmp_next(int index);
extern int  rv_snd_open(void);
extern long rv_snd_write(uintptr_t ubuf, size_t len);
extern void rv_snd_close(void);
extern int  virtio_gpu_present(void);
extern long rv_fb_write(uint64_t off, uintptr_t ubuf, size_t len);
extern long rv_fb_read(uint64_t off, uintptr_t ubuf, size_t len);
extern unsigned long rv_fb_size(void);
extern void rv_fb_geometry(unsigned *w, unsigned *h);
extern volatile int rv_mouse_x, rv_mouse_y, rv_mouse_buttons;
extern volatile unsigned rv_mouse_seq;
extern int rv_usb_ptr_on(void);
static int dsp_refs;                /* /dev/dsp を開いている fd の数（dup・fork も数える） */
extern int  rv_linux_sig_interrupt(void);
extern void rv_linux_signal_pgrp(int pgid, int sig);
extern int  rv_linux_my_pgid(struct rv_uproc *u);
extern void rv_linux_signal_self(int sig);
/* ソケット（linux_net.c） */
extern int  rv_sock_create(long domain, long type, long proto);
extern void rv_sock_ref(int i);
extern void rv_sock_unref(int i);
extern void rv_sock_set_nonblock(int i, int on);
extern int  rv_sock_get_nonblock(int i);
extern int  rv_sock_events(int i);
extern long rv_sock_bind(int i, uintptr_t ua, long len);
extern long rv_sock_listen(int i, long backlog);
extern long rv_sock_accept(int i, uintptr_t ua, uintptr_t ulen, long flags);
extern long rv_sock_connect(int i, uintptr_t ua, long len);
extern long rv_sock_name(int i, uintptr_t ua, uintptr_t ulen, int peer);
extern long rv_sock_send(int i, uintptr_t ubuf, size_t len, long flags, uintptr_t ua, long alen);
extern long rv_sock_recv(int i, uintptr_t ubuf, size_t len, long flags, uintptr_t ua, uintptr_t ulen);
extern long rv_sock_shutdown(int i, long how);
extern long rv_sock_getsockopt(int i, long level, long name, uintptr_t uval, uintptr_t ulen);
extern int  rv_sock_rx_avail(int i);
/* 疑似端末（linux_pty.c）と制御端末（linux_proc.c） */
extern int  rv_pty_open_master(void);
extern int  rv_pty_open_slave(int n);
extern int  rv_pty_exists(int n);
extern void rv_pty_ref(int n, int master);
extern void rv_pty_unref(int n, int master);
extern long rv_pty_read(int n, int master, uintptr_t ubuf, size_t len, int nonblock);
extern long rv_pty_write(int n, int master, uintptr_t ubuf, size_t len);
extern int  rv_pty_events(int n, int master);
extern int  rv_pty_ioctl(int n, int master, unsigned long cmd, uintptr_t arg, long *ret);
extern int  rv_linux_get_ctty(struct rv_uproc *u);
extern void rv_linux_set_ctty(struct rv_uproc *u, int n, int force);
extern int  rv_console_try_getc(void);
extern int  rv_console_has_input(void);
extern int  tk_dly_tsk(unsigned long dlytim);

#define LNX_getcwd      17
#define LNX_dup         23
#define LNX_dup3        24
#define LNX_fcntl       25
#define LNX_ioctl       29
#define LNX_faccessat   48
#define LNX_chdir       49
#define LNX_openat      56
#define LNX_close       57
#define LNX_getdents64  61
#define LNX_lseek       62
#define LNX_read        63
#define LNX_write       64
#define LNX_readv       65
#define LNX_writev      66
#define LNX_pread64     67
#define LNX_readlinkat  78
#define LNX_newfstatat  79
#define LNX_fstat       80
#define LNX_faccessat2  439
#define LNX_pipe2       59

#define ENOENT  2
#define EINTR   4
#define EAGAIN  11
#define EBADF   9
#define EFAULT  14
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOTTY  25
#define ESPIPE  29
#define EROFS   30
#define ENAMETOOLONG 36
#define EPIPE   32
#define O_CLOEXEC   02000000
#define FD_CLOEXEC  1
#define S_IFIFO     0010000
#define PIPE_SIZE   4096
#define NPIPE       16

#define O_ACCMODE   03
#define O_CREAT     0100
#define O_TRUNC     01000
#define O_DIRECTORY 0200000
#define O_EXCL      0200
#define O_APPEND    02000
#define EBUSY       16
#define ENODEV      19
#define EXDEV       18
#define EPERM       1
#ifndef EIO
#define EIO         5
#endif
#ifndef ENOMEM
#define ENOMEM      12
#endif
#ifndef EACCES
#define EACCES      13
#endif
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_NOFOLLOW 0x100
#define LNX_pwrite64    68
#define LNX_unlinkat    35
#define LNX_renameat    38
#define LNX_renameat2   276
#define LNX_ftruncate   46
#define AT_FDCWD    (-100)
#define AT_EMPTY_PATH 0x1000
#define W_OK        2

#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define F_DUPFD_CLOEXEC 1030

#define TIOCGWINSZ  0x5413
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define LNX_ppoll   73
#define LNX_pselect6 72

#define S_IFDIR     0040000
#define S_IFREG     0100000
#define S_IFCHR     0020000
#define S_IFBLK     0060000

#define NFD         32
#define SLOTS       32

enum { FD_FREE = 0, FD_CONSOLE, FD_FILE, FD_DIR, FD_PIPE_R, FD_PIPE_W, FD_NULL, FD_ZERO, FD_RANDOM, FD_SOCK, FD_PTM, FD_PTS, FD_TMP, FD_TMPDIR, FD_DSP, FD_FB, FD_MOUSE, FD_IO, FD_BLK, FD_EXT, FD_EXTDIR,
       FD_USOCK, FD_EPOLL, FD_EVENTFD, FD_TIMERFD, FD_INPUT, FD_IOLOG };

extern int tk_dis_dsp(void);
extern int tk_ena_dsp(void);
extern int tk_slp_tsk(int tmout);
extern int tk_wup_tsk(int tskid);
extern int tk_get_tid(void);

struct lpipe {
    int used;
    uint8_t buf[PIPE_SIZE];
    unsigned head, tail, count;     /* head = 次に書く、tail = 次に読む */
    int readers, writers;           /* 読む側・書く側の fd の数 */
    int rwait, wwait;               /* 眠っている読む側・書く側のタスク（0 = なし） */
};
static struct lpipe pipes[NPIPE];

struct lfd {
    int type;
    int cloexec;
    int nonblock;                   /* O_NONBLOCK（パイプ・端末: 待たずに -EAGAIN） */
    int pipe;                       /* パイプの番号（FD_PIPE_R / FD_PIPE_W）・ソケットの番号（FD_SOCK）・/tmp のファイルの番号（FD_TMP） */
    int acc;                        /* FD_TMP: 開いた形（0 読む、1 書く、2 両方） */
    int append;                     /* FD_TMP: O_APPEND（書くたびに末尾へ） */
    struct tarfs_file f;
    int64_t  ino;               /* ファイルの番号（tar の何番目か + 2。ルートは 1） */
    uint64_t off;               /* ファイル: 読む位置 / フォルダ: 次に返す項目の番号 */
};
static struct lfd fds[SLOTS][NFD];
static uint8_t    fds_ready[SLOTS];
static char cwds[SLOTS][128];               /* いまのディレクトリ（空 = 「/」） */
static int cur_slot = -1;                   /* いまのシステムコールのスロット */
static int cur_dirfd = AT_FDCWD;            /* いまのシステムコールの dirfd（相対パスの基準） */
static struct lfd *cur_tab;
static int upath_norm(uintptr_t upath, char *out, size_t max);
struct lfd;
static long ext_rw(struct lfd *d, uint64_t off, uintptr_t ubuf, size_t len, int wr);
static uint64_t ext_size(struct lfd *d);
static long ext_getdents64(struct lfd *d, uintptr_t ubuf, size_t len);
extern void *rv_kpage_alloc(void);
extern void rv_kpage_free(void *p);
extern void uart_puts(const char *s);

extern void rv_linux_mm_fork(int ps, int cs);
/* Wayland の下回り: UNIX ソケット（linux_unix.c）・eventfd・timerfd・入力のできごと（linux_evfd.c） */
extern int  rv_usock_create(int type, int pid);
extern int  rv_usock_pair(int type, int pid, int *a, int *b);
extern void rv_usock_ref(int i);
extern void rv_usock_unref(int i);
extern long rv_usock_bind(int i, uintptr_t uaddr, long alen);
extern long rv_usock_listen(int i, long backlog);
extern long rv_usock_connect(int i, uintptr_t uaddr, long alen, int pid);
extern long rv_usock_accept(int i, int nonblock, uintptr_t uaddr, uintptr_t ulen);
extern long rv_usock_name(int i, uintptr_t uaddr, uintptr_t ulen, int peer);
extern long rv_usock_peercred(int i, uintptr_t uval, uintptr_t ulen);
extern long rv_usock_shutdown(int i, long how);
extern long rv_usock_send(int i, uintptr_t ubuf, size_t len, int nonblock, const void *blobs, int nb);
extern long rv_usock_recv(int i, uintptr_t ubuf, size_t len, int nonblock, int peek, void *blobs, int maxb, int *nb);
extern int  rv_usock_events(int i);
extern int  rv_eventfd_create(unsigned initval, int flags);
extern void rv_eventfd_ref(int i);
extern void rv_eventfd_unref(int i);
extern long rv_eventfd_read(int i, uintptr_t ubuf, size_t len, int nonblock);
extern long rv_eventfd_write(int i, uintptr_t ubuf, size_t len);
extern int  rv_eventfd_events(int i);
extern int  rv_timerfd_create(int clock);
extern void rv_timerfd_ref(int i);
extern void rv_timerfd_unref(int i);
extern long rv_timerfd_settime(int i, int flags, uintptr_t unew, uintptr_t uold, int clock);
extern long rv_timerfd_gettime(int i, uintptr_t ucur);
extern long rv_timerfd_read(int i, uintptr_t ubuf, size_t len, int nonblock);
extern int  rv_timerfd_events(int i);
extern uint32_t rv_evdev_open(int d);
extern long rv_evdev_read(int d, uint64_t *pos, uintptr_t ubuf, size_t len, int nonblock);
extern int  rv_evdev_events(int d, uint64_t pos);
extern int  rv_evdev_ioctl(int d, unsigned long cmd, uintptr_t arg, long *r);
extern int  rv_uproc_pid(const struct rv_uproc *p);
extern int  rv_usb_kbd_on(void);
extern long iomgr_log_read(uint64_t *seq, char *kbuf, size_t max);
extern void rv_linux_mark_net(struct rv_uproc *u);
#define rv_linux_pid_of(p) rv_uproc_pid(p)
static void bcopy_k(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
}
#define memcpy(d, s, n) bcopy_k((d), (s), (n))

/* epoll: 見張る fd の表（fd の番号で持つ。epoll を作ったプロセスの表の番号） */
#define NEPOLL  16
#define EPOLL_N 64
struct lepoll {
    int used, refs, n;
    struct { int32_t fd; uint32_t events; uint64_t data; } it[EPOLL_N];
};
static struct lepoll eps[NEPOLL];
#define FDBLOB  192                 /* linux_unix.c の FDBLOB と同じ（渡す fd の中身の大きさ） */

extern void rv_linux_mm_release(int s);

static struct lfd *table(struct rv_uproc *p)
{
    int s = rv_uproc_slot(p);
    if (s < 0 || s >= SLOTS) {
        return NULL;
    }
    if (!fds_ready[s]) {
        int i;
        for (i = 0; i < NFD; i++) {
            fds[s][i].type = FD_FREE;
            fds[s][i].cloexec = 0;
        }
        fds[s][0].type = fds[s][1].type = fds[s][2].type = FD_CONSOLE;
        fds_ready[s] = 1;
    }
    return fds[s];
}

static void wake(int *who)
{
    if (*who > 0) {
        int t = *who;
        *who = 0;
        tk_wup_tsk(t);
    }
}

/* fd の参照を 1 つ増やす（dup・fork）。パイプなら端の数を増やす */
static void fd_ref(const struct lfd *d)
{
    if (d->type == FD_DSP) {
        dsp_refs++;
    } else if (d->type == FD_TMP) {
        rv_tmp_ref(d->pipe);
    } else if (d->type == FD_EXT || d->type == FD_EXTDIR) {
        ext2_iref((uint32_t)d->pipe);
    } else if (d->type == FD_PTM || d->type == FD_PTS) {
        rv_pty_ref(d->pipe, d->type == FD_PTM);
    } else if (d->type == FD_SOCK) {
        rv_sock_ref(d->pipe);
    } else if (d->type == FD_USOCK) {
        rv_usock_ref(d->pipe);
    } else if (d->type == FD_EPOLL) {
        eps[d->pipe].refs++;
    } else if (d->type == FD_EVENTFD) {
        rv_eventfd_ref(d->pipe);
    } else if (d->type == FD_TIMERFD) {
        rv_timerfd_ref(d->pipe);
    } else if (d->type == FD_PIPE_R) {
        pipes[d->pipe].readers++;
    } else if (d->type == FD_PIPE_W) {
        pipes[d->pipe].writers++;
    }
}

/* fd を閉じる。パイプなら端の数を減らし、相手を起こす */
static void fd_close(struct lfd *d)
{
    if (d->type == FD_TMP) {
        rv_tmp_unref(d->pipe);
    }
    if (d->type == FD_EXT || d->type == FD_EXTDIR) {
        ext2_iunref((uint32_t)d->pipe);
    }
    if (d->type == FD_DSP && --dsp_refs <= 0) {
        dsp_refs = 0;
        rv_snd_close();             /* 残りを鳴らし終えて止める */
    }
    if (d->type == FD_SOCK) {
        rv_sock_unref(d->pipe);
    }
    if (d->type == FD_USOCK) {
        rv_usock_unref(d->pipe);
    }
    if (d->type == FD_EPOLL && --eps[d->pipe].refs <= 0) {
        eps[d->pipe].used = 0;
    }
    if (d->type == FD_EVENTFD) {
        rv_eventfd_unref(d->pipe);
    }
    if (d->type == FD_TIMERFD) {
        rv_timerfd_unref(d->pipe);
    }
    if (d->type == FD_PTM || d->type == FD_PTS) {
        rv_pty_unref(d->pipe, d->type == FD_PTM);
    }
    if (d->type == FD_PIPE_R || d->type == FD_PIPE_W) {
        struct lpipe *pp = &pipes[d->pipe];
        tk_dis_dsp();
        if (d->type == FD_PIPE_R) {
            pp->readers--;
            wake(&pp->wwait);
        } else {
            pp->writers--;
            wake(&pp->rwait);
        }
        if (pp->readers <= 0 && pp->writers <= 0) {
            pp->used = 0;
        }
        tk_ena_dsp();
    }
    d->type = FD_FREE;
    d->cloexec = 0;
}

/* プログラムの後始末（uspace.c の rv_uproc_free から） */
void rv_linux_fs_release(int slot)
{
    int i;
    if (slot < 0 || slot >= SLOTS) {
        return;
    }
    cwds[slot][0] = 0;
    rv_linux_mm_release(slot);
    if (!fds_ready[slot]) {
        return;
    }
    for (i = 0; i < NFD; i++) {
        if (fds[slot][i].type != FD_FREE) {
            fd_close(&fds[slot][i]);
        }
    }
    fds_ready[slot] = 0;
}

/* fork: 親のスロットの fd の表を子のスロットへ写す */
void rv_linux_fs_fork(int pslot, int cslot)
{
    int i;
    if (pslot < 0 || pslot >= SLOTS || cslot < 0 || cslot >= SLOTS) {
        return;
    }
    for (i = 0; i < (int)sizeof(cwds[0]); i++) {
        cwds[cslot][i] = cwds[pslot][i];            /* いまのディレクトリも引き継ぐ */
    }
    rv_linux_mm_fork(pslot, cslot);                 /* mmap で写した所（linux_mm.c） */
    if (!fds_ready[pslot]) {
        fds_ready[cslot] = 0;           /* 親もまだ既定の表（0・1・2 だけ） */
        return;
    }
    for (i = 0; i < NFD; i++) {
        fds[cslot][i] = fds[pslot][i];
        if (fds[cslot][i].type != FD_FREE) {
            fd_ref(&fds[cslot][i]);
        }
    }
    fds_ready[cslot] = 1;
}

/* execve: 古いスロットの表を新しいスロットへ移し、「exec で閉じる」印の fd を閉じる */
void rv_linux_fs_exec(int oslot, int nslot)
{
    int i;
    if (oslot < 0 || oslot >= SLOTS || nslot < 0 || nslot >= SLOTS) {
        return;
    }
    rv_linux_mm_release(oslot);                     /* 古いプログラムの mmap は消える */
    rv_linux_mm_release(nslot);
    if (oslot != nslot) {
        for (i = 0; i < (int)sizeof(cwds[0]); i++) {
            cwds[nslot][i] = cwds[oslot][i];
        }
        cwds[oslot][0] = 0;
    }
    if (!fds_ready[oslot]) {
        fds_ready[nslot] = 0;
        return;
    }
    for (i = 0; i < NFD; i++) {
        fds[nslot][i] = fds[oslot][i];
        if (fds[nslot][i].type != FD_FREE && fds[nslot][i].cloexec) {
            fd_close(&fds[nslot][i]);
        }
        fds[oslot][i].type = FD_FREE;
    }
    fds_ready[nslot] = 1;
    fds_ready[oslot] = 0;
}

/* テスト用: 開いている fd の数（0・1・2 を含む） */
int rv_linux_fs_open_count(int slot)
{
    int i, n = 0;
    if (slot < 0 || slot >= SLOTS || !fds_ready[slot]) {
        return 0;
    }
    for (i = 0; i < NFD; i++) {
        n += fds[slot][i].type != FD_FREE;
    }
    return n;
}

static struct lfd *get_fd(struct lfd *t, long fd)
{
    if (t == NULL || fd < 0 || fd >= NFD || t[fd].type == FD_FREE) {
        return NULL;
    }
    return &t[fd];
}

static int alloc_fd(struct lfd *t, int from)
{
    int i;
    for (i = from; i < NFD; i++) {
        if (t[i].type == FD_FREE) {
            t[i].nonblock = 0;
            return i;
        }
    }
    return -EMFILE;
}

/* ---- パス ---- */

/* /tmp のパスか。0 = 違う、1 = /tmp そのもの、2 = /tmp の中（name に「/tmp/」を外した名前）、負 = エラー */
static int tmp_path_str(const char *path, char *name, size_t max)
{
    const char *n = path;
    size_t k = 0;

    while (*n == '/') {
        n++;
    }
    while (n[0] == '.' && n[1] == '/') {
        n += 2;
        while (*n == '/') {
            n++;
        }
    }
    if (!(n[0] == 't' && n[1] == 'm' && n[2] == 'p' && (n[3] == 0 || n[3] == '/'))) {
        return 0;
    }
    n += 3;
    while (*n == '/') {
        n++;
    }
    if (*n == 0) {
        return 1;
    }
    while (n[k] && k + 1 < max) {
        name[k] = n[k];
        k++;
    }
    if (n[k]) {
        return -ENAMETOOLONG;
    }
    while (k > 0 && name[k - 1] == '/') {
        k--;
    }
    name[k] = 0;
    return 2;
}

static int tmp_path(uintptr_t upath, char *name, size_t max)
{
    char path[256];
    if (upath_norm(upath, path, sizeof(path)) < 0) {
        return 0;
    }
    return tmp_path_str(path, name, max);
}

/* /tmp のファイルを、stat のために tar のファイルの形にする */
static void tmp_as_file(int i, struct tarfs_file *f)
{
    const char *n = rv_tmp_name(i);
    int k = 0;
    const char *pre = "tmp/";
    while (pre[k]) {
        f->name[k] = pre[k];
        k++;
    }
    while (*n && k < 100) {
        f->name[k++] = *n++;
    }
    f->name[k] = 0;
    f->start = 0;
    f->size = rv_tmp_size(i);
    f->mode = 0644;
}

/* 結果: 0 = ルート、1 = ファイル（f に入る）、負 = エラー */
/* /dev/io: 入出力の管理（kernel/io/iomgr.c）。pread・pwrite で、位置 = ポート番号 × 4 に 32 ビットの値
 * （番号の書き方。仮想の機器のポート）。ioctl で名前の口（/etc/io.conf）を読み書きする */
extern long iomgr_raw(unsigned port, int32_t *v, size_t n, int wr);
extern int rv_usbms_present(void);
extern uint64_t rv_usbms_size(void);
extern uint32_t rv_usbms_block(void);
extern long rv_usbms_rw(uint64_t off, uintptr_t buf, size_t len, int write, int user);
extern long iomgr_ioctl(unsigned long cmd, void *q);

static long vio_rw(uint64_t off, uintptr_t ubuf, size_t len, int wr)
{
    int32_t v[0x80];
    size_t n = len / 4;
    uint64_t port = off / 4;
    long r;

    if ((off & 3) || (len & 3)) {
        return -EINVAL;
    }
    if (port >= 0x80) {
        return 0;
    }
    if (n > 0x80 - port) {
        n = 0x80 - port;
    }
    if (wr && rv_user_copy_in(v, ubuf, n * 4) != 0) {
        return -EFAULT;
    }
    r = iomgr_raw((unsigned)port, v, n, wr);
    if (r < 0 || wr) {
        return r < 0 ? r : (long)(n * 4);
    }
    return rv_user_copy_out(ubuf, v, n * 4) == 0 ? (long)(n * 4) : -EFAULT;
}

/* 装置のファイル（/dev/null など）。名前が合えば fd の種類、なければ 0 */
static int dev_kind(uintptr_t upath)
{
    static const struct { const char *name; int type; } devs[] = {
        { "/dev/null", FD_NULL }, { "/dev/zero", FD_ZERO },
        { "/dev/tty", FD_CONSOLE }, { "/dev/console", FD_CONSOLE },
        { "/dev/urandom", FD_RANDOM }, { "/dev/random", FD_RANDOM },
        { "/dev/dsp", FD_DSP }, { "/dev/fb0", FD_FB }, { "/dev/mouse", FD_MOUSE },
        { "/dev/io", FD_IO }, { "/dev/iolog", FD_IOLOG }, { "/dev/sda", FD_BLK },
        { "/dev/input/event0", FD_INPUT }, { "/dev/input/event1", FD_INPUT + 0x100 },
        { "/dev/stdin", -1 }, { 0, 0 }
    };
    char path[256];
    int i, k;

    if (upath_norm(upath, path, sizeof(path)) < 0) {
        return 0;
    }
    for (i = 0; devs[i].name != 0; i++) {
        for (k = 0; devs[i].name[k] != 0 && devs[i].name[k] == path[k]; k++) {
        }
        if (devs[i].name[k] == 0 && path[k] == 0) {
            return devs[i].type > 0 ? devs[i].type : 0;
        }
    }
    return 0;
}

/* 疑似端末のパス: /dev/ptmx → PTY_MASTER、/dev/pts/N → N、/dev/tty → 制御端末（疑似端末なら N、
 * コンソールなら PTY_CONSOLE）。どれでもなければ PTY_NONE */
#define PTY_NONE    (-100)
#define PTY_MASTER  (-101)
#define PTY_CONSOLE (-102)
#define PTY_NOTTY   (-103)
static int pty_path(uintptr_t upath, struct rv_uproc *p)
{
    char path[32];
    int k, n;
    if (rv_user_copy_str(path, upath, sizeof(path)) == 0) {
        return PTY_NONE;
    }
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/') {
        const char *q = path + 5;
        if (q[0] == 'p' && q[1] == 't' && q[2] == 'm' && q[3] == 'x' && q[4] == 0) {
            return PTY_MASTER;
        }
        if (q[0] == 'p' && q[1] == 't' && q[2] == 's' && q[3] == '/' && q[4] >= '0' && q[4] <= '9') {
            for (n = 0, k = 4; q[k] >= '0' && q[k] <= '9'; k++) {
                n = n * 10 + (q[k] - '0');
            }
            return q[k] == 0 ? n : PTY_NONE;
        }
        if (q[0] == 't' && q[1] == 't' && q[2] == 'y' && q[3] == 0) {
            n = rv_linux_get_ctty(p);
            return n >= 0 ? n : (n == -1 ? PTY_CONSOLE : PTY_NOTTY);
        }
    }
    return PTY_NONE;
}

static uint64_t rnd_state = 0x9E3779B97F4A7C15ULL;

static uint8_t rnd_byte(void)
{
    uint64_t t;
    t = RV_RDTIME();
    rnd_state ^= t + (rnd_state << 6) + (rnd_state >> 2);
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 7;
    rnd_state ^= rnd_state << 17;
    return (uint8_t)(rnd_state >> 24);
}

static long fill_user(uintptr_t ubuf, size_t len, int zero)
{
    uint8_t chunk[256];
    size_t done = 0;
    while (done < len) {
        size_t n = len - done, k;
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        for (k = 0; k < n; k++) {
            chunk[k] = zero ? 0 : rnd_byte();
        }
        if (rv_user_copy_out(ubuf + done, chunk, n) != 0) {
            return done > 0 ? (long)done : -EFAULT;
        }
        done += n;
    }
    return (long)done;
}

/* ディスクで name と同じ名前のファイルを探す（あれば 1、*ino に番号） */
static int find_exact(const char *n, struct tarfs_file *f, int64_t *ino)
{
    int i, r;
    for (i = 0; ; i++) {
        r = tarfs_entry(i, f);
        if (r <= 0) {
            return 0;
        }
        {
            const char *a = f->name, *b = n;
            while (*a && *a == *b) {
                a++;
                b++;
            }
            if (*a == 0 && *b == 0) {
                *ino = i + 2;
                return 1;
            }
        }
    }
}

/*
 * コマンドの一覧（ディスクの「X.links」、1 行 1 つのパス）。名前が載っていれば X.elf が
 * そのコマンドを持つ（busybox.links → busybox.elf、dropbear.links → dropbear.elf）。
 * 初めて使うときに読んでおく。合えば X.elf の名前を elf に入れて 1
 */
#define LINKS_MAX 4
static char links_buf[LINKS_MAX][8192];
static char links_elf[LINKS_MAX][64];
static int  links_n = -1;

static void load_links(void)
{
    int i;
    links_n = 0;
    for (i = 0; links_n < LINKS_MAX; i++) {
        struct tarfs_file lf;
        int r = tarfs_entry(i, &lf), k;
        int64_t n;
        if (r <= 0) {
            break;
        }
        for (k = 0; lf.name[k]; k++) {
        }
        if (k < 7 || k > 56 || lf.name[k - 6] != '.' || lf.name[k - 5] != 'l' || lf.name[k - 4] != 'i'
            || lf.name[k - 3] != 'n' || lf.name[k - 2] != 'k' || lf.name[k - 1] != 's'
            || lf.size >= sizeof(links_buf[0])) {
            continue;
        }
        n = tarfs_read_all(&lf, links_buf[links_n], sizeof(links_buf[0]) - 1);
        if (n <= 0) {
            continue;
        }
        links_buf[links_n][n] = 0;
        {
            int m;
            for (m = 0; m < k - 6; m++) {
                links_elf[links_n][m] = lf.name[m];
            }
            for (m = 0; m < 5; m++) {
                links_elf[links_n][k - 6 + m] = ".elf"[m];
            }
        }
        links_n++;
    }
}

static const char *links_lookup(const char *name)
{
    int b;
    if (links_n < 0) {
        load_links();
    }
    for (b = 0; b < links_n; b++) {
        const char *p, *e;
        for (p = links_buf[b]; *p; p = (*e) ? e + 1 : e) {
            const char *base = p, *a, *c;
            for (e = p; *e && *e != '\n'; e++) {
                if (*e == '/') {
                    base = e + 1;
                }
            }
            for (a = base, c = name; a < e && *c && *a == *c; a++, c++) {
            }
            if (a == e && *c == 0) {
                return links_elf[b];
            }
        }
    }
    return NULL;
}

/*
 * パスをディスクのファイルにする。ファイルなら 1、ルート（フォルダ）なら 0、ないなら -ENOENT。
 *   ディスクはフォルダのない tar なので、先頭の「/」「./」を外した名前で探す。
 *   プログラムの置き場（/bin /usr/bin /sbin /usr/sbin /usr/local/bin）の名前は、
 *     そのまま → X.links に載っていれば X.elf（busybox・dropbear）→ 名前.elf
 *   の順に探す（"bash" → bash.elf、"/bin/ls" → busybox.elf）。
 */
int rv_linux_resolve(const char *path, struct tarfs_file *f, int64_t *ino)
{
    static const char *const bindirs[] = { "usr/local/bin/", "usr/bin/", "usr/sbin/", "bin/", "sbin/", 0 };
    const char *n = path;
    char name[112];
    int i, k, progdir = 1;

    while (*n == '/') {
        n++;
    }
    while (n[0] == '.' && n[1] == '/') {
        n += 2;
        while (*n == '/') {
            n++;
        }
    }
    if (*n == 0 || (n[0] == '.' && n[1] == 0)) {
        *ino = 1;
        f->name[0] = 0;                 /* ルートのフォルダ */
        return 0;
    }
    for (i = 0; bindirs[i] != 0; i++) {
        for (k = 0; bindirs[i][k] && bindirs[i][k] == n[k]; k++) {
        }
        if (bindirs[i][k] == 0) {
            n += k;
            break;
        }
    }
    for (k = 0; n[k]; k++) {
        if (n[k] == '/') {
            progdir = 0;                /* ほかのフォルダの中: そのままの名前でだけ探す */
        }
    }
    if (find_exact(n, f, ino)) {
        return 1;
    }
    {
        /* フォルダ: 「名前/」で始まるファイルがあれば（tar はフォルダの項目を持たなくてよい） */
        int j, len = k;
        while (len > 0 && n[len - 1] == '/') {
            len--;
        }
        for (j = 0; len > 0 && len < 100; j++) {
            struct tarfs_file e;
            int r = tarfs_entry(j, &e), m;
            if (r <= 0) {
                break;
            }
            for (m = 0; m < len && e.name[m] == n[m]; m++) {
            }
            if (m == len && e.name[len] == '/') {
                for (m = 0; m < len; m++) {
                    f->name[m] = n[m];
                }
                f->name[len] = 0;
                *ino = 100000 + j;
                return 0;
            }
        }
    }
    if (!progdir || k + 5 > (int)sizeof(name)) {
        return -ENOENT;
    }
    /* busybox のコマンドを先に（ディスクの sh.elf・cat.elf は古い呼び出し方のプログラムなので） */
    {
        const char *elf = links_lookup(n);
        if (elf != NULL && find_exact(elf, f, ino)) {
            return 1;
        }
    }
    for (i = 0; i < k; i++) {
        name[i] = n[i];
    }
    name[k] = '.'; name[k + 1] = 'e'; name[k + 2] = 'l'; name[k + 3] = 'f'; name[k + 4] = 0;
    if (find_exact(name, f, ino)) {
        return 1;
    }
    return -ENOENT;
}

static int lookup(uintptr_t upath, struct tarfs_file *f, int64_t *ino)
{
    char path[256];
    int len = upath_norm(upath, path, sizeof(path));

    if (len < 0) {
        return len;
    }
    if (len >= 127) {
        return -ENAMETOOLONG;
    }
    return rv_linux_resolve(path, f, ino);
}

static int ends_with_elf(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n > 4 && s[n - 4] == '.' && s[n - 3] == 'e' && s[n - 2] == 'l' && s[n - 1] == 'f';
}

/* struct stat（RISC-V の Linux、128 バイト） */
struct lnx_stat {
    uint64_t st_dev, st_ino;
    uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev, pad1;
    int64_t  st_size;
    int32_t  st_blksize, pad2;
    int64_t  st_blocks;
    int64_t  st_atime; uint64_t st_atime_nsec;
    int64_t  st_mtime; uint64_t st_mtime_nsec;
    int64_t  st_ctime; uint64_t st_ctime_nsec;
    uint32_t unused[2];
};

static long fill_stat(uintptr_t ust, int type, const struct tarfs_file *f, int64_t ino)
{
    struct lnx_stat st;
    unsigned k;
    for (k = 0; k < sizeof(st); k++) {
        ((uint8_t *)&st)[k] = 0;
    }
    st.st_dev = 1;
    st.st_ino = (uint64_t)ino;
    st.st_nlink = 1;
    st.st_blksize = 4096;
    if (type == FD_DIR) {
        st.st_mode = S_IFDIR | 0755;
        st.st_nlink = 2;
    } else if (type == FD_PIPE_R || type == FD_PIPE_W) {
        st.st_mode = S_IFIFO | 0600;
        st.st_dev = 0;
    } else if (type == FD_CONSOLE) {
        st.st_mode = S_IFCHR | 0620;
        st.st_rdev = (5UL << 8) | 1;            /* /dev/console 相当 */
        st.st_dev = 0;
    } else if (type == FD_PTM || type == FD_PTS) {
        st.st_mode = S_IFCHR | 0620;
        st.st_rdev = (type == FD_PTM) ? ((5UL << 8) | 2) : ((136UL << 8) | (uint64_t)ino);
        st.st_dev = 0;
    } else if (type == FD_SOCK || type == FD_USOCK) {
        st.st_mode = 0140000 | 0777;            /* S_IFSOCK */
        st.st_dev = 0;
    } else if (type == FD_EPOLL || type == FD_EVENTFD || type == FD_TIMERFD) {
        st.st_mode = 0600;                      /* 名前のない inode（anon_inode） */
        st.st_dev = 0;
    } else if (type == FD_INPUT) {
        st.st_mode = S_IFCHR | 0660;
        st.st_rdev = (13UL << 8) | (uint64_t)(64 + ino);    /* /dev/input/eventN */
        st.st_dev = 0;
    } else if (type == FD_IO) {
        st.st_mode = S_IFCHR | 0664;            /* 読むのは誰でも、動かすのは root と io のグループ（安全の決まり 6） */
        st.st_gid = 20;
        st.st_rdev = (10UL << 8) | 240;         /* 仮の番号（misc） */
        st.st_dev = 0;
    } else if (type == FD_IOLOG) {
        st.st_mode = S_IFCHR | 0444;
        st.st_rdev = (10UL << 8) | 241;
        st.st_dev = 0;
    } else if (type == FD_MOUSE) {
        st.st_mode = S_IFCHR | 0444;
        st.st_rdev = (13UL << 8) | 63;          /* /dev/input/mice と同じ番号 */
        st.st_dev = 0;
    } else if (type == FD_BLK) {
        st.st_mode = S_IFBLK | 0660;
        st.st_rdev = (8UL << 8) | 0;            /* /dev/sda */
        st.st_size = (int64_t)rv_usbms_size();
        st.st_blksize = (int32_t)rv_usbms_block();
        st.st_dev = 0;
    } else if (type == FD_FB) {
        st.st_mode = S_IFCHR | 0666;
        st.st_rdev = (29UL << 8) | 0;           /* /dev/fb0 */
        st.st_size = (int64_t)rv_fb_size();
        st.st_dev = 0;
    } else if (type == FD_DSP) {
        st.st_mode = S_IFCHR | 0666;
        st.st_rdev = (14UL << 8) | 3;           /* /dev/dsp（OSS） */
        st.st_dev = 0;
    } else if (type == FD_NULL || type == FD_ZERO || type == FD_RANDOM) {
        st.st_mode = S_IFCHR | 0666;
        st.st_rdev = (1UL << 8) | (type == FD_NULL ? 3 : type == FD_ZERO ? 5 : 9);
        st.st_dev = 0;
    } else {
        st.st_mode = S_IFREG | ((ends_with_elf(f->name) || (f->mode & 0111)) ? 0755 : 0644);
        st.st_size = (int64_t)f->size;
        st.st_blocks = (int64_t)((f->size + 511) / 512);
    }
    return rv_user_copy_out(ust, &st, sizeof(st)) == 0 ? 0 : -EFAULT;
}

/* ---- 入出力 ---- */

static long console_write(uintptr_t ubuf, size_t len)
{
    char chunk[129];
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > 128) {
            n = 128;
        }
        if (rv_user_copy_in(chunk, ubuf + done, n) != 0) {
            return done > 0 ? (long)done : -EFAULT;
        }
        chunk[n] = 0;
        rv_console_puts(chunk);
        done += n;
    }
    return (long)done;
}

/*
 * 端末の設定（termios、RISC-V の Linux のカーネルの形 36 バイト）。コンソールは 1 つなので 1 組。
 *   ICANON あり（既定）: 1 行ずつ。カーネルがエコーし、Backspace で消せる。行頭の Ctrl-D で終わり（0）
 *   ICANON なし（raw）: 1 文字以上届いたら、そのとき届いている分を返す。エコーしない
 *     （busybox の ash は、入力行を自分で編集するときこちらにする）
 */
#define T_ICRNL   0x100
#define T_ICANON  0x002
#define T_ECHO    0x008
#define T_VERASE  2
#define T_VEOF    4
struct lnx_termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t  line;
    uint8_t  cc[19];
};
static struct lnx_termios tio = {
    0x100 | 0x400,                  /* ICRNL | IXON */
    0x001 | 0x004,                  /* OPOST | ONLCR */
    0x0bf,                          /* B38400 | CS8 | CREAD */
    0x001 | 0x002 | 0x008 | 0x010 | 0x020 | 0x8000,   /* ISIG ICANON ECHO ECHOE ECHOK IEXTEN */
    0,
    { 3, 0x1c, 0x7f, 0x15, 4, 0, 1, 0, 0x11, 0x13, 0x1a, 0, 0x12, 0x0f, 0x17, 0x16, 0, 0, 0 }
};

#define T_ISIG    0x001
#define T_VINTR   0
#define T_VQUIT   1
#define T_VSUSP   10

/* 端末の前面のプロセスグループ（Ctrl-C を送る相手）。0 = なし */
static volatile int fg_pgrp;

void rv_tty_set_foreground(int pgid)
{
    fg_pgrp = pgid;
}

/* 端末の特別な文字（ISIG のとき）: Ctrl-C → SIGINT、Ctrl-\ → SIGQUIT、Ctrl-Z → SIGTSTP。
 * シグナルにしたら 1（その文字は入力にしない）。割込み処理の中からも呼ばれる */
static int tty_isig(int c)
{
    int sig = 0;

    if (!(tio.lflag & T_ISIG) || c == 0) {
        return 0;
    }
    if (c == tio.cc[T_VINTR]) {
        sig = 2;
    } else if (c == tio.cc[T_VQUIT]) {
        sig = 3;
    } else if (c == tio.cc[T_VSUSP]) {
        sig = 20;
    } else {
        return 0;
    }
    if (tio.lflag & T_ECHO) {
        rv_console_puts(sig == 2 ? "^C" : sig == 3 ? "^\\" : "^Z");
    }
    if (fg_pgrp > 0) {
        rv_linux_signal_pgrp(fg_pgrp, sig);
    }
    return 1;
}

int rv_tty_rx_hook(int c)
{
    return tty_isig(c);
}

static void echo_str(const char *s)
{
    if (tio.lflag & T_ECHO) {
        rv_console_puts(s);
    }
}

static int map_in(int ch)
{
    return (ch == '\r' && (tio.iflag & T_ICRNL)) ? '\n' : ch;
}

static long console_read(uintptr_t ubuf, size_t len)
{
    char line[256];
    size_t n = 0;

    if (len == 0) {
        return 0;
    }
    if (!(tio.lflag & T_ICANON)) {
        /* raw: 1 文字目は届くまで待ち、あとは届いている分だけ */
        int ch;
        do {
            ch = rv_console_getc_intr(rv_linux_sig_interrupt);
            if (ch < 0) {
                return -EINTR;
            }
        } while (tty_isig(ch));         /* テスト用の入力の Ctrl-C など */
        ch = map_in(ch);
        line[n++] = (char)ch;
        while (n < len && n < sizeof(line)) {
            ch = rv_console_try_getc();
            if (ch < 0) {
                break;
            }
            line[n++] = (char)map_in(ch);
            if (ch == 4) {
                break;
            }
        }
        return rv_user_copy_out(ubuf, line, n) == 0 ? (long)n : -EFAULT;
    }
    /* 1 行ずつ */
    for (;;) {
        int ch = rv_console_getc_intr(rv_linux_sig_interrupt);
        if (ch < 0) {
            return -EINTR;              /* シグナル（書きかけの行は捨てる） */
        }
        if (tty_isig(ch)) {
            n = 0;
            continue;
        }
        ch = map_in(ch);
        if (ch == tio.cc[T_VEOF]) {             /* Ctrl-D */
            if (n == 0) {
                return 0;
            }
            break;
        }
        if (ch == tio.cc[T_VERASE] || ch == 0x08) {
            if (n > 0) {
                n--;
                echo_str("\b \b");
            }
            continue;
        }
        if (n < sizeof(line) - 1) {
            char e[2] = { (char)ch, 0 };
            line[n++] = (char)ch;
            echo_str(ch == '\n' ? "\n" : e);
        }
        if (ch == '\n' || n >= len) {
            break;
        }
    }
    if (n > len) {
        n = len;
    }
    return rv_user_copy_out(ubuf, line, n) == 0 ? (long)n : -EFAULT;
}

static long file_read_at(struct lfd *d, uint64_t off, uintptr_t ubuf, size_t len)
{
    uint8_t chunk[256];
    size_t done = 0;
    while (done < len) {
        size_t want = len - done;
        int64_t n;
        if (want > sizeof(chunk)) {
            want = sizeof(chunk);
        }
        n = tarfs_read(&d->f, off + done, chunk, want);
        if (n < 0) {
            return done > 0 ? (long)done : -5;  /* EIO */
        }
        if (n == 0) {
            break;
        }
        if (rv_user_copy_out(ubuf + done, chunk, (size_t)n) != 0) {
            return done > 0 ? (long)done : -EFAULT;
        }
        done += (size_t)n;
        if ((size_t)n < want) {
            break;
        }
    }
    return (long)done;
}

static long pipe_read(struct lpipe *pp, uintptr_t ubuf, size_t len, int nonblock)
{
    uint8_t chunk[256];
    if (len == 0) {
        return 0;
    }
    for (;;) {
        size_t n = 0;
        tk_dis_dsp();
        while (n < len && n < sizeof(chunk) && pp->count > 0) {
            chunk[n++] = pp->buf[pp->tail];
            pp->tail = (pp->tail + 1) % PIPE_SIZE;
            pp->count--;
        }
        if (n > 0) {
            wake(&pp->wwait);
            tk_ena_dsp();
            return rv_user_copy_out(ubuf, chunk, n) == 0 ? (long)n : -EFAULT;
        }
        if (pp->writers <= 0) {
            tk_ena_dsp();
            return 0;                           /* 書く側がいない = 終わり */
        }
        if (nonblock) {
            tk_ena_dsp();
            return -EAGAIN;
        }
        pp->rwait = tk_get_tid();
        tk_ena_dsp();
        tk_slp_tsk(-1);                         /* 書かれるか、書く側が閉じると起こされる */
        if (rv_linux_sig_interrupt()) {
            return -EINTR;
        }
    }
}

static long pipe_write(struct lpipe *pp, uintptr_t ubuf, size_t len, int nonblock)
{
    size_t done = 0;
    while (done < len) {
        uint8_t chunk[256];
        size_t want = len - done, n = 0;
        if (want > sizeof(chunk)) {
            want = sizeof(chunk);
        }
        if (rv_user_copy_in(chunk, ubuf + done, want) != 0) {
            return done > 0 ? (long)done : -EFAULT;
        }
        while (n < want) {
            tk_dis_dsp();
            if (pp->readers <= 0) {
                tk_ena_dsp();
                return done + n > 0 ? (long)(done + n) : -EPIPE;
            }
            while (n < want && pp->count < PIPE_SIZE) {
                pp->buf[pp->head] = chunk[n++];
                pp->head = (pp->head + 1) % PIPE_SIZE;
                pp->count++;
            }
            wake(&pp->rwait);
            if (n < want && nonblock) {
                tk_ena_dsp();
                return done + n > 0 ? (long)(done + n) : -EAGAIN;
            }
            if (n < want) {
                pp->wwait = tk_get_tid();       /* 満杯: 読まれるまで眠る */
                tk_ena_dsp();
                tk_slp_tsk(-1);
                if (rv_linux_sig_interrupt()) {
                    return done + n > 0 ? (long)(done + n) : -EINTR;
                }
            } else {
                tk_ena_dsp();
            }
        }
        done += want;
    }
    return (long)done;
}

static long do_read(struct lfd *d, uintptr_t ubuf, size_t len)
{
    long r;
    if (d->type == FD_USOCK) {
        char blobs[1];
        int nb;
        return rv_usock_recv(d->pipe, ubuf, len, d->nonblock, 0, blobs, 0, &nb);
    }
    if (d->type == FD_EVENTFD) {
        return rv_eventfd_read(d->pipe, ubuf, len, d->nonblock);
    }
    if (d->type == FD_TIMERFD) {
        return rv_timerfd_read(d->pipe, ubuf, len, d->nonblock);
    }
    if (d->type == FD_INPUT) {
        return rv_evdev_read(d->pipe, &d->off, ubuf, len, d->nonblock);
    }
    if (d->type == FD_IOLOG) {
        static char lb[2048];
        long n = iomgr_log_read(&d->off, lb, len < sizeof(lb) ? len : sizeof(lb));
        return n > 0 && rv_user_copy_out(ubuf, lb, (size_t)n) != 0 ? -EFAULT : n;
    }
    if (d->type == FD_EPOLL) {
        return -EINVAL;
    }
    if (d->type == FD_CONSOLE) {
        if (d->nonblock && !rv_console_has_input()) {
            return -EAGAIN;
        }
        return console_read(ubuf, len);
    }
    if (d->type == FD_PIPE_R) {
        return pipe_read(&pipes[d->pipe], ubuf, len, d->nonblock);
    }
    if (d->type == FD_PIPE_W) {
        return -EBADF;
    }
    if (d->type == FD_DIR) {
        return -EISDIR;
    }
    if (d->type == FD_NULL) {
        return 0;
    }
    if (d->type == FD_SOCK) {
        return rv_sock_recv(d->pipe, ubuf, len, 0, 0, 0);
    }
    if (d->type == FD_PTM || d->type == FD_PTS) {
        return rv_pty_read(d->pipe, d->type == FD_PTM, ubuf, len, d->nonblock);
    }
    if (d->type == FD_ZERO || d->type == FD_RANDOM) {
        return fill_user(ubuf, len, d->type == FD_ZERO);
    }
    if (d->type == FD_TMPDIR) {
        return -EISDIR;
    }
    if (d->type == FD_DSP) {
        return -EINVAL;             /* 録音はない */
    }
    if (d->type == FD_IO) {
        long r = vio_rw(d->off, ubuf, len, 0);
        if (r > 0) {
            d->off += r;
        }
        return r;
    }
    if (d->type == FD_MOUSE) {
        int32_t m[4];
        if (len < sizeof(m)) {
            return -EINVAL;
        }
        m[0] = rv_mouse_x;
        m[1] = rv_mouse_y;
        m[2] = rv_mouse_buttons;
        m[3] = (int32_t)rv_mouse_seq;
        return rv_user_copy_out(ubuf, m, sizeof(m)) == 0 ? (long)sizeof(m) : -EFAULT;
    }
    if (d->type == FD_EXT || d->type == FD_EXTDIR) {
        long r = ext_rw(d, d->off, ubuf, len, 0);
        if (r > 0) {
            d->off += (uint64_t)r;
        }
        return r;
    }
    if (d->type == FD_BLK) {
        long r = rv_usbms_rw(d->off, ubuf, len, 0, 1);
        if (r > 0) {
            d->off += (uint64_t)r;
        }
        return r;
    }
    if (d->type == FD_FB) {
        long r = rv_fb_read(d->off, ubuf, len);
        if (r > 0) {
            d->off += (uint64_t)r;
        }
        return r;
    }
    if (d->type == FD_TMP) {
        if (d->acc == 1) {
            return -EBADF;
        }
        r = rv_tmp_read(d->pipe, d->off, ubuf, len);
        if (r > 0) {
            d->off += (uint64_t)r;
        }
        return r;
    }
    r = file_read_at(d, d->off, ubuf, len);
    if (r > 0) {
        d->off += (uint64_t)r;
    }
    return r;
}

static long do_write(struct lfd *d, uintptr_t ubuf, size_t len)
{
    if (d->type == FD_USOCK) {
        return rv_usock_send(d->pipe, ubuf, len, d->nonblock, NULL, 0);
    }
    if (d->type == FD_EVENTFD) {
        return rv_eventfd_write(d->pipe, ubuf, len);
    }
    if (d->type == FD_TIMERFD || d->type == FD_EPOLL || d->type == FD_INPUT) {
        return -EINVAL;
    }
    if (d->type == FD_IO) {
        long r = vio_rw(d->off, ubuf, len, 1);
        if (r > 0) {
            d->off += r;
        }
        return r;
    }
    if (d->type == FD_CONSOLE) {
        return console_write(ubuf, len);
    }
    if (d->type == FD_PIPE_W) {
        long r = pipe_write(&pipes[d->pipe], ubuf, len, d->nonblock);
        if (r == -EPIPE) {
            rv_linux_signal_self(13);           /* 読む側がいない: SIGPIPE（既定で終了） */
        }
        return r;
    }
    if (d->type == FD_NULL || d->type == FD_ZERO || d->type == FD_RANDOM) {
        return (long)len;                       /* 捨てる */
    }
    if (d->type == FD_SOCK) {
        return rv_sock_send(d->pipe, ubuf, len, 0, 0, 0);
    }
    if (d->type == FD_PTM || d->type == FD_PTS) {
        return rv_pty_write(d->pipe, d->type == FD_PTM, ubuf, len);
    }
    if (d->type == FD_DSP) {
        return rv_snd_write(ubuf, len);
    }
    if (d->type == FD_EXT || d->type == FD_EXTDIR) {
        long r;
        if (d->append) {
            d->off = ext_size(d);
        }
        r = ext_rw(d, d->off, ubuf, len, 1);
        if (r > 0) {
            d->off += (uint64_t)r;
        }
        return r;
    }
    if (d->type == FD_BLK) {
        long r = rv_usbms_rw(d->off, ubuf, len, 1, 1);
        if (r > 0) {
            d->off += (uint64_t)r;
        }
        return r;
    }
    if (d->type == FD_FB) {
        long r = rv_fb_write(d->off, ubuf, len);
        if (r > 0) {
            d->off += (uint64_t)r;
        }
        return r;
    }
    if (d->type == FD_TMP) {
        long r;
        if (d->acc == 0) {
            return -EBADF;
        }
        if (d->append) {
            d->off = rv_tmp_size(d->pipe);
        }
        r = rv_tmp_write(d->pipe, d->off, ubuf, len);
        if (r > 0) {
            d->off += (uint64_t)r;
        }
        return r;
    }
    return -EBADF;                              /* ディスクのファイルは読み取り専用 */
}

struct lnx_iovec { uint64_t base; uint64_t len; };

static long do_iov(struct lfd *d, uintptr_t uiov, long cnt, int is_write)
{
    long total = 0, i;
    if (cnt < 0 || cnt > 1024) {
        return -EINVAL;
    }
    for (i = 0; i < cnt; i++) {
        struct lnx_iovec v;
        long r;
        if (rv_user_copy_in(&v, uiov + (uintptr_t)i * sizeof(v), sizeof(v)) != 0) {
            return total > 0 ? total : -EFAULT;
        }
        if (v.len == 0) {
            continue;
        }
        r = is_write ? do_write(d, (uintptr_t)v.base, (size_t)v.len)
                     : do_read(d, (uintptr_t)v.base, (size_t)v.len);
        if (r < 0) {
            return total > 0 ? total : r;
        }
        total += r;
        if ((uint64_t)r < v.len) {
            break;                              /* 読み込みが短い（ファイルの終わり・1 行） */
        }
    }
    return total;
}

/* struct linux_dirent64 を 1 つ書く。書けたら大きさ、入らなければ 0 */
static size_t put_dirent(uintptr_t ubuf, size_t room, uint64_t ino, int64_t next, uint8_t type,
                         const char *name)
{
    uint8_t rec[19 + 104];
    size_t nl = 0, reclen, k;
    while (name[nl]) {
        nl++;
    }
    reclen = (19 + nl + 1 + 7) & ~(size_t)7;
    if (reclen > room || reclen > sizeof(rec)) {
        return 0;
    }
    for (k = 0; k < reclen; k++) {
        rec[k] = 0;
    }
    for (k = 0; k < 8; k++) {
        rec[k] = (uint8_t)(ino >> (8 * k));
        rec[8 + k] = (uint8_t)((uint64_t)next >> (8 * k));
    }
    rec[16] = (uint8_t)reclen;
    rec[17] = (uint8_t)(reclen >> 8);
    rec[18] = type;
    for (k = 0; k < nl; k++) {
        rec[19 + k] = (uint8_t)name[k];
    }
    return rv_user_copy_out(ubuf, rec, reclen) == 0 ? reclen : 0;
}

/* フォルダ prefix（"" = ルート）の中で、tar の index 番目の項目が直接の子なら、その名前を
 * name に入れて種類（8 = ファイル、4 = フォルダ）を返す。子でない・すでに出したフォルダなら 0 */
static int child_of(const char *prefix, int index, const char *full, char *name)
{
    int pl = 0, k, j;
    const char *rest;
    while (prefix[pl]) {
        pl++;
    }
    if (pl > 0) {
        for (k = 0; k < pl && full[k] == prefix[k]; k++) {
        }
        if (k < pl || full[pl] != '/') {
            return 0;
        }
        rest = full + pl + 1;
    } else {
        rest = full;
    }
    for (k = 0; rest[k] && rest[k] != '/' && k < 99; k++) {
        name[k] = rest[k];
    }
    name[k] = 0;
    if (k == 0) {
        return 0;
    }
    if (rest[k] != '/') {
        return 8;
    }
    /* 中のフォルダ: 同じフォルダをすでに出していないか（前の項目を見る） */
    for (j = 0; j < index; j++) {
        struct tarfs_file e;
        char other[100];
        if (tarfs_entry(j, &e) <= 0) {
            break;
        }
        if (child_of(prefix, 0, e.name, other) == 4) {
            int m;
            for (m = 0; other[m] && other[m] == name[m]; m++) {
            }
            if (other[m] == 0 && name[m] == 0) {
                return 0;
            }
        }
    }
    return 4;
}

#define TMP_LISTED  (1ULL << 40)     /* ルートの一覧で「tmp」を出し終えた印 */

static long tmpdir_getdents64(struct lfd *d, uintptr_t ubuf, size_t len)
{
    size_t used = 0;
    for (;;) {
        size_t w;
        uint64_t pos = d->off;                  /* 0 = "."、1 = ".."、2 以降 = /tmp のファイルの番号 + 2 */
        if (pos == 0) {
            w = put_dirent(ubuf + used, len - used, 3, 1, 4, ".");
        } else if (pos == 1) {
            w = put_dirent(ubuf + used, len - used, 1, 2, 4, "..");
        } else {
            int i = rv_tmp_next((int)(pos - 2));
            if (i < 0) {
                break;
            }
            d->off = (uint64_t)i + 2;
            w = put_dirent(ubuf + used, len - used, 200000 + (uint64_t)i, (int64_t)i + 3, 8, rv_tmp_name(i));
        }
        if (w == 0) {
            if (used == 0) {
                return -EINVAL;
            }
            break;
        }
        used += w;
        d->off++;
    }
    return (long)used;
}

static long do_getdents64(struct lfd *d, uintptr_t ubuf, size_t len)
{
    size_t used = 0;
    if (d->type == FD_TMPDIR) {
        return tmpdir_getdents64(d, ubuf, len);
    }
    if (d->type == FD_EXTDIR) {
        return ext_getdents64(d, ubuf, len);
    }
    if (d->type != FD_DIR) {
        return -ENOTDIR;
    }
    for (;;) {
        struct tarfs_file f;
        size_t w;
        uint64_t pos = d->off;                  /* 0 = "."、1 = ".."、2 以降 = tar の項目 */
        if (pos == 0) {
            w = put_dirent(ubuf + used, len - used, 1, 1, 4 /* DT_DIR */, ".");
        } else if (pos == 1) {
            w = put_dirent(ubuf + used, len - used, 1, 2, 4, "..");
        } else if (pos == TMP_LISTED) {
            if (d->f.name[0] != 0 || !ext2_mounted()) {
                break;
            }
            w = put_dirent(ubuf + used, len - used, 4, (int64_t)TMP_LISTED + 1, 4, "data");   /* ルート: /data も */
            if (w == 0) {
                if (used == 0) {
                    return -EINVAL;
                }
                break;
            }
            used += w;
            d->off = TMP_LISTED + 1;
            continue;
        } else if (pos == TMP_LISTED + 1) {
            break;
        } else {
            char name[100];
            int kind;
            int r = tarfs_entry((int)(pos - 2), &f);
            if (r < 0) {
                return used > 0 ? (long)used : -5;
            }
            if (r == 0) {
                if (d->f.name[0] == 0) {        /* ルート: 最後に /tmp を見せる */
                    w = put_dirent(ubuf + used, len - used, 3, (int64_t)TMP_LISTED, 4, "tmp");
                    if (w == 0) {
                        if (used == 0) {
                            return -EINVAL;
                        }
                        break;
                    }
                    used += w;
                    d->off = TMP_LISTED;
                    continue;
                }
                break;                          /* 終わり */
            }
            kind = child_of(d->f.name, (int)(pos - 2), f.name, name);
            if (kind == 0) {
                d->off++;                       /* このフォルダの子ではない */
                continue;
            }
            w = put_dirent(ubuf + used, len - used, kind == 4 ? 100000 + (int64_t)pos : (int64_t)pos,
                           (int64_t)pos + 1, (uint8_t)kind, name);
        }
        if (w == 0) {
            if (used == 0) {
                return -EINVAL;                 /* バッファが小さすぎる */
            }
            break;
        }
        used += w;
        d->off++;
    }
    return (long)used;
}

/* ---- システムコール ---- */

/* 扱ったら 1（*ret に戻り値）、扱わない番号なら 0 */
/* fd の今の状態（poll の形: POLLIN 1, POLLOUT 4, POLLERR 8, POLLHUP 0x10, POLLNVAL 0x20） */
static int16_t fd_events(struct lfd *x)
{
    int16_t ev = 0;
    if (x == NULL) {
        return 0x20;
    }
    if (x->type == FD_CONSOLE) {
        if (rv_console_has_input()) ev |= 0x1;
        ev |= 0x4;
    } else if (x->type == FD_SOCK) {
        ev = (int16_t)rv_sock_events(x->pipe);
    } else if (x->type == FD_USOCK) {
        ev = (int16_t)rv_usock_events(x->pipe);
    } else if (x->type == FD_EVENTFD) {
        ev = (int16_t)rv_eventfd_events(x->pipe);
    } else if (x->type == FD_TIMERFD) {
        ev = (int16_t)rv_timerfd_events(x->pipe);
    } else if (x->type == FD_INPUT) {
        ev = (int16_t)rv_evdev_events(x->pipe, x->off);
    } else if (x->type == FD_EPOLL) {
        ev = 0;                                 /* epoll の中の epoll は、まだ */
    } else if (x->type == FD_PTM || x->type == FD_PTS) {
        ev = (int16_t)rv_pty_events(x->pipe, x->type == FD_PTM);
    } else if (x->type == FD_PIPE_R) {
        if (pipes[x->pipe].count > 0) ev |= 0x1;
        if (pipes[x->pipe].writers <= 0) ev |= 0x10;
    } else if (x->type == FD_PIPE_W) {
        if (pipes[x->pipe].count < PIPE_SIZE) ev |= 0x4;
        if (pipes[x->pipe].readers <= 0) ev |= 0x8;
    } else {
        ev |= 0x1 | 0x4;
    }
    return ev;
}

/* pselect6: fd_set（64 個まで）を見る。ppoll と同じく 10ms おきに見直す */
static long do_pselect(struct lfd *t, long nfds, uintptr_t urd, uintptr_t uwr, uintptr_t uex, uintptr_t uts)
{
    uint64_t rd = 0, wr = 0, ex = 0, ord, owr;
    int64_t waited = 0, limit = -1;
    long k, ready;

    if (nfds < 0 || nfds > 64) {
        return -EINVAL;
    }
    if ((urd && rv_user_copy_in(&rd, urd, 8)) || (uwr && rv_user_copy_in(&wr, uwr, 8))
        || (uex && rv_user_copy_in(&ex, uex, 8))) {
        return -EFAULT;
    }
    if (uts != 0) {
        int64_t ts[2];
        if (rv_user_copy_in(ts, uts, sizeof(ts)) != 0) {
            return -EFAULT;
        }
        limit = ts[0] * 1000 + ts[1] / 1000000;
    }
    for (;;) {
        ord = owr = 0;
        ready = 0;
        for (k = 0; k < nfds; k++) {
            uint64_t bit = 1ULL << k;
            int16_t ev;
            if (!((rd | wr | ex) & bit)) {
                continue;
            }
            ev = fd_events(get_fd(t, k));
            if (ev & 0x20) {
                return -EBADF;
            }
            if ((rd & bit) && (ev & (0x1 | 0x10))) { ord |= bit; ready++; }
            if ((wr & bit) && (ev & (0x4 | 0x8)))  { owr |= bit; ready++; }
        }
        if (ready > 0 || (limit >= 0 && waited >= limit)) {
            uint64_t zero = 0;
            if ((urd && rv_user_copy_out(urd, &ord, 8)) || (uwr && rv_user_copy_out(uwr, &owr, 8))
                || (uex && rv_user_copy_out(uex, &zero, 8))) {
                return -EFAULT;
            }
            return ready;
        }
        if (rv_linux_sig_interrupt()) {
            return -EINTR;
        }
        tk_dly_tsk(10);
        waited += 10;
    }
}

/* ================================================================ /data（書き込めるディスク、ext2）と、いまのディレクトリ
 *
 *   2 台目の virtio-blk のディスク（ext2）を /data に見せる。中身は kernel/fs/ext2.c。
 *   プロセスごとに、いまのディレクトリ（cd）を持つ。相対のパスは、いまのディレクトリ（*at の dirfd があればそこ）からの
 *   パスにし、「.」「..」をほどいた絶対のパスにしてから、/tmp・/data・ディスク（tar）・装置へ振り分ける。
 *   /data の中のシンボリックリンクは、リンクの先を組み込んだパスでたどり直す（/data の中か、ディスクの tar へ）。
 */
static const char *cwd_of(int slot)
{
    return (slot >= 0 && slot < SLOTS && cwds[slot][0]) ? cwds[slot] : "/";
}

/* base（絶対）と in（絶対か相対）から、「.」「..」と重なった「/」をほどいた絶対のパスを作る */
static int norm_join(const char *base, const char *in, char *out, size_t max)
{
    char tmp[512];
    size_t n = 0, o = 0, i;
    if (in[0] != '/') {
        for (i = 0; base[i] && n < sizeof(tmp) - 2; i++) {
            tmp[n++] = base[i];
        }
        tmp[n++] = '/';
    }
    for (i = 0; in[i] && n < sizeof(tmp) - 1; i++) {
        tmp[n++] = in[i];
    }
    if (in[i]) {
        return -ENAMETOOLONG;
    }
    tmp[n] = 0;
    i = 0;
    while (tmp[i]) {
        size_t s, len;
        while (tmp[i] == '/') {
            i++;
        }
        s = i;
        while (tmp[i] && tmp[i] != '/') {
            i++;
        }
        len = i - s;
        if (len == 0 || (len == 1 && tmp[s] == '.')) {
            continue;
        }
        if (len == 2 && tmp[s] == '.' && tmp[s + 1] == '.') {
            while (o > 0 && out[o - 1] != '/') {
                o--;
            }
            if (o > 0) {
                o--;                                /* 「/」も外す */
            }
            continue;
        }
        if (o + 1 + len + 1 > max) {
            return -ENAMETOOLONG;
        }
        out[o++] = '/';
        for (; s < i; s++) {
            out[o++] = tmp[s];
        }
    }
    if (o == 0) {
        out[o++] = '/';
    }
    out[o] = 0;
    return (int)o;
}

/* 開いたディレクトリの fd の、絶対のパス */
static const char *fd_dir_path(struct lfd *d)
{
    if (d->type == FD_EXTDIR) {
        return d->f.name[0] == '/' ? d->f.name : NULL;
    }
    if (d->type == FD_TMPDIR) {
        return "/tmp";
    }
    return NULL;
}

/* 利用者のパスを、正規の絶対のパスにする。長さか、負のエラー */
static int upath_norm(uintptr_t upath, char *out, size_t max)
{
    char in[256];
    const char *base = cwd_of(cur_slot);
    size_t len = rv_user_copy_str(in, upath, sizeof(in));
    if (len == 0) {
        return -ENOENT;
    }
    if (len >= sizeof(in) - 1) {
        return -ENAMETOOLONG;
    }
    if (in[0] != '/' && cur_dirfd != AT_FDCWD && cur_tab) {
        struct lfd *d = (cur_dirfd >= 0 && cur_dirfd < NFD && cur_tab[cur_dirfd].type != FD_FREE)
                        ? &cur_tab[cur_dirfd] : NULL;
        const char *b = d ? fd_dir_path(d) : NULL;
        if (b) {
            base = b;
        }
    }
    return norm_join(base, in, out, max);
}

/* /data のパスか。0 = 違う、1 = /data そのもの、2 = /data の中（rel に /data/ を外した名前） */
static int data_path_str(const char *abs, char *rel, size_t max)
{
    const char *n = abs;
    size_t k = 0;
    if (!ext2_mounted()) {
        return 0;
    }
    if (!(n[0] == '/' && n[1] == 'd' && n[2] == 'a' && n[3] == 't' && n[4] == 'a' && (n[5] == 0 || n[5] == '/'))) {
        return 0;
    }
    n += 5;
    while (*n == '/') {
        n++;
    }
    while (n[k] && k + 1 < max) {
        rel[k] = n[k];
        k++;
    }
    rel[k] = 0;
    return k == 0 ? 1 : 2;
}

/* 利用者のパスが /data か。abs（256 バイト）に絶対のパス、rel に /data からの名前 */
static int data_path(uintptr_t upath, char *abs, char *rel)
{
    int r = upath_norm(upath, abs, 256);
    if (r < 0) {
        return 0;                                   /* エラーはいつもの道で出す */
    }
    return data_path_str(abs, rel, 256);
}

/* ext2 がリンクの先のパスを返した: abs と rel を作り直す。まだ /data の中なら 1 か 2 */
static int data_redirect(const char *redir, char *abs, char *rel)
{
    if (norm_join("/", redir, abs, 256) < 0) {
        return -ENAMETOOLONG;
    }
    return data_path_str(abs, rel, 256);
}

/* /data の名前を、リンクをたどって inode にする。follow = 0 なら最後のリンクはたどらない。
 * /data の外（ディスクの tar）を指していたら 1 を返し、abs にそのパス */
static int data_lookup(char *abs, char *rel, int follow, uint32_t *ino)
{
    char redir[EXT2_PATH_MAX];
    int depth, r;
    for (depth = 0; depth < 8; depth++) {
        r = ext2_lookup(rel, follow, ino, redir);
        if (r != EXT2_REDIRECT) {
            return r;
        }
        r = data_redirect(redir, abs, rel);
        if (r < 0) {
            return r;
        }
        if (r == 0) {
            return 1;                               /* /data の外 */
        }
    }
    return -40;                                     /* ELOOP */
}

static long data_fill_stat(uintptr_t ust, uint32_t ino)
{
    struct ext2_stat es;
    struct lnx_stat st;
    unsigned k;
    if (ext2_stat(ino, &es) != 0) {
        return -EIO;
    }
    for (k = 0; k < sizeof(st); k++) {
        ((uint8_t *)&st)[k] = 0;
    }
    st.st_dev = 0x801;
    st.st_ino = ino;
    st.st_mode = es.mode;
    st.st_nlink = es.nlink;
    st.st_uid = es.uid;
    st.st_gid = es.gid;
    st.st_size = (int64_t)es.size;
    st.st_blksize = (int32_t)es.blksize;
    st.st_blocks = es.blocks;
    st.st_atime = es.atime;
    st.st_mtime = es.mtime;
    st.st_ctime = es.ctime;
    return rv_user_copy_out(ust, &st, sizeof(st)) == 0 ? 0 : -EFAULT;
}

/* /data のファイルの読み書き（4KiB ずつ、カーネルのページを経由） */
static long ext_rw(struct lfd *d, uint64_t off, uintptr_t ubuf, size_t len, int wr)
{
    uint8_t *pg;
    size_t done = 0;
    long r = 0;
    if (d->type == FD_EXTDIR) {
        return -EISDIR;
    }
    if ((wr && d->acc == 0) || (!wr && d->acc == 1)) {
        return -EBADF;
    }
    pg = rv_kpage_alloc();
    if (pg == NULL) {
        return -ENOMEM;
    }
    while (done < len) {
        size_t n = len - done > 4096 ? 4096 : len - done;
        if (wr) {
            if (rv_user_copy_in(pg, ubuf + done, n) != 0) {
                r = -EFAULT;
                break;
            }
            r = ext2_write((uint32_t)d->pipe, off + done, pg, n);
        } else {
            r = ext2_read((uint32_t)d->pipe, off + done, pg, n);
            if (r > 0 && rv_user_copy_out(ubuf + done, pg, (size_t)r) != 0) {
                r = -EFAULT;
            }
        }
        if (r <= 0) {
            break;
        }
        done += (size_t)r;
        if ((size_t)r < n) {
            break;
        }
    }
    rv_kpage_free(pg);
    return done > 0 ? (long)done : r;
}

static uint64_t ext_size(struct lfd *d)
{
    struct ext2_stat es;
    return ext2_stat((uint32_t)d->pipe, &es) == 0 ? es.size : 0;
}

static long ext_getdents64(struct lfd *d, uintptr_t ubuf, size_t len)
{
    size_t used = 0;
    for (;;) {
        char name[256];
        uint32_t child;
        uint8_t ft;
        uint64_t pos = d->off;
        size_t w;
        int r = ext2_readdir((uint32_t)d->pipe, &pos, name, sizeof(name), &child, &ft);
        if (r < 0) {
            return used ? (long)used : r;
        }
        if (r == 0) {
            break;
        }
        w = put_dirent(ubuf + used, len - used, child, (int64_t)pos,
                       ft == 2 ? 4 : ft == 7 ? 10 : ft == 1 ? 8 : 0, name);
        if (w == 0) {
            if (used == 0) {
                return -EINVAL;
            }
            break;
        }
        used += w;
        d->off = pos;
    }
    return (long)used;
}

/* /data を開く。abs・rel は data_path で作ったもの */
static long data_open(struct lfd *t, char *abs, char *rel, long flags, unsigned mode)
{
    char redir[EXT2_PATH_MAX];
    struct ext2_stat es;
    uint32_t ino;
    int depth, r, fd, acc = (int)(flags & O_ACCMODE), ef = 0;

    if (flags & O_CREAT) {
        ef |= EXT2_CREAT;
    }
    if (flags & O_EXCL) {
        ef |= EXT2_EXCL;
    }
    if ((flags & O_TRUNC) && acc != 0) {
        ef |= EXT2_TRUNC;
    }
    if (acc != 0 && ext2_readonly()) {
        return -EROFS;
    }
    for (depth = 0; ; depth++) {
        if (depth >= 8) {
            return -40;
        }
        r = ext2_open(rel, ef, (uint16_t)(mode & 07777), &ino, redir);
        if (r != EXT2_REDIRECT) {
            break;
        }
        r = data_redirect(redir, abs, rel);
        if (r < 0) {
            return r;
        }
        if (r == 0) {
            return -EXDEV;                          /* /data の外を指すリンクは、開くときにはたどらない */
        }
    }
    if (r < 0) {
        return r;
    }
    if (ext2_stat(ino, &es) != 0) {
        return -EIO;
    }
    if ((es.mode & 0xF000) == 0x4000) {
        if (acc != 0) {
            return -EISDIR;
        }
    } else if (flags & O_DIRECTORY) {
        return -ENOTDIR;
    }
    fd = alloc_fd(t, 0);
    if (fd < 0) {
        return fd;
    }
    t[fd].type = (es.mode & 0xF000) == 0x4000 ? FD_EXTDIR : FD_EXT;
    t[fd].pipe = (int)ino;
    t[fd].ino = ino;
    t[fd].acc = acc;
    t[fd].append = (flags & O_APPEND) ? 1 : 0;
    t[fd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
    t[fd].off = 0;
    {
        size_t k;
        for (k = 0; abs[k] && k < sizeof(t[fd].f.name) - 1; k++) {
            t[fd].f.name[k] = abs[k];
        }
        t[fd].f.name[abs[k] ? 0 : k] = 0;           /* 長すぎるパスは持たない（dirfd の基準にならない） */
    }
    t[fd].f.size = 0;
    ext2_iref(ino);
    return fd;
}

/* /data の名前を変える系（パスの親の途中のリンクは、たどり直す） */
#define DATA_RETRY(call)                                            \
    do {                                                            \
        int _d;                                                     \
        for (_d = 0; _d < 8; _d++) {                                \
            r = (call);                                             \
            if (r != EXT2_REDIRECT) {                               \
                break;                                              \
            }                                                       \
            r = data_redirect(redir, abs, rel);                     \
            if (r <= 0) {                                           \
                r = r < 0 ? r : -EXDEV;                             \
                break;                                              \
            }                                                       \
        }                                                           \
        if (r == EXT2_REDIRECT) {                                   \
            r = -40;                                                \
        }                                                           \
    } while (0)

/* 時刻（ext2 の時刻のため）。linux_syscall.c の RTC から */
extern uint32_t rv_now_unix(void);
extern int virtio_blk_data_present(void);
extern uint64_t virtio_blk_data_capacity(void);
extern int virtio_blk_data_read(uint64_t sector, void *buf, unsigned count);
extern int virtio_blk_data_write(uint64_t sector, const void *buf, unsigned count);

/* 起動のとき: 2 台目のディスクが ext2 なら /data にする */
void rv_data_mount(void)
{
    int r;
    if (!virtio_blk_data_present()) {
        uart_puts("[data] no second disk: /data is not available\r\n");
        return;
    }
    ext2_set_prefix("/data");
    r = ext2_mount(virtio_blk_data_read, virtio_blk_data_write, virtio_blk_data_capacity(), rv_now_unix);
    if (r != 0) {
        uart_puts("[data] second disk is not ext2 (make it with mke2fs -t ext2): /data is not available\r\n");
        return;
    }
    {
        uint64_t bsz, blocks, bfree, files, ffree;
        char b[24];
        int i;
        uint64_t v;
        ext2_statfs(&bsz, &blocks, &bfree, &files, &ffree);
        uart_puts("[data] /data: ext2, ");
        v = blocks * bsz / (1024 * 1024);
        i = 23;
        b[i] = 0;
        do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
        uart_puts(b + i);
        uart_puts(" MiB, free ");
        v = bfree * bsz / (1024 * 1024);
        i = 23;
        do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
        uart_puts(b + i);
        uart_puts(ext2_readonly() ? " MiB (read-only)\r\n" : " MiB\r\n");
    }
    ext2_start_sync_task();             /* 1 秒ごとに、ためてある分をディスクへ */
}

/* execve から: パスがいまのディレクトリからの相対なら絶対にし、/data のプログラムなら buf に読む。
 * 1 = 読んだ（*size）、0 = /data ではない（npath に絶対のパス）、負 = エラー */
int rv_linux_exec_read(int slot, const char *path, char *buf, size_t max, int64_t *size, char *npath)
{
    char abs[256], rel[256];
    struct ext2_stat es;
    uint32_t ino;
    int r, k;
    if (norm_join(cwd_of(slot), path, abs, sizeof(abs)) < 0) {
        return -ENAMETOOLONG;
    }
    for (k = 0; abs[k]; k++) {
        npath[k] = abs[k];
    }
    npath[k] = 0;
    for (k = 0; path[k] && path[k] != '/'; k++) {
    }
    if (path[k] == 0) {                             /* 「/」のない名前（bash・busybox.elf）は、今までどおりディスクから探す */
        for (k = 0; path[k]; k++) {
            npath[k] = path[k];
        }
        npath[k] = 0;
        return 0;
    }
    if (data_path_str(abs, rel, sizeof(rel)) == 0) {
        return 0;
    }
    r = data_lookup(abs, rel, 1, &ino);
    if (r == 1) {
        for (k = 0; abs[k]; k++) {
            npath[k] = abs[k];
        }
        npath[k] = 0;
        return 0;                                   /* リンクの先はディスク */
    }
    if (r < 0) {
        return r;
    }
    if (ext2_stat(ino, &es) != 0) {
        return -EIO;
    }
    if ((es.mode & 0xF000) != 0x8000) {
        return -EACCES;
    }
    if (es.size > max) {
        return -ENOMEM;
    }
    if (ext2_read(ino, 0, buf, (size_t)es.size) != (long)es.size) {
        return -EIO;
    }
    *size = (int64_t)es.size;
    return 1;
}

/* ---- UNIX ソケット: fd を渡す（SCM_RIGHTS） ---- */
typedef char lfd_fits_blob[sizeof(struct lfd) <= FDBLOB ? 1 : -1];

/* linux_unix.c から: 受け取られなかった fd の中身を閉じる */
void rv_linux_fd_blob_drop(void *blob)
{
    struct lfd d;
    memcpy(&d, blob, sizeof(d));
    fd_close(&d);
}

#define SOL_SOCKET  1
#define SCM_RIGHTS  1
#define MSG_PEEK        0x2
#define MSG_DONTWAIT    0x40
#define MSG_CTRUNC      0x8
#define MSG_CMSG_CLOEXEC 0x40000000
#define MAXPASS     16

/* sendmsg・recvmsg（UNIX ソケット）。struct msghdr: name, namelen, iov, iovlen, control, controllen, flags */
static long usock_msg(struct lfd *t, struct lfd *d, int send, uintptr_t umh, long flags)
{
    uint64_t mh[7];
    uint64_t iov[16];
    unsigned niov, k;
    int nonblock = d->nonblock || (flags & MSG_DONTWAIT);
    static uint8_t blobs[MAXPASS * FDBLOB];
    long total = 0, r;

    if (rv_user_copy_in(mh, umh, sizeof(mh)) != 0) {
        return -EFAULT;
    }
    niov = (unsigned)mh[3];
    if (niov > 8) {
        return -EINVAL;
    }
    if (niov > 0 && rv_user_copy_in(iov, (uintptr_t)mh[2], niov * 16) != 0) {
        return -EFAULT;
    }
    if (send) {
        int nb = 0;
        uint64_t clen = mh[5];
        uintptr_t cp = (uintptr_t)mh[4];
        while (cp && clen >= 16) {               /* struct cmsghdr { size_t len; int level; int type; } + データ */
            uint64_t ch[2];
            if (rv_user_copy_in(ch, cp, 16) != 0) {
                return -EFAULT;
            }
            if (ch[0] < 16 || ch[0] > clen) {
                return -EINVAL;
            }
            if ((uint32_t)ch[1] == SOL_SOCKET && (uint32_t)(ch[1] >> 32) == SCM_RIGHTS) {
                unsigned n = (unsigned)((ch[0] - 16) / 4), m;
                int32_t fds[MAXPASS];
                if (nb + (int)n > MAXPASS) {
                    return -EINVAL;
                }
                if (rv_user_copy_in(fds, cp + 16, n * 4) != 0) {
                    return -EFAULT;
                }
                for (m = 0; m < n; m++) {
                    struct lfd *s = get_fd(t, fds[m]);
                    if (s == NULL) {
                        while (nb-- > 0) {
                            rv_linux_fd_blob_drop(blobs + (size_t)nb * FDBLOB);
                        }
                        return -EBADF;
                    }
                    fd_ref(s);                  /* 渡している間も生かしておく */
                    memcpy(blobs + (size_t)nb * FDBLOB, s, sizeof(*s));
                    ((struct lfd *)(void *)(blobs + (size_t)nb * FDBLOB))->cloexec = 0;
                    nb++;
                }
            }
            k = (unsigned)((ch[0] + 7) & ~7ULL);
            if (k >= clen) {
                break;
            }
            cp += k;
            clen -= k;
        }
        for (k = 0; k < niov; k++) {
            if (iov[k * 2 + 1] == 0) {
                continue;
            }
            r = rv_usock_send(d->pipe, (uintptr_t)iov[k * 2], (size_t)iov[k * 2 + 1], nonblock,
                              blobs, total == 0 ? nb : 0);
            if (r < 0) {
                if (total == 0) {
                    while (nb-- > 0) {
                        rv_linux_fd_blob_drop(blobs + (size_t)nb * FDBLOB);
                    }
                    return r;
                }
                break;
            }
            total += r;
            if ((uint64_t)r < iov[k * 2 + 1]) {
                break;
            }
        }
        if (total == 0 && nb > 0) {             /* 0 バイトでは fd を渡せない */
            while (nb-- > 0) {
                rv_linux_fd_blob_drop(blobs + (size_t)nb * FDBLOB);
            }
        }
        return total;
    } else {
        int got = 0, nb, inst[MAXPASS], ni = 0, trunc = 0;
        uint64_t clen = mh[5], used = 0, zero = 0;
        for (k = 0; k < niov; k++) {
            if (iov[k * 2 + 1] == 0) {
                continue;
            }
            r = rv_usock_recv(d->pipe, (uintptr_t)iov[k * 2], (size_t)iov[k * 2 + 1],
                              total > 0 ? 1 : nonblock, (flags & MSG_PEEK) != 0,
                              blobs + (size_t)got * FDBLOB, MAXPASS - got, &nb);
            if (r < 0) {
                if (total == 0) {
                    return r;
                }
                break;
            }
            got += nb;
            total += r;
            if (r == 0 || (uint64_t)r < iov[k * 2 + 1]) {
                break;
            }
        }
        /* 受け取った fd をこのプロセスの表に入れる */
        for (k = 0; k < (unsigned)got; k++) {
            int fd = ((uint64_t)(16 + (ni + 1) * 4) <= clen) ? alloc_fd(t, 0) : -1;
            if (fd < 0) {
                rv_linux_fd_blob_drop(blobs + (size_t)k * FDBLOB);
                trunc = 1;
                continue;
            }
            memcpy(&t[fd], blobs + (size_t)k * FDBLOB, sizeof(struct lfd));
            t[fd].cloexec = (flags & MSG_CMSG_CLOEXEC) ? 1 : 0;
            inst[ni++] = fd;
        }
        if (ni > 0) {
            uint64_t ch[2];
            ch[0] = 16 + (uint64_t)ni * 4;
            ch[1] = SOL_SOCKET | ((uint64_t)SCM_RIGHTS << 32);
            if (rv_user_copy_out((uintptr_t)mh[4], ch, 16) != 0
                || rv_user_copy_out((uintptr_t)mh[4] + 16, inst, (size_t)ni * 4) != 0) {
                return -EFAULT;
            }
            used = (ch[0] + 7) & ~7ULL;
            if (used > clen) {
                used = ch[0];
            }
        }
        rv_user_copy_out(umh + 40, &used, 8);                       /* msg_controllen */
        zero = trunc ? MSG_CTRUNC : 0;
        rv_user_copy_out(umh + 48, &zero, 4);                       /* msg_flags */
        if (mh[0]) {
            uint32_t nl = 0;
            rv_user_copy_out(umh + 8, &nl, 4);                      /* 相手の名前はなし */
        }
        return total;
    }
}

/* UNIX ソケットの、ソケットのシステムコール（200〜212） */
static long usock_call(struct rv_uproc *p, struct lfd *t, struct lfd *d, long nr, const uint64_t *a)
{
    int nb;
    char dummy[1];
    switch (nr) {
    case 200: return rv_usock_bind(d->pipe, (uintptr_t)a[1], (long)a[2]);
    case 201: return rv_usock_listen(d->pipe, (long)a[1]);
    case 203: return rv_usock_connect(d->pipe, (uintptr_t)a[1], (long)a[2], rv_linux_pid_of(p));
    case 204: return rv_usock_name(d->pipe, (uintptr_t)a[1], (uintptr_t)a[2], 0);
    case 205: return rv_usock_name(d->pipe, (uintptr_t)a[1], (uintptr_t)a[2], 1);
    case 206: return rv_usock_send(d->pipe, (uintptr_t)a[1], (size_t)a[2],
                                   d->nonblock || ((long)a[3] & MSG_DONTWAIT), NULL, 0);
    case 207: return rv_usock_recv(d->pipe, (uintptr_t)a[1], (size_t)a[2],
                                   d->nonblock || ((long)a[3] & MSG_DONTWAIT), ((long)a[3] & MSG_PEEK) != 0,
                                   dummy, 0, &nb);
    case 208: return 0;                         /* setsockopt: 受け付けるだけ */
    case 209:                                   /* getsockopt */
        if ((long)a[1] == SOL_SOCKET && (long)a[2] == 17) {         /* SO_PEERCRED */
            return rv_usock_peercred(d->pipe, (uintptr_t)a[3], (uintptr_t)a[4]);
        }
        if ((long)a[1] == SOL_SOCKET && ((long)a[2] == 3 || (long)a[2] == 4 || (long)a[2] == 39)) {
            /* SO_TYPE（SOCK_STREAM）・SO_ERROR（0）・SO_DOMAIN（AF_UNIX） */
            int32_t v = (long)a[2] == 3 ? 1 : (long)a[2] == 39 ? 1 : 0;
            uint32_t len = 4;
            if (rv_user_copy_out((uintptr_t)a[3], &v, 4) != 0 || rv_user_copy_out((uintptr_t)a[4], &len, 4) != 0) {
                return -EFAULT;
            }
            return 0;
        }
        return -92;                             /* ENOPROTOOPT */
    case 210: return rv_usock_shutdown(d->pipe, (long)a[1]);
    case 211: return usock_msg(t, d, 1, (uintptr_t)a[1], (long)a[2]);
    case 212: return usock_msg(t, d, 0, (uintptr_t)a[1], (long)a[2]);
    }
    return -EINVAL;
}

/* ---- epoll ---- */
#define EPOLLIN      0x001
#define EPOLLOUT     0x004
#define EPOLLERR     0x008
#define EPOLLHUP     0x010
#define EPOLLONESHOT (1u << 30)

static long epoll_ctl(struct lfd *t, long epfd, long op, long fd, uintptr_t uev)
{
    struct lfd *e = get_fd(t, epfd);
    struct lepoll *ep;
    uint32_t ev[4];
    int k;
    if (e == NULL || get_fd(t, fd) == NULL) {
        return -EBADF;
    }
    if (e->type != FD_EPOLL || fd == epfd) {
        return -EINVAL;
    }
    ep = &eps[e->pipe];
    if (op != 2 && rv_user_copy_in(ev, uev, 16) != 0) {             /* struct epoll_event { u32 events; u64 data; } */
        return -EFAULT;
    }
    for (k = 0; k < ep->n && ep->it[k].fd != fd; k++) {
    }
    if (op == 1) {                              /* EPOLL_CTL_ADD */
        if (k < ep->n) {
            return -17;                         /* EEXIST */
        }
        if (ep->n >= EPOLL_N) {
            return -28;                         /* ENOSPC */
        }
        ep->it[ep->n].fd = (int32_t)fd;
        ep->it[ep->n].events = ev[0];
        ep->it[ep->n].data = (uint64_t)ev[2] | ((uint64_t)ev[3] << 32);
        ep->n++;
        return 0;
    }
    if (k >= ep->n) {
        return -2;                              /* ENOENT */
    }
    if (op == 2) {                              /* EPOLL_CTL_DEL */
        ep->it[k] = ep->it[--ep->n];
        return 0;
    }
    if (op == 3) {                              /* EPOLL_CTL_MOD */
        ep->it[k].events = ev[0];
        ep->it[k].data = (uint64_t)ev[2] | ((uint64_t)ev[3] << 32);
        return 0;
    }
    return -EINVAL;
}

/* 待つ。印のついた fd が 1 つでもあれば返す。なければ 5ms おきに見直す（limit ミリ秒まで、負は無限） */
static long epoll_wait(struct lfd *t, long epfd, uintptr_t uout, long max, int64_t limit)
{
    struct lfd *e = get_fd(t, epfd);
    struct lepoll *ep;
    int64_t waited = 0;
    if (e == NULL) {
        return -EBADF;
    }
    if (e->type != FD_EPOLL || max <= 0) {
        return -EINVAL;
    }
    ep = &eps[e->pipe];
    for (;;) {
        long n = 0;
        int k;
        for (k = 0; k < ep->n && n < max; k++) {
            struct lfd *x = get_fd(t, ep->it[k].fd);
            uint32_t want = ep->it[k].events, got;
            if (want == 0) {
                continue;                       /* ONESHOT で止めたもの */
            }
            got = x ? (uint32_t)(uint16_t)fd_events(x) : EPOLLHUP;
            got &= (want & (EPOLLIN | EPOLLOUT)) | EPOLLERR | EPOLLHUP;
            if (got) {
                uint32_t out[4];
                out[0] = got;
                out[1] = 0;
                out[2] = (uint32_t)ep->it[k].data;
                out[3] = (uint32_t)(ep->it[k].data >> 32);
                if (rv_user_copy_out(uout + (uintptr_t)n * 16, out, 16) != 0) {
                    return -EFAULT;
                }
                if (want & EPOLLONESHOT) {
                    ep->it[k].events = 0;
                }
                n++;
            }
        }
        if (n > 0 || (limit >= 0 && waited >= limit)) {
            return n;
        }
        if (rv_linux_sig_interrupt()) {
            return -EINTR;
        }
        tk_dly_tsk(limit >= 0 && limit - waited < 5 ? (int)(limit - waited) : 5);
        waited += limit >= 0 && limit - waited < 5 ? limit - waited : 5;
    }
}

int rv_linux_fs_syscall(struct rv_uproc *p, long nr, const uint64_t *a, long *ret)
{
    struct lfd *t = table(p);
    struct lfd *d;
    long r;

    if (t == NULL) {
        return 0;
    }
    cur_slot = rv_uproc_slot(p);
    cur_tab = t;
    switch (nr) {                           /* *at の dirfd（相対パスの基準） */
    case LNX_openat: case LNX_newfstatat: case LNX_faccessat: case LNX_faccessat2: case LNX_unlinkat:
    case LNX_readlinkat: case LNX_renameat: case LNX_renameat2: case 34 /* mkdirat */: case 53 /* fchmodat */:
    case 88 /* utimensat */: case 37 /* linkat */:
        cur_dirfd = (int)(long)a[0];
        break;
    case 36:                                /* symlinkat(target, newdirfd, linkpath) */
        cur_dirfd = (int)(long)a[1];
        break;
    default:
        cur_dirfd = AT_FDCWD;
    }
    switch (nr) {
    case LNX_read:
        d = get_fd(t, (long)a[0]);
        r = d ? do_read(d, (uintptr_t)a[1], (size_t)a[2]) : -EBADF;
        break;
    case LNX_write:
        d = get_fd(t, (long)a[0]);
        r = d ? do_write(d, (uintptr_t)a[1], (size_t)a[2]) : -EBADF;
        break;
    case LNX_readv:
    case LNX_writev:
        d = get_fd(t, (long)a[0]);
        r = d ? do_iov(d, (uintptr_t)a[1], (long)a[2], nr == LNX_writev) : -EBADF;
        break;
    case LNX_pread64:
        d = get_fd(t, (long)a[0]);
        if (d == NULL) {
            r = -EBADF;
        } else if (d->type == FD_EXT || d->type == FD_EXTDIR) {
            r = ext_rw(d, a[3], (uintptr_t)a[1], (size_t)a[2], 0);
        } else if (d->type == FD_BLK) {
            r = rv_usbms_rw(a[3], (uintptr_t)a[1], (size_t)a[2], 0, 1);
        } else if (d->type == FD_FB) {
            r = rv_fb_read(a[3], (uintptr_t)a[1], (size_t)a[2]);
        } else if (d->type == FD_IO) {
            r = vio_rw(a[3], (uintptr_t)a[1], (size_t)a[2], 0);
        } else if (d->type == FD_TMP) {
            r = d->acc == 1 ? -EBADF : rv_tmp_read(d->pipe, a[3], (uintptr_t)a[1], (size_t)a[2]);
        } else if (d->type != FD_FILE) {
            r = (d->type == FD_DIR || d->type == FD_TMPDIR) ? -EISDIR : -ESPIPE;
        } else {
            r = file_read_at(d, a[3], (uintptr_t)a[1], (size_t)a[2]);
        }
        break;
    case LNX_pwrite64:
        d = get_fd(t, (long)a[0]);
        if (d == NULL) {
            r = -EBADF;
        } else if (d->type == FD_EXT || d->type == FD_EXTDIR) {
            r = ext_rw(d, a[3], (uintptr_t)a[1], (size_t)a[2], 1);
        } else if (d->type == FD_BLK) {
            r = rv_usbms_rw(a[3], (uintptr_t)a[1], (size_t)a[2], 1, 1);
        } else if (d->type == FD_FB) {
            r = rv_fb_write(a[3], (uintptr_t)a[1], (size_t)a[2]);
        } else if (d->type == FD_IO) {
            r = vio_rw(a[3], (uintptr_t)a[1], (size_t)a[2], 1);
        } else if (d->type == FD_TMP) {
            r = d->acc == 0 ? -EBADF : rv_tmp_write(d->pipe, a[3], (uintptr_t)a[1], (size_t)a[2]);
        } else {
            r = (d->type == FD_FILE) ? -EBADF : -ESPIPE;
        }
        break;
    case LNX_ftruncate:
        d = get_fd(t, (long)a[0]);
        if (d && d->type == FD_EXT) {
            r = (d->acc == 0) ? -EINVAL : ext2_truncate((uint32_t)d->pipe, a[1]);
            break;
        }
        d = get_fd(t, (long)a[0]);
        if (d == NULL) {
            r = -EBADF;
        } else if (d->type == FD_TMP && d->acc != 0) {
            r = (int64_t)a[1] < 0 ? -EINVAL : rv_tmp_truncate(d->pipe, a[1]);
        } else {
            r = (d->type == FD_FILE) ? -EROFS : -EINVAL;
        }
        break;
    case LNX_unlinkat: {
        char tn[72];
        struct tarfs_file f;
        int64_t ino;
        int k;
        {
            char abs[256], rel[256], redir[EXT2_PATH_MAX];
            if (data_path((uintptr_t)a[1], abs, rel)) {
                DATA_RETRY(ext2_remove(rel, ((long)a[2] & AT_REMOVEDIR) ? 1 : 0, redir));
                break;
            }
        }
        k = tmp_path((uintptr_t)a[1], tn, sizeof(tn));
        if (k < 0) {
            r = k;
        } else if (k == 1) {
            r = -EBUSY;
        } else if (k == 2) {
            r = ((long)a[2] & 0x200) ? -ENOTDIR : rv_tmp_unlink(tn);    /* AT_REMOVEDIR */
        } else {
            k = lookup((uintptr_t)a[1], &f, &ino);
            r = (k < 0) ? k : -EROFS;
        }
        break;
    }
    case LNX_renameat:
    case LNX_renameat2: {
        char from[72], to[72];
        int k1, k2;
        {
            char abs[256], rel[256], abs2[256], rel2[256], redir[EXT2_PATH_MAX];
            int d1 = data_path((uintptr_t)a[1], abs, rel);
            int d2 = data_path((uintptr_t)a[3], abs2, rel2);
            if (d1 && d2) {
                if (nr == LNX_renameat2 && (long)a[4] != 0) {
                    r = -EINVAL;
                } else {
                    r = ext2_rename(rel, rel2, redir);
                    if (r == EXT2_REDIRECT) {
                        r = -40;                /* 親の途中にリンクのある名前の変更は、まだ扱わない */
                    }
                }
                break;
            }
            if (d1 || d2) {
                r = -EXDEV;                     /* /data とほかの場所の間は移せない（cp と rm で） */
                break;
            }
        }
        k1 = tmp_path((uintptr_t)a[1], from, sizeof(from));
        k2 = tmp_path((uintptr_t)a[3], to, sizeof(to));
        if (k1 < 0 || k2 < 0) {
            r = k1 < 0 ? k1 : k2;
        } else if (k1 == 2 && k2 == 2) {
            r = (nr == LNX_renameat2 && (long)a[4] != 0) ? -EINVAL : rv_tmp_rename(from, to);
        } else if (k1 == 2 || k2 == 2) {
            r = -EXDEV;                         /* /tmp とディスクの間は移せない（ディスクは読み取り専用） */
        } else {
            r = (k1 == 1 || k2 == 1) ? -EBUSY : -EROFS;
        }
        break;
    }
    case LNX_openat: {
        struct tarfs_file f;
        int64_t ino;
        int k, fd;
        long flags = (long)a[2];
        if ((long)a[0] != AT_FDCWD && (long)a[0] >= 0 && get_fd(t, (long)a[0]) == NULL) {
            r = -EBADF;
            break;
        }
        k = pty_path((uintptr_t)a[1], p);
        if (k != PTY_NONE) {
            int master = (k == PTY_MASTER), n = -1;
            if (k == PTY_NOTTY) {
                r = -6;                         /* ENXIO: 制御端末がない */
                break;
            }
            if (k == PTY_MASTER) {
                n = rv_pty_open_master();
            } else if (k == PTY_CONSOLE) {
                k = -1;                         /* 制御端末はコンソール: 下の dev_kind で開く */
            } else {
                n = rv_pty_open_slave(k);
                if (n >= 0 && !(flags & 0400)) {    /* O_NOCTTY でなければ、制御端末になりうる */
                    rv_linux_set_ctty(p, n, 0);
                }
            }
            if (k >= 0 || master) {
                if (n < 0) {
                    r = n;
                    break;
                }
                fd = alloc_fd(t, 0);
                if (fd < 0) {
                    rv_pty_unref(n, master);
                    r = fd;
                    break;
                }
                t[fd].type = master ? FD_PTM : FD_PTS;
                t[fd].pipe = n;
                t[fd].ino = n;
                t[fd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
                t[fd].nonblock = (flags & 04000) ? 1 : 0;
                r = fd;
                break;
            }
        }
        k = dev_kind((uintptr_t)a[1]);
        if ((k & 0xff) == FD_INPUT) {               /* /dev/input/event0（キーボード）・event1（マウス） */
            int which = k >> 8;
            if (which ? !rv_usb_ptr_on() : !rv_usb_kbd_on()) {
                r = -ENODEV;
                break;
            }
            fd = alloc_fd(t, 0);
            if (fd >= 0) {
                t[fd].type = FD_INPUT;
                t[fd].pipe = which;
                t[fd].ino = which;
                t[fd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
                t[fd].nonblock = (flags & 04000) ? 1 : 0;
                t[fd].off = rv_evdev_open(which);
            }
            r = fd;
            break;
        }
        if (k == FD_IOLOG) {                        /* 機器の操作の記録（安全の決まり 7） */
            fd = alloc_fd(t, 0);
            if (fd >= 0) {
                t[fd].type = FD_IOLOG;
                t[fd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
                t[fd].off = 0;
            }
            r = fd;
            break;
        }
        if (k == FD_IO) {
            fd = alloc_fd(t, 0);
            if (fd >= 0) {
                t[fd].type = FD_IO;
                t[fd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
                t[fd].off = 0;
            }
            r = fd;
            break;
        }
        if (k == FD_MOUSE) {
            if (!rv_usb_ptr_on()) {
                r = -ENODEV;
                break;
            }
            fd = alloc_fd(t, 0);
            if (fd >= 0) {
                t[fd].type = FD_MOUSE;
                t[fd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
                t[fd].off = 0;
            }
            r = fd;
            break;
        }
        if (k == FD_BLK) {
            if (!rv_usbms_present()) {
                r = -ENODEV;
                break;
            }
            fd = alloc_fd(t, 0);
            if (fd >= 0) {
                t[fd].type = FD_BLK;
                t[fd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
                t[fd].off = 0;
                t[fd].f.size = rv_usbms_size();
            }
            r = fd;
            break;
        }
        if (k == FD_FB) {
            if (!virtio_gpu_present()) {
                r = -ENODEV;
                break;
            }
            fd = alloc_fd(t, 0);
            if (fd >= 0) {
                t[fd].type = FD_FB;
                t[fd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
                t[fd].off = 0;
                t[fd].f.size = rv_fb_size();
            }
            r = fd;
            break;
        }
        if (k == FD_DSP) {
            int e = rv_snd_open();
            if (e < 0) {
                r = e;                      /* 音の装置がない（ENODEV）・使用中（EBUSY） */
                break;
            }
            fd = alloc_fd(t, 0);
            if (fd < 0) {
                rv_snd_close();
                r = fd;
                break;
            }
            t[fd].type = FD_DSP;
            t[fd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
            t[fd].off = 0;
            dsp_refs = 1;
            r = fd;
            break;
        }
        if (k > 0) {
            fd = alloc_fd(t, 0);
            if (fd >= 0) {
                t[fd].type = k;
                t[fd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
                t[fd].off = 0;
            }
            r = fd;
            break;
        }
        {
            char abs[256], rel[256];
            if (data_path((uintptr_t)a[1], abs, rel)) {     /* /data（書き込めるディスク） */
                r = data_open(t, abs, rel, flags, (unsigned)a[3]);
                break;
            }
        }
        {
            char tn[72];
            int tk = tmp_path((uintptr_t)a[1], tn, sizeof(tn));
            if (tk < 0) {
                r = tk;
                break;
            }
            if (tk == 1) {                      /* /tmp のフォルダ */
                if ((flags & O_ACCMODE) != 0) {
                    r = -EISDIR;
                    break;
                }
                fd = alloc_fd(t, 0);
                if (fd >= 0) {
                    t[fd].type = FD_TMPDIR;
                    t[fd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
                    t[fd].off = 0;
                    t[fd].ino = 3;
                    t[fd].f.name[0] = 't'; t[fd].f.name[1] = 'm'; t[fd].f.name[2] = 'p'; t[fd].f.name[3] = 0;
                }
                r = fd;
                break;
            }
            if (tk == 2) {                      /* /tmp の中のファイル */
                int i = rv_tmp_lookup(tn);
                if (i >= 0 && (flags & O_CREAT) && (flags & O_EXCL)) {
                    r = -EEXIST;
                    break;
                }
                if (flags & O_DIRECTORY) {
                    r = i >= 0 ? -ENOTDIR : -ENOENT;
                    break;
                }
                if (i < 0) {
                    if (!(flags & O_CREAT)) {
                        r = -ENOENT;
                        break;
                    }
                    i = rv_tmp_create(tn);
                    if (i < 0) {
                        r = i;
                        break;
                    }
                }
                fd = alloc_fd(t, 0);
                if (fd < 0) {
                    r = fd;
                    break;
                }
                if ((flags & O_TRUNC) && (flags & O_ACCMODE) != 0) {
                    rv_tmp_truncate(i, 0);
                }
                t[fd].type = FD_TMP;
                t[fd].pipe = i;
                t[fd].acc = (int)(flags & O_ACCMODE);
                t[fd].append = (flags & O_APPEND) ? 1 : 0;
                t[fd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
                t[fd].off = 0;
                t[fd].ino = 200000 + i;
                tmp_as_file(i, &t[fd].f);
                rv_tmp_ref(i);
                r = fd;
                break;
            }
        }
        k = lookup((uintptr_t)a[1], &f, &ino);
        if (k < 0) {
            r = (flags & O_CREAT) ? -EROFS : k;
            break;
        }
        if ((flags & O_ACCMODE) != 0 || (flags & (O_CREAT | O_TRUNC))) {
            r = (k == 0) ? -EISDIR : -EROFS;
            break;
        }
        if (k == 1 && (flags & O_DIRECTORY)) {
            r = -ENOTDIR;
            break;
        }
        fd = alloc_fd(t, 0);
        if (fd < 0) {
            r = fd;
            break;
        }
        t[fd].type = (k == 0) ? FD_DIR : FD_FILE;
        t[fd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
        t[fd].f = f;
        t[fd].ino = ino;
        t[fd].off = 0;
        r = fd;
        break;
    }
    case LNX_close:
        d = get_fd(t, (long)a[0]);
        if (d) {
            fd_close(d);
            r = 0;
        } else {
            r = -EBADF;
        }
        break;
    case LNX_lseek: {
        int64_t base, off = (int64_t)a[1];
        d = get_fd(t, (long)a[0]);
        if (d == NULL) {
            r = -EBADF;
            break;
        }
        if (d->type == FD_NULL || d->type == FD_ZERO || d->type == FD_RANDOM) {
            r = 0;
            break;
        }
        if (d->type == FD_CONSOLE || d->type == FD_PIPE_R || d->type == FD_PIPE_W) {
            r = -ESPIPE;
            break;
        }
        if (d->type == FD_DIR || d->type == FD_TMPDIR || d->type == FD_EXTDIR) {
            if ((long)a[2] == 0 && off == 0) {  /* rewinddir */
                d->off = 0;
                r = 0;
            } else {
                r = -EINVAL;
            }
            break;
        }
        base = ((long)a[2] == 0) ? 0 : ((long)a[2] == 1) ? (int64_t)d->off
             : ((long)a[2] == 2) ? (int64_t)(d->type == FD_TMP ? rv_tmp_size(d->pipe)
                                            : d->type == FD_EXT ? ext_size(d) : d->f.size) : -1;
        if (base < 0 || base + off < 0) {
            r = -EINVAL;
            break;
        }
        d->off = (uint64_t)(base + off);
        r = (long)d->off;
        break;
    }
    case LNX_fstat:
        d = get_fd(t, (long)a[0]);
        if (d && (d->type == FD_EXT || d->type == FD_EXTDIR)) {
            r = data_fill_stat((uintptr_t)a[1], (uint32_t)d->pipe);
        } else if (d && d->type == FD_TMP) {
            tmp_as_file(d->pipe, &d->f);
            r = fill_stat((uintptr_t)a[1], FD_FILE, &d->f, d->ino);
        } else if (d && d->type == FD_TMPDIR) {
            r = fill_stat((uintptr_t)a[1], FD_DIR, &d->f, d->ino);
        } else {
            r = d ? fill_stat((uintptr_t)a[1], d->type, &d->f, d->ino) : -EBADF;
        }
        break;
    case LNX_newfstatat: {
        struct tarfs_file f;
        int64_t ino;
        int k;
        char c0 = 0;
        rv_user_copy_in(&c0, (uintptr_t)a[1], 1);
        if (((long)a[3] & AT_EMPTY_PATH) && c0 == 0) {
            d = get_fd(t, (long)a[0]);
            if (d && (d->type == FD_EXT || d->type == FD_EXTDIR)) {
                r = data_fill_stat((uintptr_t)a[2], (uint32_t)d->pipe);
            } else if (d && d->type == FD_TMP) {
                tmp_as_file(d->pipe, &d->f);
                r = fill_stat((uintptr_t)a[2], FD_FILE, &d->f, d->ino);
            } else if (d && d->type == FD_TMPDIR) {
                r = fill_stat((uintptr_t)a[2], FD_DIR, &d->f, d->ino);
            } else {
                r = d ? fill_stat((uintptr_t)a[2], d->type, &d->f, d->ino) : -EBADF;
            }
            break;
        }
        {
            char abs[256], rel[256];
            uint32_t eino;
            if (data_path((uintptr_t)a[1], abs, rel)) {
                k = data_lookup(abs, rel, ((long)a[3] & AT_SYMLINK_NOFOLLOW) ? 0 : 1, &eino);
                if (k == 1) {                   /* /data の外を指すリンク: ディスクの tar で */
                    k = rv_linux_resolve(abs, &f, &ino);
                    r = (k < 0) ? k : fill_stat((uintptr_t)a[2], k == 0 ? FD_DIR : FD_FILE, &f, ino);
                } else {
                    r = (k < 0) ? k : data_fill_stat((uintptr_t)a[2], eino);
                }
                break;
            }
        }
        {
            char tn[72];
            int tk = tmp_path((uintptr_t)a[1], tn, sizeof(tn));
            if (tk < 0) {
                r = tk;
                break;
            }
            if (tk == 1) {
                r = fill_stat((uintptr_t)a[2], FD_DIR, &f, 3);
                break;
            }
            if (tk == 2) {
                int i = rv_tmp_lookup(tn);
                if (i < 0) {
                    r = i;
                    break;
                }
                tmp_as_file(i, &f);
                r = fill_stat((uintptr_t)a[2], FD_FILE, &f, 200000 + i);
                break;
            }
        }
        k = pty_path((uintptr_t)a[1], p);
        if (k == PTY_MASTER || k >= 0) {
            if (k >= 0 && !rv_pty_exists(k)) {
                r = -ENOENT;
                break;
            }
            r = fill_stat((uintptr_t)a[2], k == PTY_MASTER ? FD_PTM : FD_PTS, &f, k < 0 ? 0 : k);
            break;
        }
        k = dev_kind((uintptr_t)a[1]);
        if (k > 0) {
            r = fill_stat((uintptr_t)a[2], k, &f, 0);
            break;
        }
        k = lookup((uintptr_t)a[1], &f, &ino);
        r = (k < 0) ? k : fill_stat((uintptr_t)a[2], k == 0 ? FD_DIR : FD_FILE, &f, ino);
        break;
    }
    case LNX_faccessat:
    case LNX_faccessat2: {
        struct tarfs_file f;
        int64_t ino;
        int k;
        if (dev_kind((uintptr_t)a[1]) > 0) {
            r = 0;
            break;
        }
        {
            char abs[256], rel[256];
            uint32_t eino;
            if (data_path((uintptr_t)a[1], abs, rel)) {
                k = data_lookup(abs, rel, 1, &eino);
                if (k == 1) {
                    k = rv_linux_resolve(abs, &f, &ino);
                    r = (k < 0) ? k : (((long)a[2] & W_OK) ? -EROFS : 0);
                } else {
                    r = (k < 0) ? k : (((long)a[2] & W_OK) && ext2_readonly() ? -EROFS : 0);
                }
                break;
            }
        }
        {
            char tn[72];
            int tk = tmp_path((uintptr_t)a[1], tn, sizeof(tn));
            if (tk < 0) {
                r = tk;
                break;
            }
            if (tk > 0) {
                r = (tk == 1 || rv_tmp_lookup(tn) >= 0) ? 0 : -ENOENT;
                break;
            }
        }
        k = lookup((uintptr_t)a[1], &f, &ino);
        r = (k < 0) ? k : (((long)a[2] & W_OK) ? -EROFS : 0);
        break;
    }
    case 34: {                                  /* mkdirat(dirfd, path, mode) */
        char abs[256], rel[256], redir[EXT2_PATH_MAX];
        int k = data_path((uintptr_t)a[1], abs, rel);
        if (k == 1) {
            r = -EEXIST;
        } else if (k == 2) {
            DATA_RETRY(ext2_mkdir(rel, (uint16_t)(a[2] & 07777), redir));
        } else {
            char tn[72];
            int tk = tmp_path((uintptr_t)a[1], tn, sizeof(tn));
            struct tarfs_file f;
            int64_t ino;
            r = tk == 1 ? -EEXIST : tk == 2 ? -EPERM            /* /tmp の中にフォルダは作れない */
              : lookup((uintptr_t)a[1], &f, &ino) >= 0 ? -EEXIST : -EROFS;
        }
        break;
    }
    case 36: {                                  /* symlinkat(target, newdirfd, linkpath) */
        char abs[256], rel[256], redir[EXT2_PATH_MAX], target[EXT2_PATH_MAX];
        size_t n = rv_user_copy_str(target, (uintptr_t)a[0], sizeof(target));
        if (n == 0 || n >= sizeof(target) - 1) {
            r = n == 0 ? -ENOENT : -ENAMETOOLONG;
        } else if (data_path((uintptr_t)a[2], abs, rel) == 2) {
            DATA_RETRY(ext2_symlink(target, rel, redir));
        } else {
            r = -EROFS;
        }
        break;
    }
    case 37: {                                  /* linkat(olddirfd, old, newdirfd, new, flags) */
        char abs[256], rel[256], abs2[256], rel2[256], redir[EXT2_PATH_MAX];
        int d1 = data_path((uintptr_t)a[1], abs, rel);
        int d2;
        cur_dirfd = (int)(long)a[2];
        d2 = data_path((uintptr_t)a[3], abs2, rel2);
        if (d1 == 2 && d2 == 2) {
            r = ext2_link(rel, rel2, redir);
            if (r == EXT2_REDIRECT) {
                r = -40;
            }
        } else {
            r = (d1 || d2) ? -EXDEV : -EROFS;
        }
        break;
    }
    case 45: {                                  /* truncate(path, len) */
        char abs[256], rel[256];
        uint32_t eino;
        if (data_path((uintptr_t)a[0], abs, rel)) {
            int k = data_lookup(abs, rel, 1, &eino);
            r = k == 1 ? -EROFS : k < 0 ? k : ext2_truncate(eino, a[1]);
        } else {
            r = -EROFS;
        }
        break;
    }
    case 52:                                    /* fchmod(fd, mode) */
        d = get_fd(t, (long)a[0]);
        if (d && (d->type == FD_EXT || d->type == FD_EXTDIR)) {
            r = ext2_chmod((uint32_t)d->pipe, (uint16_t)(a[1] & 07777));
        } else {
            r = d ? -EROFS : -EBADF;
        }
        break;
    case 53: {                                  /* fchmodat(dirfd, path, mode, flags) */
        char abs[256], rel[256];
        uint32_t eino;
        if (data_path((uintptr_t)a[1], abs, rel)) {
            int k = data_lookup(abs, rel, 1, &eino);
            r = k == 1 ? -EROFS : k < 0 ? k : ext2_chmod(eino, (uint16_t)(a[2] & 07777));
        } else {
            char tn[72];
            r = tmp_path((uintptr_t)a[1], tn, sizeof(tn)) > 0 ? 0 : -EROFS;
        }
        break;
    }
    case 82:                                    /* fsync: /data のためてある分をディスクへ */
    case 83:                                    /* fdatasync */
    case 267:                                   /* syncfs */
        r = get_fd(t, (long)a[0]) ? (ext2_sync() == 0 ? 0 : -EIO) : -EBADF;
        break;
    case 32:                                    /* flock: 1 つのプロセスずつ動く前提で、受け付けるだけ */
        r = get_fd(t, (long)a[0]) ? 0 : -EBADF;
        break;
    case 47:                                    /* fallocate: 持たない（posix_fallocate は ftruncate に戻る） */
        r = get_fd(t, (long)a[0]) ? -95 : -EBADF;   /* EOPNOTSUPP */
        break;
    case 81:                                    /* sync */
        ext2_sync();
        r = 0;
        break;
    case 88: {                                  /* utimensat(dirfd, path, times, flags) */
        struct { int64_t sec, nsec; } ts[2];
        uint32_t at, mt, nowt = rv_now_unix(), eino = 0;
        int k = 0;
        char abs[256], rel[256];
        if (a[2] != 0) {
            if (rv_user_copy_in(ts, (uintptr_t)a[2], sizeof(ts)) != 0) {
                r = -EFAULT;
                break;
            }
        } else {
            ts[0].nsec = ts[1].nsec = (1L << 30) - 1;               /* UTIME_NOW */
        }
        if (a[1] == 0) {                        /* futimens */
            d = get_fd(t, (long)a[0]);
            if (!d) {
                r = -EBADF;
                break;
            }
            if (d->type != FD_EXT && d->type != FD_EXTDIR) {
                r = (d->type == FD_TMP) ? 0 : -EROFS;
                break;
            }
            eino = (uint32_t)d->pipe;
        } else if (data_path((uintptr_t)a[1], abs, rel)) {
            k = data_lookup(abs, rel, ((long)a[3] & AT_SYMLINK_NOFOLLOW) ? 0 : 1, &eino);
            if (k != 0) {
                r = k == 1 ? -EROFS : k;
                break;
            }
        } else {
            char tn[72];
            int tk = tmp_path((uintptr_t)a[1], tn, sizeof(tn));
            r = (tk == 1 || (tk == 2 && rv_tmp_lookup(tn) >= 0)) ? 0 : (tk == 2 ? -ENOENT : -EROFS);
            break;
        }
        {
            struct ext2_stat es;
            if (ext2_stat(eino, &es) != 0) {
                r = -EIO;
                break;
            }
            at = ts[0].nsec == (1L << 30) - 1 ? nowt : ts[0].nsec == (1L << 30) - 2 ? es.atime : (uint32_t)ts[0].sec;
            mt = ts[1].nsec == (1L << 30) - 1 ? nowt : ts[1].nsec == (1L << 30) - 2 ? es.mtime : (uint32_t)ts[1].sec;
            r = ext2_utime(eino, at, mt);
        }
        break;
    }
    case 43:                                    /* statfs(path, buf) */
    case 44: {                                  /* fstatfs(fd, buf) */
        int isdata = 0;
        if (nr == 43) {
            char abs[256], rel[256];
            isdata = data_path((uintptr_t)a[0], abs, rel) != 0;
        } else {
            d = get_fd(t, (long)a[0]);
            if (!d) {
                r = -EBADF;
                break;
            }
            isdata = d->type == FD_EXT || d->type == FD_EXTDIR;
        }
        {
            struct { int64_t type, bsize; uint64_t blocks, bfree, bavail, files, ffree; int32_t fsid[2];
                     int64_t namelen, frsize, flags, spare[4]; } sf;
            unsigned z;
            for (z = 0; z < sizeof(sf); z++) {
                ((uint8_t *)&sf)[z] = 0;
            }
            if (isdata) {
                uint64_t bsz, bl, bf, fi, ff;
                ext2_statfs(&bsz, &bl, &bf, &fi, &ff);
                sf.type = 0xEF53;
                sf.bsize = sf.frsize = (int64_t)bsz;
                sf.blocks = bl;
                sf.bfree = sf.bavail = bf;
                sf.files = fi;
                sf.ffree = ff;
                sf.flags = ext2_readonly() ? 1 : 0;
            } else {
                sf.type = 0x858458f6;           /* 読み取り専用のディスク（中身の数は出さない） */
                sf.bsize = sf.frsize = 512;
                sf.flags = 1;                   /* ST_RDONLY */
            }
            sf.namelen = 255;
            r = rv_user_copy_out((uintptr_t)a[1], &sf, sizeof(sf)) == 0 ? 0 : -EFAULT;
        }
        break;
    }
    case LNX_getdents64:
        d = get_fd(t, (long)a[0]);
        r = d ? do_getdents64(d, (uintptr_t)a[1], (size_t)a[2]) : -EBADF;
        break;
    case LNX_getcwd: {
        const char *c = cwd_of(cur_slot);
        size_t n = 0;
        while (c[n]) {
            n++;
        }
        if ((size_t)a[1] < n + 1) {
            r = -34;                            /* ERANGE */
        } else {
            r = rv_user_copy_out((uintptr_t)a[0], c, n + 1) == 0 ? (long)(n + 1) : -EFAULT;
        }
        break;
    }
    case LNX_chdir:
    case 50: {                                  /* fchdir */
        struct tarfs_file f;
        int64_t ino;
        char abs[256], rel[256];
        int k, n;
        if (nr == 50) {
            const char *b;
            d = get_fd(t, (long)a[0]);
            b = d ? fd_dir_path(d) : NULL;
            if (d && d->type == FD_DIR && d->f.name[0] == 0) {
                b = "/";
            }
            if (!b) {
                r = d ? -ENOTDIR : -EBADF;
                break;
            }
            for (n = 0; b[n] && n < 255; n++) {
                abs[n] = b[n];
            }
            abs[n] = 0;
        } else {
            n = upath_norm((uintptr_t)a[0], abs, sizeof(abs));
            if (n < 0) {
                r = n;
                break;
            }
        }
        if (n >= (int)sizeof(cwds[0])) {
            r = -ENAMETOOLONG;
            break;
        }
        k = data_path_str(abs, rel, sizeof(rel));
        if (k) {
            uint32_t eino;
            struct ext2_stat es;
            k = data_lookup(abs, rel, 1, &eino);
            if (k == 1) {
                r = -EXDEV;
                break;
            }
            if (k < 0) {
                r = k;
                break;
            }
            if (ext2_stat(eino, &es) != 0 || (es.mode & 0xF000) != 0x4000) {
                r = -ENOTDIR;
                break;
            }
        } else if (tmp_path_str(abs, rel, sizeof(rel)) == 2) {
            r = -ENOTDIR;                       /* /tmp の中にフォルダはない */
            break;
        } else if (tmp_path_str(abs, rel, sizeof(rel)) != 1) {
            k = rv_linux_resolve(abs, &f, &ino);
            if (k != 0) {
                r = (k < 0) ? k : -ENOTDIR;
                break;
            }
        }
        if (cur_slot >= 0 && cur_slot < SLOTS) {
            for (k = 0; abs[k]; k++) {
                cwds[cur_slot][k] = abs[k];
            }
            cwds[cur_slot][k] = 0;
        }
        r = 0;
        break;
    }
    case LNX_readlinkat: {
        struct tarfs_file f;
        int64_t ino;
        int k;
        {
            char abs[256], rel[256], redir[EXT2_PATH_MAX], lb[EXT2_PATH_MAX];
            if (data_path((uintptr_t)a[1], abs, rel)) {
                long n;
                int dd;
                for (dd = 0; dd < 8; dd++) {
                    n = ext2_readlink(rel, lb, sizeof(lb), redir);
                    if (n != EXT2_REDIRECT) {
                        break;
                    }
                    if (data_redirect(redir, abs, rel) <= 0) {
                        n = -EXDEV;
                        break;
                    }
                }
                if (n >= 0) {
                    size_t m = (size_t)n < (size_t)a[3] ? (size_t)n : (size_t)a[3];
                    n = rv_user_copy_out((uintptr_t)a[2], lb, m) == 0 ? (long)m : -EFAULT;
                }
                r = n == EXT2_REDIRECT ? -40 : n;
                break;
            }
        }
        {
            /* /proc/self/fd/N → その fd の名前（musl の ttyname がこれで端末の名前を知る） */
            char path[40], name[110];
            const char *pre = "/proc/self/fd/";
            int i, n = 0, ok = 1, len = 0;
            if (rv_user_copy_str(path, (uintptr_t)a[1], sizeof(path)) != 0) {
                for (i = 0; pre[i]; i++) {
                    if (path[i] != pre[i]) {
                        ok = 0;
                        break;
                    }
                }
                if (ok && path[i] >= '0' && path[i] <= '9') {
                    struct lfd *x;
                    for (; path[i] >= '0' && path[i] <= '9'; i++) {
                        n = n * 10 + (path[i] - '0');
                    }
                    x = (path[i] == 0) ? get_fd(t, n) : NULL;
                    if (x == NULL) {
                        r = -ENOENT;
                        break;
                    }
                    if (x->type == FD_PTS) {
                        const char *b = "/dev/pts/";
                        for (len = 0; b[len]; len++) name[len] = b[len];
                        if (x->pipe >= 10) name[len++] = (char)('0' + x->pipe / 10);
                        name[len++] = (char)('0' + x->pipe % 10);
                    } else {
                        const char *b = x->type == FD_PTM ? "/dev/ptmx" : x->type == FD_CONSOLE ? "/dev/console"
                                      : x->type == FD_NULL ? "/dev/null" : x->type == FD_PIPE_R || x->type == FD_PIPE_W
                                      ? "pipe:[1]" : x->type == FD_SOCK ? "socket:[1]" : x->f.name;
                        if (x->type == FD_TMP) {
                            tmp_as_file(x->pipe, &x->f);
                            b = x->f.name;
                        }
                        if (x->type == FD_FILE || x->type == FD_DIR || x->type == FD_TMP || x->type == FD_TMPDIR) {
                            name[len++] = '/';
                        }
                        for (i = 0; b[i] && len < 100; i++) name[len++] = b[i];
                    }
                    if ((size_t)len > (size_t)a[3]) {
                        len = (int)a[3];
                    }
                    r = rv_user_copy_out((uintptr_t)a[2], name, (size_t)len) == 0 ? len : -EFAULT;
                    break;
                }
            }
        }
        k = lookup((uintptr_t)a[1], &f, &ino);
        r = (k < 0) ? k : -EINVAL;              /* シンボリックリンクはない */
        break;
    }
    case LNX_fcntl: {
        long cmd = (long)a[1];
        int fd;
        d = get_fd(t, (long)a[0]);
        if (d == NULL) {
            r = -EBADF;
            break;
        }
        if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
            fd = alloc_fd(t, (int)a[2]);
            if (fd >= 0) {
                t[fd] = *d;
                t[fd].cloexec = (cmd == F_DUPFD_CLOEXEC);
                fd_ref(&t[fd]);
            }
            r = fd;
        } else if (cmd == F_GETFD) {
            r = d->cloexec ? FD_CLOEXEC : 0;
        } else if (cmd == F_SETFD) {
            d->cloexec = ((long)a[2] & FD_CLOEXEC) ? 1 : 0;
            r = 0;
        } else if (cmd == F_SETFL) {
            d->nonblock = ((long)a[2] & 04000) != 0;                      /* O_NONBLOCK */
            if (d->type == FD_SOCK) {
                rv_sock_set_nonblock(d->pipe, d->nonblock);
            }
            r = 0;
        } else if (cmd == 1033 || cmd == 1034) {    /* F_ADD_SEALS・F_GET_SEALS（memfd）: 受け付けるだけ */
            r = (d->type == FD_TMP) ? 0 : -EINVAL;
        } else if (cmd == F_GETFL) {
            r = (d->type == FD_TMP) ? d->acc | (d->append ? O_APPEND : 0) : (d->type == FD_CONSOLE || d->type == FD_SOCK || d->type == FD_USOCK || d->type == FD_EVENTFD || d->type == FD_PTM || d->type == FD_PTS) ? 2 /* O_RDWR */ : (d->type == FD_DIR ? O_DIRECTORY
                : (d->type == FD_PIPE_W ? 1 /* O_WRONLY */ : 0));
            if (d->nonblock || (d->type == FD_SOCK && rv_sock_get_nonblock(d->pipe))) {
                r |= 04000;
            }
        } else {
            r = -EINVAL;
        }
        break;
    }
    case LNX_dup: {
        int fd;
        d = get_fd(t, (long)a[0]);
        if (d == NULL) {
            r = -EBADF;
            break;
        }
        fd = alloc_fd(t, 0);
        if (fd >= 0) {
            t[fd] = *d;
            t[fd].cloexec = 0;
            fd_ref(&t[fd]);
        }
        r = fd;
        break;
    }
    case LNX_dup3: {
        long nfd = (long)a[1];
        d = get_fd(t, (long)a[0]);
        if (d == NULL || nfd < 0 || nfd >= NFD) {
            r = -EBADF;
        } else if (nfd == (long)a[0]) {
            r = -EINVAL;
        } else {
            struct lfd copy = *d;
            if (t[nfd].type != FD_FREE) {
                fd_close(&t[nfd]);              /* 先にあった fd は閉じる */
            }
            t[nfd] = copy;
            t[nfd].cloexec = ((long)a[2] & O_CLOEXEC) ? 1 : 0;
            fd_ref(&t[nfd]);
            r = nfd;
        }
        break;
    }
    case LNX_ioctl:
        d = get_fd(t, (long)a[0]);
        if (d == NULL) {
            r = -EBADF;
        } else if (d->type == FD_INPUT && rv_evdev_ioctl(d->pipe, (unsigned long)a[1], (uintptr_t)a[2], &r)) {
            /* EVIOC…（linux_evfd.c） */
        } else if (d->type == FD_USOCK && a[1] == 0x541B) {         /* FIONREAD: まだ読んでいないバイトの数は返さない */
            int32_t n = (rv_usock_events(d->pipe) & 1) ? 1 : 0;
            r = rv_user_copy_out((uintptr_t)a[2], &n, sizeof(n)) == 0 ? 0 : -EFAULT;
        } else if (d->type == FD_USOCK && a[1] == 0x5421) {         /* FIONBIO */
            int32_t on = 0;
            rv_user_copy_in(&on, (uintptr_t)a[2], sizeof(on));
            d->nonblock = on != 0;
            r = 0;
        } else if ((d->type == FD_PTM || d->type == FD_PTS)
                   && !rv_pty_ioctl(d->pipe, d->type == FD_PTM, (unsigned long)a[1], (uintptr_t)a[2], &r)) {
            if (d->type == FD_PTS && a[1] == 0x540E) {              /* TIOCSCTTY */
                rv_linux_set_ctty(p, d->pipe, 1);
            }
        } else if (d->type == FD_PTM || d->type == FD_PTS) {
            r = -ENOTTY;
        } else if (d->type == FD_BLK && (a[1] == 0x80081272 || a[1] == 0x1268 || a[1] == 0x1260)) {
            /* BLKGETSIZE64（バイト）・BLKSSZGET（ブロックの大きさ）・BLKGETSIZE（512 バイトの数） */
            uint64_t v = a[1] == 0x80081272 ? rv_usbms_size() : a[1] == 0x1268 ? rv_usbms_block()
                       : rv_usbms_size() / 512;
            size_t n = a[1] == 0x1268 ? 4 : 8;
            uint32_t v32 = (uint32_t)v;
            r = rv_user_copy_out((uintptr_t)a[2], n == 4 ? (const void *)&v32 : (const void *)&v, n) == 0 ? 0 : -EFAULT;
        } else if (d->type == FD_IO && (a[1] >= 0x4f01 && a[1] <= 0x4f04)) {
            unsigned char q[56];                    /* struct io_req（kernel/io/iomgr.c） */
            if (a[1] == 0x4f04) {                   /* IOC_SAFE: 引数は使わない */
                r = iomgr_ioctl((unsigned long)a[1], q);
            } else if (rv_user_copy_in(q, (uintptr_t)a[2], sizeof(q)) != 0) {
                r = -EFAULT;
            } else {
                r = iomgr_ioctl((unsigned long)a[1], q);
                if (r == 0 && rv_user_copy_out((uintptr_t)a[2], q, sizeof(q)) != 0) {
                    r = -EFAULT;
                }
            }
        } else if (d->type == FD_FB && (a[1] == 0x4600 || a[1] == 0x4602)) {
            /* FBIOGET_VSCREENINFO・FBIOGET_FSCREENINFO（使うところだけ埋める） */
            uint32_t v[40];
            unsigned w, h;
            unsigned z;
            rv_fb_geometry(&w, &h);
            for (z = 0; z < 40; z++) {
                v[z] = 0;
            }
            if (a[1] == 0x4600) {
                v[0] = w; v[1] = h; v[2] = w; v[3] = h;     /* xres yres xres_virtual yres_virtual */
                v[6] = 32;                                  /* bits_per_pixel */
                v[8] = 16; v[9] = 8;                        /* red: offset length */
                v[11] = 8; v[12] = 8;                       /* green */
                v[14] = 0; v[15] = 8;                       /* blue */
                r = rv_user_copy_out((uintptr_t)a[2], v, 160) == 0 ? 0 : -EFAULT;
            } else {
                {
                    const char *id = "virtio-gpu";      /* id[16] */
                    for (z = 0; id[z]; z++) {
                        ((char *)v)[z] = id[z];
                    }
                }
                v[6] = (uint32_t)rv_fb_size();              /* smem_len（offset 24） */
                v[9] = 2;                                   /* visual: TRUECOLOR（offset 36） */
                v[12] = w * 4;                              /* line_length（offset 48） */
                r = rv_user_copy_out((uintptr_t)a[2], v, 80) == 0 ? 0 : -EFAULT;
            }
        } else if (d->type == FD_SOCK && a[1] == 0x541B) {          /* FIONREAD */
            int32_t n = rv_sock_rx_avail(d->pipe);
            r = rv_user_copy_out((uintptr_t)a[2], &n, sizeof(n)) == 0 ? 0 : -EFAULT;
        } else if (d->type == FD_SOCK && a[1] == 0x5421) {          /* FIONBIO */
            int32_t on = 0;
            rv_user_copy_in(&on, (uintptr_t)a[2], sizeof(on));
            rv_sock_set_nonblock(d->pipe, on != 0);
            r = 0;
        } else if (d->type != FD_CONSOLE) {
            r = -ENOTTY;
        } else if (a[1] == TIOCGWINSZ) {
            uint16_t ws[4] = { 24, 80, 0, 0 };
            r = rv_user_copy_out((uintptr_t)a[2], ws, sizeof(ws)) == 0 ? 0 : -EFAULT;
        } else if (a[1] == TCGETS) {
            r = rv_user_copy_out((uintptr_t)a[2], &tio, sizeof(tio)) == 0 ? 0 : -EFAULT;
        } else if (a[1] == TCSETS || a[1] == TCSETSW || a[1] == TCSETSF) {
            struct lnx_termios nt;
            if (rv_user_copy_in(&nt, (uintptr_t)a[2], sizeof(nt)) != 0) {
                r = -EFAULT;
            } else {
                tio = nt;
                r = 0;
            }
        } else if (a[1] == TIOCSPGRP) {
            int32_t pg;
            if (rv_user_copy_in(&pg, (uintptr_t)a[2], sizeof(pg)) != 0) {
                r = -EFAULT;
            } else {
                fg_pgrp = pg;
                r = 0;
            }
        } else if (a[1] == TIOCGPGRP) {
            int32_t pg = fg_pgrp > 0 ? fg_pgrp : rv_linux_my_pgid(p);
            r = rv_user_copy_out((uintptr_t)a[2], &pg, sizeof(pg)) == 0 ? 0 : -EFAULT;
        } else {
            r = -EINVAL;
        }
        break;
    case LNX_ppoll: {
        /* struct pollfd { int fd; short events; short revents; } を見て、
         * 読める・書けるものがあればすぐ返す。なければ 10ms おきに見直す（時間切れまで） */
        struct pfd { int32_t fd; int16_t events, revents; };
        struct pfd pf[16];
        long nfds = (long)a[1];
        int64_t waited = 0, limit = -1;
        long k;
        if (nfds < 0 || nfds > 16) {
            r = -EINVAL;
            break;
        }
        if (a[2] != 0) {
            int64_t ts[2];
            if (rv_user_copy_in(ts, (uintptr_t)a[2], sizeof(ts)) != 0) {
                r = -EFAULT;
                break;
            }
            limit = ts[0] * 1000 + ts[1] / 1000000;
        }
        if (nfds > 0 && rv_user_copy_in(pf, (uintptr_t)a[0], (size_t)nfds * sizeof(pf[0])) != 0) {
            r = -EFAULT;
            break;
        }
        for (;;) {
            long ready = 0;
            for (k = 0; k < nfds; k++) {
                int16_t ev;
                pf[k].revents = 0;
                if (pf[k].fd < 0) {
                    continue;
                }
                ev = fd_events(get_fd(t, pf[k].fd));
                pf[k].revents = ev & (pf[k].events | 0x8 | 0x10 | 0x20);
                if (pf[k].revents) {
                    ready++;
                }
            }
            if (ready > 0 || (limit >= 0 && waited >= limit)) {
                if (nfds > 0 && rv_user_copy_out((uintptr_t)a[0], pf, (size_t)nfds * sizeof(pf[0])) != 0) {
                    r = -EFAULT;
                } else {
                    r = ready;
                }
                break;
            }
            if (rv_linux_sig_interrupt()) {
                r = -EINTR;
                break;
            }
            tk_dly_tsk(10);
            waited += 10;
        }
        break;
    }
    case 198:                                   /* socket */
        if ((long)a[0] == 1) {                  /* AF_UNIX（linux_unix.c） */
            int si = rv_usock_create((int)a[1], rv_linux_pid_of(p)), fd;
            if (si < 0) {
                r = si;
                break;
            }
            fd = alloc_fd(t, 0);
            if (fd < 0) {
                rv_usock_unref(si);
                r = fd;
                break;
            }
            t[fd].type = FD_USOCK;
            t[fd].pipe = si;
            t[fd].cloexec = ((long)a[1] & O_CLOEXEC) ? 1 : 0;
            t[fd].nonblock = ((long)a[1] & 04000) ? 1 : 0;
            r = fd;
            break;
        }
        /* fall through */
    case 0x10000 + 198: {
        int si = rv_sock_create((long)a[0], (long)a[1], (long)a[2]), fd;
        if (si >= 0) {
            rv_linux_mark_net(p);               /* ネットワークを使うプロセスは機器を動かせない（安全の決まり 6） */
        }
        if (si < 0) {
            r = si;
            break;
        }
        fd = alloc_fd(t, 0);
        if (fd < 0) {
            rv_sock_unref(si);
            r = fd;
            break;
        }
        t[fd].type = FD_SOCK;
        t[fd].pipe = si;
        t[fd].cloexec = ((long)a[1] & O_CLOEXEC) ? 1 : 0;
        r = fd;
        break;
    }
    case 202:                                   /* accept */
    case 242: {                                 /* accept4 */
        int si, fd;
        d = get_fd(t, (long)a[0]);
        if (d != NULL && d->type == FD_USOCK) {
            long fl = nr == 242 ? (long)a[3] : 0;
            si = (int)rv_usock_accept(d->pipe, d->nonblock, (uintptr_t)a[1], (uintptr_t)a[2]);
            if (si < 0) {
                r = si;
                break;
            }
            fd = alloc_fd(t, 0);
            if (fd < 0) {
                rv_usock_unref(si);
                r = fd;
                break;
            }
            t[fd].type = FD_USOCK;
            t[fd].pipe = si;
            t[fd].cloexec = (fl & O_CLOEXEC) ? 1 : 0;
            t[fd].nonblock = (fl & 04000) ? 1 : 0;
            r = fd;
            break;
        }
        if (d == NULL || d->type != FD_SOCK) {
            r = d ? -88 : -EBADF;
            break;
        }
        si = (int)rv_sock_accept(d->pipe, (uintptr_t)a[1], (uintptr_t)a[2], nr == 242 ? (long)a[3] : 0);
        if (si < 0) {
            r = si;
            break;
        }
        fd = alloc_fd(t, 0);
        if (fd < 0) {
            rv_sock_unref(si);
            r = fd;
            break;
        }
        t[fd].type = FD_SOCK;
        t[fd].pipe = si;
        t[fd].cloexec = (nr == 242 && ((long)a[3] & O_CLOEXEC)) ? 1 : 0;
        r = fd;
        break;
    }
    case 199: {                                 /* socketpair（AF_UNIX のみ） */
        int sa, sb, fa, fb;
        int32_t pair[2];
        if ((long)a[0] != 1) {
            r = -97;                            /* EAFNOSUPPORT */
            break;
        }
        r = rv_usock_pair((int)a[1], rv_linux_pid_of(p), &sa, &sb);
        if (r < 0) {
            break;
        }
        fa = alloc_fd(t, 0);
        if (fa >= 0) {
            t[fa].type = FD_USOCK;              /* 仮に確保 */
            t[fa].pipe = sa;
        }
        fb = fa >= 0 ? alloc_fd(t, 0) : -EMFILE;
        if (fb < 0) {
            if (fa >= 0) {
                t[fa].type = FD_FREE;
            }
            rv_usock_unref(sa);
            rv_usock_unref(sb);
            r = -EMFILE;
            break;
        }
        t[fb].type = FD_USOCK;
        t[fb].pipe = sb;
        t[fa].cloexec = t[fb].cloexec = ((long)a[1] & O_CLOEXEC) ? 1 : 0;
        t[fa].nonblock = t[fb].nonblock = ((long)a[1] & 04000) ? 1 : 0;
        pair[0] = fa;
        pair[1] = fb;
        if (rv_user_copy_out((uintptr_t)a[3], pair, sizeof(pair)) != 0) {
            fd_close(&t[fa]);
            fd_close(&t[fb]);
            r = -EFAULT;
            break;
        }
        r = 0;
        break;
    }
    case 200: case 201: case 203: case 204: case 205: case 206: case 207:
    case 208: case 209: case 210: case 211: case 212: {
        d = get_fd(t, (long)a[0]);
        if (d != NULL && d->type == FD_USOCK) {
            r = usock_call(p, t, d, nr, a);
            break;
        }
        if (d == NULL || d->type != FD_SOCK) {
            r = d ? -88 : -EBADF;               /* ENOTSOCK */
            break;
        }
        switch (nr) {
        case 200: r = rv_sock_bind(d->pipe, (uintptr_t)a[1], (long)a[2]); break;
        case 201: r = rv_sock_listen(d->pipe, (long)a[1]); break;
        case 203: r = rv_sock_connect(d->pipe, (uintptr_t)a[1], (long)a[2]); break;
        case 204: r = rv_sock_name(d->pipe, (uintptr_t)a[1], (uintptr_t)a[2], 0); break;
        case 205: r = rv_sock_name(d->pipe, (uintptr_t)a[1], (uintptr_t)a[2], 1); break;
        case 206: r = rv_sock_send(d->pipe, (uintptr_t)a[1], (size_t)a[2], (long)a[3], (uintptr_t)a[4], (long)a[5]); break;
        case 207: r = rv_sock_recv(d->pipe, (uintptr_t)a[1], (size_t)a[2], (long)a[3], (uintptr_t)a[4], (uintptr_t)a[5]); break;
        case 208: r = 0; break;                 /* setsockopt: 受け付けるだけ */
        case 209: r = rv_sock_getsockopt(d->pipe, (long)a[1], (long)a[2], (uintptr_t)a[3], (uintptr_t)a[4]); break;
        case 210: r = rv_sock_shutdown(d->pipe, (long)a[1]); break;
        default: {                              /* sendmsg / recvmsg: 1 つ目の iovec と相手のアドレスだけ */
            uint64_t mh[7];                     /* name, namelen, iov, iovlen, control, controllen, flags */
            uint64_t iov[2];
            if (rv_user_copy_in(mh, (uintptr_t)a[1], sizeof(mh)) != 0
                || ((uint32_t)mh[3] > 0 && rv_user_copy_in(iov, (uintptr_t)mh[2], sizeof(iov)) != 0)) {
                r = -EFAULT;
                break;
            }
            if ((uint32_t)mh[3] == 0) {
                iov[0] = iov[1] = 0;
            }
            if (nr == 211) {
                r = rv_sock_send(d->pipe, (uintptr_t)iov[0], (size_t)iov[1], (long)a[2],
                                 (uintptr_t)mh[0], (long)(uint32_t)mh[1]);
            } else {
                r = rv_sock_recv(d->pipe, (uintptr_t)iov[0], (size_t)iov[1], (long)a[2],
                                 (uintptr_t)mh[0], mh[0] ? (uintptr_t)a[1] + 8 : 0);
                if (r >= 0) {
                    uint64_t z = 0;                  /* msg_controllen = 0, msg_flags = 0 */
                    rv_user_copy_out((uintptr_t)a[1] + 40, &z, 8);
                }
            }
            break;
        }
        }
        break;
    }
    case 20:                                    /* epoll_create1 */
    case 19:                                    /* eventfd2 */
    case 85: {                                  /* timerfd_create */
        int oi, fd;
        if (nr == 20) {
            for (oi = 0; oi < NEPOLL && eps[oi].used; oi++) {
            }
            if (oi >= NEPOLL) {
                r = -EMFILE;
                break;
            }
            eps[oi].used = 1;
            eps[oi].refs = 1;
            eps[oi].n = 0;
        } else if (nr == 19) {
            oi = rv_eventfd_create((unsigned)a[0], (int)a[1]);
        } else {
            oi = rv_timerfd_create((int)a[0]);
        }
        if (oi < 0) {
            r = oi;
            break;
        }
        fd = alloc_fd(t, 0);
        if (fd < 0) {
            if (nr == 20) eps[oi].used = 0;
            else if (nr == 19) rv_eventfd_unref(oi);
            else rv_timerfd_unref(oi);
            r = fd;
            break;
        }
        t[fd].type = nr == 20 ? FD_EPOLL : nr == 19 ? FD_EVENTFD : FD_TIMERFD;
        t[fd].pipe = oi;
        t[fd].ino = (int64_t)a[0];              /* timerfd: 時計の種類 */
        /* EPOLL_CLOEXEC・EFD_CLOEXEC・TFD_CLOEXEC は O_CLOEXEC、EFD_NONBLOCK・TFD_NONBLOCK は O_NONBLOCK */
        t[fd].cloexec = ((long)(nr == 20 ? a[0] : a[1]) & O_CLOEXEC) ? 1 : 0;
        t[fd].nonblock = (nr != 20 && ((long)a[1] & 04000)) ? 1 : 0;
        r = fd;
        break;
    }
    case 86:                                    /* timerfd_settime */
    case 87:                                    /* timerfd_gettime */
        d = get_fd(t, (long)a[0]);
        if (d == NULL || d->type != FD_TIMERFD) {
            r = d ? -EINVAL : -EBADF;
        } else if (nr == 86) {
            r = rv_timerfd_settime(d->pipe, (int)a[1], (uintptr_t)a[2], (uintptr_t)a[3], (int)d->ino);
        } else {
            r = rv_timerfd_gettime(d->pipe, (uintptr_t)a[1]);
        }
        break;
    case 21:                                    /* epoll_ctl(epfd, op, fd, event) */
        r = epoll_ctl(t, (long)a[0], (long)a[1], (long)a[2], (uintptr_t)a[3]);
        break;
    case 22:                                    /* epoll_pwait(epfd, events, max, timeout_ms, sigmask) */
    case 441: {                                 /* epoll_pwait2(epfd, events, max, timespec*, sigmask) */
        int64_t limit = (long)(int32_t)a[3];
        if (nr == 441) {
            int64_t ts[2];
            limit = -1;
            if (a[3] != 0) {
                if (rv_user_copy_in(ts, (uintptr_t)a[3], sizeof(ts)) != 0) {
                    r = -EFAULT;
                    break;
                }
                limit = ts[0] * 1000 + ts[1] / 1000000;
            }
        }
        r = epoll_wait(t, (long)a[0], (uintptr_t)a[1], (long)a[2], limit);
        break;
    }
    case LNX_pselect6:
        r = do_pselect(t, (long)a[0], (uintptr_t)a[1], (uintptr_t)a[2], (uintptr_t)a[3], (uintptr_t)a[4]);
        break;
    case LNX_pipe2: {
        int pi, fr, fw;
        int32_t pair[2];
        for (pi = 0; pi < NPIPE && pipes[pi].used; pi++) {
        }
        if (pi >= NPIPE) {
            r = -EMFILE;
            break;
        }
        fr = alloc_fd(t, 0);
        if (fr < 0) {
            r = fr;
            break;
        }
        t[fr].type = FD_PIPE_R;                 /* 仮に確保（次の alloc_fd で選ばれないように） */
        fw = alloc_fd(t, 0);
        if (fw < 0) {
            t[fr].type = FD_FREE;
            r = fw;
            break;
        }
        pipes[pi].used = 1;
        pipes[pi].head = pipes[pi].tail = pipes[pi].count = 0;
        pipes[pi].readers = 1;
        pipes[pi].writers = 1;
        pipes[pi].rwait = pipes[pi].wwait = 0;
        t[fr].pipe = pi;
        t[fr].cloexec = ((long)a[1] & O_CLOEXEC) ? 1 : 0;
        t[fw].type = FD_PIPE_W;
        t[fw].pipe = pi;
        t[fw].cloexec = t[fr].cloexec;
        t[fr].nonblock = t[fw].nonblock = ((long)a[1] & 04000) ? 1 : 0;    /* O_NONBLOCK */
        pair[0] = fr;
        pair[1] = fw;
        if (rv_user_copy_out((uintptr_t)a[0], pair, sizeof(pair)) != 0) {
            fd_close(&t[fr]);
            fd_close(&t[fw]);
            r = -EFAULT;
            break;
        }
        r = 0;
        break;
    }
    default:
        return 0;
    }
    *ret = r;
    return 1;
}

/* ---- linux_mm.c（mmap・memfd）から ---- */
extern int rv_tmp_create_anon(void);

/* fd の種類: 0 なし・写せない、1 メモリの上のファイル（共有で写せる。*idx に番号）、2 読めるファイル。*acc に開いた形 */
int rv_linux_fd_kind(struct rv_uproc *p, long fd, int *idx, int *acc)
{
    struct lfd *d = get_fd(table(p), fd);
    if (d == NULL) {
        return 0;
    }
    if (d->type == FD_TMP) {
        *idx = d->pipe;
        *acc = d->acc;
        return 1;
    }
    if (d->type == FD_FILE || d->type == FD_EXT) {
        *acc = 0;
        return 2;
    }
    return 0;
}

/* ファイルの off から len バイトをユーザーの ubuf へ（ファイルの終わりで止まる）。読めたバイト数か負のエラー */
long rv_linux_fd_pread(struct rv_uproc *p, long fd, uint64_t off, uintptr_t ubuf, size_t len)
{
    struct lfd *d = get_fd(table(p), fd);
    if (d == NULL) {
        return -EBADF;
    }
    if (d->type == FD_TMP) {
        return rv_tmp_read(d->pipe, off, ubuf, len);
    }
    if (d->type == FD_EXT) {
        return ext_rw(d, off, ubuf, len, 0);
    }
    if (d->type == FD_FILE) {
        return file_read_at(d, off, ubuf, len);
    }
    return -ENODEV;
}

/* memfd_create: メモリの上の名前のないファイルを fd にする。fd か負のエラー */
int rv_linux_fd_install_tmp(struct rv_uproc *p, int idx, int cloexec)
{
    struct lfd *t = table(p);
    int fd;
    if (t == NULL) {
        return -EBADF;
    }
    fd = alloc_fd(t, 0);
    if (fd < 0) {
        return fd;
    }
    t[fd].type = FD_TMP;
    t[fd].pipe = idx;
    t[fd].acc = 2;
    t[fd].append = 0;
    t[fd].nonblock = 0;
    t[fd].cloexec = cloexec ? 1 : 0;
    t[fd].off = 0;
    t[fd].ino = 200000 + idx;
    tmp_as_file(idx, &t[fd].f);
    rv_tmp_ref(idx);
    return fd;
}
