/*
 * linux_syscall.c — RISC-V の Linux 互換のシステムコール（S-mode + MMU のみ）
 *
 *   Tamazusa の Linux 互換層を RISC-V 上に作り直す、その第 1 段。
 *   RISC-V の Linux の番号（asm-generic）で受け付ける。a7 = 番号、a0〜a5 = 引数、戻り値は a0
 *   （失敗は -errno）。x86_64 版の syscall.c は、動きを確かめる手本として使う。
 *
 *   ファイルと入出力（read / write / openat / fstat / getdents64 など）は linux_fs.c。
 *   ここで扱うもの:
 *     exit / exit_group / set_tid_address / set_robust_list / futex（最低限）
 *     brk / mmap（無名のみ）/ munmap / mprotect / madvise
 *     getpid ほか ID / uname / clock_gettime / nanosleep / sched_yield / getrandom / prlimit64
 *     rt_sigaction / rt_sigprocmask（受け付けるだけ）
 *   それ以外は「未実装」と 1 回表示して -ENOSYS を返す。
 */
#include <stdint.h>
#include <stddef.h>
#include <rv_priv.h>

struct rv_uproc;
extern struct rv_uproc *rv_uproc_current(void);
extern int  rv_uproc_is_linux(const struct rv_uproc *p);
extern int  rv_uproc_pid(const struct rv_uproc *p);
extern uintptr_t rv_linux_brk(struct rv_uproc *p, uintptr_t addr);
extern uintptr_t rv_linux_mmap_anon(struct rv_uproc *p, size_t len);
extern int  rv_linux_munmap(struct rv_uproc *p, uintptr_t addr, size_t len);
extern int  rv_user_copy_in(void *dst, uintptr_t uva, size_t len);
extern int  rv_user_copy_out(uintptr_t uva, const void *src, size_t len);
extern void rv_console_puts(const char *s);
extern int  rv_console_getc(void);
extern int  tk_dly_tsk(unsigned long dlytim);
extern void uart_puts(const char *s);
extern int  rv_linux_fs_syscall(struct rv_uproc *p, long nr, const uint64_t *a, long *ret);
extern int  rv_linux_mm_syscall(struct rv_uproc *p, long nr, const uint64_t *a, long *ret);
extern int  rv_linux_cred_syscall(struct rv_uproc *p, long nr, const uint64_t *a, long *ret);
extern long rv_linux_clone(struct rv_uproc *u, const uint64_t *f, unsigned long flags, uintptr_t newsp);
extern long rv_linux_execve(struct rv_uproc *u, uint64_t *f, uintptr_t upath, uintptr_t uargv, uintptr_t uenvp);
extern long rv_linux_wait4(struct rv_uproc *u, long pid, uintptr_t ustatus, long options);
extern int  rv_linux_getppid(struct rv_uproc *u);

/* RISC-V の Linux のシステムコール番号（asm-generic） */
#define LNX_ioctl           29
#define LNX_close           57
#define LNX_read            63
#define LNX_write           64
#define LNX_writev          66
#define LNX_exit            93
#define LNX_exit_group      94
#define LNX_set_tid_address 96
#define LNX_futex           98
#define LNX_set_robust_list 99
#define LNX_nanosleep       101
#define LNX_clock_gettime   113
#define LNX_sched_yield     124
#define LNX_rt_sigaction    134
#define LNX_rt_sigprocmask  135
#define LNX_uname           160
#define LNX_getpid          172
#define LNX_getppid         173
#define LNX_getuid          174
#define LNX_geteuid         175
#define LNX_getgid          176
#define LNX_getegid         177
#define LNX_gettid          178
#define LNX_brk             214
#define LNX_munmap          215
#define LNX_mmap            222
#define LNX_mprotect        226
#define LNX_madvise         233
#define LNX_prlimit64       261
#define LNX_getrandom       278
#define LNX_clone           220
#define LNX_execve          221
#define LNX_wait4           260
#define LNX_prctl           167
#define LNX_umask           166
#define LNX_setpgid         154
#define LNX_getpgid         155
#define LNX_setsid          157
#define LNX_sigaltstack     132
#define LNX_kill            129
#define LNX_tkill           130
#define LNX_tgkill          131
#define LNX_rt_sigsuspend   133
#define LNX_rt_sigpending   136
#define LNX_rt_sigreturn    139
#define LNX_getsid          156

#define EPERM   1
#define EBADF   9
#define EAGAIN  11
#define ENOMEM  12
#define EFAULT  14
#define EINVAL  22
#define ENOTTY  25
#define ENOSYS  38

#define TIOCGWINSZ  0x5413
#define TCGETS      0x5401

#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20

extern long rv_linux_sigreturn(struct rv_uproc *u, uint64_t *f);
extern long rv_linux_sigaction(struct rv_uproc *u, long sig, uintptr_t uact, uintptr_t uoact, long size);
extern long rv_linux_sigprocmask(struct rv_uproc *u, long how, uintptr_t uset, uintptr_t uoset, long size);
extern long rv_linux_sigpending(struct rv_uproc *u, uintptr_t uset);
extern long rv_linux_sigsuspend(struct rv_uproc *u, uintptr_t umask, long size);
extern long rv_linux_kill(struct rv_uproc *u, long pid, long sig);
extern long rv_linux_setpgid(struct rv_uproc *u, long pid, long pgid);
extern long rv_linux_getpgid(struct rv_uproc *u, long pid);
extern long rv_linux_getsid(struct rv_uproc *u, long pid);
extern long rv_linux_setsid(struct rv_uproc *u);
extern void rv_linux_sys_enter(struct rv_uproc *u, const uint64_t *f);
extern void rv_linux_sys_leave(struct rv_uproc *u, long nr, long ret);
extern int  rv_linux_sig_interrupt(void);
extern long rv_linux_setitimer(struct rv_uproc *u, long which, uintptr_t unew, uintptr_t uold);
extern long rv_linux_getitimer(struct rv_uproc *u, long which, uintptr_t ucur);

extern int tk_slp_tsk(int tmout);
#ifndef EINTR
#define EINTR 4
#endif

#define TICKS_PER_SEC   10000000UL      /* QEMU virt の time CSR は 10MHz */

/* テスト用: 最後に「未実装」だった番号 */
volatile long rv_linux_last_enosys = -1;

struct lnx_timespec { int64_t sec; int64_t nsec; };

/* 日付と時刻: QEMU virt の RTC（RISC-V は goldfish、0x0010_1000。1970 年からのナノ秒。AArch64 は PL031）を、起動のあと初めて使うときに 1 回読み、
 * あとは time（起動からの時間）を足す。RTC の値がおかしければ、起動からの時間のまま（1970 年） */
static int64_t rtc_base_ns = -1;            /* RTC を読んだときの日時 − そのときの time */

static void rtc_init(void)
{
    uint64_t ns, t;
#if defined(__aarch64__)
    /* AArch64 の QEMU virt の RTC は PL031（0x0901_0000）。RTCDR（0 番地）が 1970 年からの秒 */
    volatile uint32_t *rtc = (volatile uint32_t *)RV_IO(0x09010000UL);
    t = RV_RDTIME();
    ns = (uint64_t)rtc[0] * 1000000000ULL;
#else
    volatile uint32_t *rtc = (volatile uint32_t *)RV_IO(0x101000UL);
    uint64_t lo, hi;
    t = RV_RDTIME();
    lo = rtc[0];                            /* 下を読むと上が決まる */
    hi = rtc[1];
    ns = (hi << 32) | lo;
#endif
    if (ns < 1577836800ULL * 1000000000ULL) {   /* 2020 年より前: RTC がないとみなす */
        rtc_base_ns = 0;
        return;
    }
    rtc_base_ns = (int64_t)ns - (int64_t)(t * (1000000000UL / TICKS_PER_SEC));
}

/* 1970 年からの秒（ファイルの時刻など） */
uint32_t rv_now_unix(void)
{
    uint64_t t;
    if (rtc_base_ns < 0) {
        rtc_init();
    }
    t = RV_RDTIME();
    return (uint32_t)((rtc_base_ns + (int64_t)(t * (1000000000UL / TICKS_PER_SEC))) / 1000000000LL);
}

/* 1970 年からのミリ秒（機器の操作の記録など） */
int64_t rv_now_unix_ms(void)
{
    uint64_t t;
    if (rtc_base_ns < 0) {
        rtc_init();
    }
    t = RV_RDTIME();
    return (rtc_base_ns + (int64_t)(t * (1000000000UL / TICKS_PER_SEC))) / 1000000LL;
}

static long sys_clock_gettime(long clk, uintptr_t uts)
{
    uint64_t t;
    struct lnx_timespec ts;
    int64_t ns;

    t = RV_RDTIME();
    ns = (int64_t)(t * (1000000000UL / TICKS_PER_SEC));
    if (clk == 0) {                         /* CLOCK_REALTIME: 日付と時刻 */
        if (rtc_base_ns < 0) {
            rtc_init();
        }
        ns += rtc_base_ns;
    }
    ts.sec = ns / 1000000000LL;
    ts.nsec = ns % 1000000000LL;
    return rv_user_copy_out(uts, &ts, sizeof(ts)) == 0 ? 0 : -EFAULT;
}

static long sys_nanosleep(uintptr_t ureq)
{
    struct lnx_timespec ts;
    unsigned long ms;

    if (rv_user_copy_in(&ts, ureq, sizeof(ts)) != 0) {
        return -EFAULT;
    }
    if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000L) {
        return -EINVAL;
    }
    ms = (unsigned long)ts.sec * 1000UL + (unsigned long)(ts.nsec + 999999) / 1000000UL;
    if (ms > 0) {
        /* 眠っている間にシグナルが来たら -EINTR（起床要求で早く起きたら、残りをまた眠る） */
        uint64_t t0, t, end;
        t0 = RV_RDTIME();
        end = t0 + ms * (TICKS_PER_SEC / 1000);
        for (;;) {
            if (rv_linux_sig_interrupt()) {
                return -EINTR;
            }
            t = RV_RDTIME();
            if (t >= end) {
                break;
            }
            tk_slp_tsk((int)((end - t) / (TICKS_PER_SEC / 1000)) + 1);
        }
    }
    return 0;
}

static void put_field(char *dst, const char *s)
{
    int i;
    for (i = 0; i < 64 && s[i] != 0; i++) {
        dst[i] = s[i];
    }
    for (; i < 65; i++) {
        dst[i] = 0;
    }
}

static long sys_uname(uintptr_t ubuf)
{
    char u[6 * 65];

    put_field(u + 0 * 65, "Tamazusa");
    put_field(u + 1 * 65, "tamazusa");
#if defined(__aarch64__)
    put_field(u + 2 * 65, "0.1.0-a64");
    put_field(u + 3 * 65, "T-Kernel 2.0 AArch64");
    put_field(u + 4 * 65, "aarch64");
#else
    put_field(u + 2 * 65, "0.1.0-rv64");
    put_field(u + 3 * 65, "T-Kernel 2.0 RISC-V");
    put_field(u + 4 * 65, "riscv64");
#endif
    put_field(u + 5 * 65, "");
    return rv_user_copy_out(ubuf, u, sizeof(u)) == 0 ? 0 : -EFAULT;
}

static long sys_getrandom(uintptr_t ubuf, size_t len)
{
    static uint64_t state;
    size_t done = 0;

    if (state == 0) {
        state = RV_RDTIME();
        state |= 1;
    }
    while (done < len) {
        uint64_t x;
        size_t n = len - done;
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        x = state;
        if (n > 8) {
            n = 8;
        }
        if (rv_user_copy_out(ubuf + done, &x, n) != 0) {
            return done > 0 ? (long)done : -EFAULT;
        }
        done += n;
    }
    return (long)done;
}

static long sys_prlimit64(long pid, long res, uintptr_t unew, uintptr_t uold)
{
    (void)pid;
    (void)res;
    (void)unew;
    if (uold != 0) {
        uint64_t lim[2] = { ~0ULL, ~0ULL };     /* RLIM_INFINITY */
        if (res == 3) {                         /* RLIMIT_STACK: 実際のスタックの大きさ */
            lim[0] = lim[1] = 32 * 4096;
        }
        if (rv_user_copy_out(uold, lim, sizeof(lim)) != 0) {
            return -EFAULT;
        }
    }
    return 0;
}

static void report_enosys(long nr)
{
    static long reported[16];
    static int nrep;
    int i;
    char buf[64];
    int n = 0;
    long v = nr;
    char digits[24];
    int d = 0;

    rv_linux_last_enosys = nr;
    for (i = 0; i < nrep; i++) {
        if (reported[i] == nr) {
            return;
        }
    }
    if (nrep < 16) {
        reported[nrep++] = nr;
    }
    do {
        digits[d++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v > 0 && d < 20);
    {
        const char *pre = "[linux] not implemented yet: syscall ";
        while (*pre) {
            buf[n++] = *pre++;
        }
    }
    while (d > 0) {
        buf[n++] = digits[--d];
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    buf[n] = 0;
    uart_puts(buf);
}

/* 実行中のプログラムが Linux 互換なら、そのプログラム（でなければ NULL） */
struct rv_uproc *rv_linux_current(void)
{
    struct rv_uproc *p = rv_uproc_current();
    return rv_uproc_is_linux(p) ? p : NULL;
}

/*
 * f = トラップフレーム（f[9..14] = a0..a5、f[TF_NR] = a7）
 * 戻り値: 0 = ユーザーへ戻る / 1 = プログラム終了（終了コードは f[TF_A0]）
 */
static int linux_syscall2(struct rv_uproc *p, uint64_t *f);

int rv_linux_syscall(struct rv_uproc *p, uint64_t *f)
{
    long nr = (long)f[TF_NR];
    int ret;

    if (nr == LNX_rt_sigreturn) {
        return (int)rv_linux_sigreturn(p, f);   /* フレームのレジスタをすべて戻す（a0 も） */
    }
    rv_linux_sys_enter(p, f);
    ret = linux_syscall2(p, f);
    if (ret == 0) {
        rv_linux_sys_leave(p, nr, (long)f[TF_A0]);
    }
    return ret;
}

static int linux_syscall2(struct rv_uproc *p, uint64_t *f)
{
    long nr = (long)f[TF_NR];
    uint64_t a0 = f[TF_A0], a1 = f[TF_A0 + 1], a2 = f[TF_A0 + 2], a3 = f[TF_A0 + 3], a4 = f[TF_A0 + 4];
    long r;

    /* ファイルと入出力（linux_fs.c） */
    if (rv_linux_mm_syscall(p, nr, &f[TF_A0], &r) || rv_linux_cred_syscall(p, nr, &f[TF_A0], &r)
        || rv_linux_fs_syscall(p, nr, &f[TF_A0], &r)) {
        f[TF_A0] = (uint64_t)r;
        return 0;
    }

    switch (nr) {
    case LNX_exit:
    case LNX_exit_group:
        f[TF_A0] = a0 & 0xff;               /* 終了コード（Linux と同じく下位 8 ビット） */
        return 1;
    case LNX_set_tid_address: r = rv_uproc_pid(p); break;
    case LNX_set_robust_list: r = 0; break;
    case LNX_futex:
        /* 第 1 段はスレッド 1 本: 待ち（FUTEX_WAIT）は値が変わったものとして戻り、起こし（WAKE）は 0 */
        r = ((a1 & 0x7f) == 0) ? -EAGAIN : 0;
        break;
    case LNX_nanosleep:     r = sys_nanosleep((uintptr_t)a0); break;
    case LNX_clock_gettime: r = sys_clock_gettime((long)a0, (uintptr_t)a1); break;
    case LNX_sched_yield:   r = 0; break;
    case LNX_rt_sigaction:   r = rv_linux_sigaction(p, (long)a0, (uintptr_t)a1, (uintptr_t)a2, (long)a3); break;
    case LNX_rt_sigprocmask: r = rv_linux_sigprocmask(p, (long)a0, (uintptr_t)a1, (uintptr_t)a2, (long)a3); break;
    case LNX_rt_sigpending:  r = rv_linux_sigpending(p, (uintptr_t)a0); break;
    case LNX_rt_sigsuspend:  r = rv_linux_sigsuspend(p, (uintptr_t)a0, (long)a1); break;
    case LNX_kill:           r = rv_linux_kill(p, (long)a0, (long)a1); break;
    case LNX_tkill:          r = rv_linux_kill(p, (long)a0, (long)a1); break;     /* スレッド = プロセス */
    case LNX_tgkill:         r = rv_linux_kill(p, (long)a1, (long)a2); break;
    case LNX_uname:         r = sys_uname((uintptr_t)a0); break;
    case 71:                r = -EINVAL; break;
    /* uid・gid（143〜159・174〜177）は linux_proc.c の rv_linux_cred_syscall が先に受ける */
    case 34: case 35: case 36: case 37: case 38: case 276:
        r = -30;            /* mkdirat unlinkat symlinkat linkat renameat renameat2: EROFS（ディスクは読み取り専用） */
        break;
    case 52: case 53: case 54: case 55:
        r = 0;              /* fchmod fchmodat fchownat fchown: ディスクは読み取り専用で、持ち主は root だけ。受け付けるだけ */
        break;
    case 281:               r = -ENOSYS; break;     /* execveat（fexecve）: まだ。呼ぶ側は execve に戻る */
    case 102:               r = rv_linux_getitimer(p, (long)a0, (uintptr_t)a1); break;
    case 103:               r = rv_linux_setitimer(p, (long)a0, (uintptr_t)a1, (uintptr_t)a2); break;
    case 165: {                                     /* getrusage: 使った時間はまだ数えていない（0） */
        uint8_t ru[144] = { 0 };
        r = rv_user_copy_out((uintptr_t)a1, ru, sizeof(ru)) == 0 ? 0 : -EFAULT;
        break;
    }
    case 153: {                                     /* times: 戻り値は起動からの時間（1/100 秒） */
        uint64_t tms[4] = { 0, 0, 0, 0 }, t;
        t = RV_RDTIME();
        r = (a0 != 0 && rv_user_copy_out((uintptr_t)a0, tms, sizeof(tms)) != 0) ? -EFAULT
            : (long)(t / (TICKS_PER_SEC / 100));
        break;
    }     /* sendfile: 使えないと返せば、read と write で写してくれる */
    case LNX_getpid:
    case LNX_gettid:        r = rv_uproc_pid(p); break;
    case LNX_getppid:       r = rv_linux_getppid(p); break;
    case LNX_clone:         r = rv_linux_clone(p, f, (unsigned long)a0, (uintptr_t)a1); break;
    case LNX_execve:        r = rv_linux_execve(p, f, (uintptr_t)a0, (uintptr_t)a1, (uintptr_t)a2); break;
    case LNX_wait4:         r = rv_linux_wait4(p, (long)a0, (uintptr_t)a1, (long)a2); break;
    case LNX_prctl:         r = 0; break;           /* PR_SET_NAME など: 受け付けるだけ */
    case LNX_umask:         r = 022; break;
    case LNX_setpgid:       r = rv_linux_setpgid(p, (long)a0, (long)a1); break;
    case LNX_getpgid:       r = rv_linux_getpgid(p, (long)a0); break;
    case LNX_getsid:        r = rv_linux_getsid(p, (long)a0); break;
    case LNX_setsid:        r = rv_linux_setsid(p); break;
    case LNX_sigaltstack:   r = 0; break;
    case LNX_brk:           r = (long)rv_linux_brk(p, (uintptr_t)a0); break;
    case LNX_mmap:
        if (!(a3 & MAP_ANONYMOUS) || (long)a4 != -1) {
            r = -ENOSYS;                /* ファイルの mmap は後の段 */
            report_enosys(nr);
        } else {
            uintptr_t va = rv_linux_mmap_anon(p, (size_t)a1);
            r = va ? (long)va : -ENOMEM;
        }
        break;
    case LNX_munmap:        r = rv_linux_munmap(p, (uintptr_t)a0, (size_t)a1); break;
    case LNX_mprotect:
    case LNX_madvise:       r = 0; break;
    case LNX_prlimit64:     r = sys_prlimit64((long)a0, (long)a1, (uintptr_t)a2, (uintptr_t)a3); break;
    case LNX_getrandom:     r = sys_getrandom((uintptr_t)a0, (size_t)a1); break;
    default:
        report_enosys(nr);
        r = -ENOSYS;
        break;
    }
    f[TF_A0] = (uint64_t)r;
    return 0;
}
