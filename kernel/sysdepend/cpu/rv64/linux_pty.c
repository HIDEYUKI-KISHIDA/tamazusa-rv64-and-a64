/*
 * linux_pty.c — 疑似端末（/dev/ptmx・/dev/pts/N）（S-mode + MMU のみ）
 *
 *   sshd（dropbear）がログインした人のシェルにつなぐ「端末」。親（マスター）側は sshd が持ち、
 *   子（スレーブ）側はシェルの標準入出力になる。
 *     マスターに書いた文字 → 行の規則（termios）を通して → スレーブで読める
 *       ICRNL（CR を改行に）、ISIG（Ctrl-C → SIGINT、Ctrl-\ → SIGQUIT、Ctrl-Z → SIGTSTP を前面のグループへ）、
 *       ICANON（行ごと: Backspace で消す・Ctrl-U で行を消す・行頭の Ctrl-D で終わり）、ECHO（打った文字を返す）
 *     スレーブに書いた文字 → OPOST|ONLCR（改行を CR LF に）→ マスターで読める
 *   スレーブ側がすべて閉じると、マスターの read は -EIO（Linux と同じ。sshd はこれで終わりを知る）。
 *   端末の大きさ（TIOCSWINSZ）が変わると、前面のグループへ SIGWINCH。
 */
#include <stdint.h>
#include <stddef.h>

extern int  rv_user_copy_in(void *dst, uintptr_t uva, size_t len);
extern int  rv_user_copy_out(uintptr_t uva, const void *src, size_t len);
extern int  rv_linux_sig_interrupt(void);
extern void rv_linux_signal_pgrp(int pgid, int sig);
extern int  tk_dis_dsp(void);
extern int  tk_ena_dsp(void);
extern int  tk_slp_tsk(int tmout);
extern int  tk_wup_tsk(int tskid);
extern int  tk_get_tid(void);

#define EINTR   4
#define EIO     5
#define EAGAIN  11
#define EFAULT  14
#define EINVAL  22
#define ENOTTY  25
#define ENXIO   6

#define NPTY    8
#define RING    4096

#define T_ICRNL   0x100
#define T_OPOST   0x001
#define T_ONLCR   0x004
#define T_ISIG    0x001
#define T_ICANON  0x002
#define T_ECHO    0x008
#define T_ECHOE   0x010
#define T_ECHOCTL 0x200

struct lnx_termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t  line;
    uint8_t  cc[19];
};

struct ring {
    uint8_t  b[RING];
    unsigned head, tail, count;
};

struct pty {
    int used;
    int masters, slaves;            /* 開いている fd の数 */
    int locked;
    int eof_pending;                /* 行頭の Ctrl-D（スレーブの read が 0 を返す） */
    int fg_pgrp;
    struct lnx_termios tio;
    uint16_t ws[4];                 /* 行・桁・横の点・縦の点 */
    struct ring in;                 /* スレーブが読む（行の規則を通したもの） */
    struct ring out;                /* マスターが読む */
    uint8_t  line[512];             /* ICANON で編集中の行 */
    unsigned line_len;
    volatile int m_wait, s_wait;    /* 眠っている読む側（マスター・スレーブ） */
};
static struct pty ptys[NPTY];

static const struct lnx_termios default_tio = {
    0x100 | 0x400,                  /* ICRNL | IXON */
    0x001 | 0x004,                  /* OPOST | ONLCR */
    0x0bf,                          /* B38400 | CS8 | CREAD */
    0x001 | 0x002 | 0x008 | 0x010 | 0x020 | 0x200 | 0x8000,   /* ISIG ICANON ECHO ECHOE ECHOK ECHOCTL IEXTEN */
    0,
    { 3, 0x1c, 0x7f, 0x15, 4, 0, 1, 0, 0x11, 0x13, 0x1a, 0, 0x12, 0x0f, 0x17, 0x16, 0, 0, 0 }
};

static int ring_put(struct ring *r, uint8_t c)
{
    if (r->count >= RING) {
        return 0;
    }
    r->b[r->head] = c;
    r->head = (r->head + 1) % RING;
    r->count++;
    return 1;
}

static int ring_get(struct ring *r)
{
    int c;
    if (r->count == 0) {
        return -1;
    }
    c = r->b[r->tail];
    r->tail = (r->tail + 1) % RING;
    r->count--;
    return c;
}

static void wake(volatile int *w)
{
    int t = *w;
    if (t > 0) {
        *w = 0;
        tk_wup_tsk(t);
    }
}

/* マスターへ出す（ONLCR） */
static void out_char(struct pty *p, uint8_t c)
{
    if ((p->tio.oflag & T_OPOST) && (p->tio.oflag & T_ONLCR) && c == '\n') {
        ring_put(&p->out, '\r');
    }
    ring_put(&p->out, c);
}

static void echo_char(struct pty *p, uint8_t c)
{
    if (!(p->tio.lflag & T_ECHO)) {
        return;
    }
    if ((p->tio.lflag & T_ECHOCTL) && c < 0x20 && c != '\n' && c != '\t' && c != '\r') {
        out_char(p, '^');
        out_char(p, (uint8_t)(c + '@'));
        return;
    }
    out_char(p, c);
}

/* マスターから届いた 1 文字を行の規則に通す */
static void input_char(struct pty *p, uint8_t c)
{
    struct lnx_termios *t = &p->tio;

    if (c == '\r' && (t->iflag & T_ICRNL)) {
        c = '\n';
    }
    if (t->lflag & T_ISIG) {
        int sig = 0;
        if (c == t->cc[0]) {
            sig = 2;
        } else if (c == t->cc[1]) {
            sig = 3;
        } else if (c == t->cc[10]) {
            sig = 20;
        }
        if (sig) {
            echo_char(p, c);
            p->line_len = 0;
            if (p->fg_pgrp > 0) {
                rv_linux_signal_pgrp(p->fg_pgrp, sig);
            }
            return;
        }
    }
    if (!(t->lflag & T_ICANON)) {
        ring_put(&p->in, c);
        echo_char(p, c);
        return;
    }
    if (c == t->cc[2] || c == 0x08) {                   /* VERASE */
        if (p->line_len > 0) {
            p->line_len--;
            if ((t->lflag & T_ECHO) && (t->lflag & T_ECHOE)) {
                out_char(p, '\b');
                out_char(p, ' ');
                out_char(p, '\b');
            }
        }
        return;
    }
    if (c == t->cc[3]) {                                /* VKILL（Ctrl-U） */
        while (p->line_len > 0) {
            p->line_len--;
            if (t->lflag & T_ECHO) {
                out_char(p, '\b');
                out_char(p, ' ');
                out_char(p, '\b');
            }
        }
        return;
    }
    if (c == t->cc[4]) {                                /* VEOF（Ctrl-D） */
        unsigned i;
        if (p->line_len == 0) {
            p->eof_pending = 1;
        }
        for (i = 0; i < p->line_len; i++) {
            ring_put(&p->in, p->line[i]);
        }
        p->line_len = 0;
        return;
    }
    if (p->line_len < sizeof(p->line)) {
        p->line[p->line_len++] = c;
    }
    echo_char(p, c);
    if (c == '\n') {
        unsigned i;
        for (i = 0; i < p->line_len; i++) {
            ring_put(&p->in, p->line[i]);
        }
        p->line_len = 0;
    }
}

/* ---- 開く・閉じる ---- */

int rv_pty_open_master(void)
{
    int i;
    for (i = 0; i < NPTY; i++) {
        if (!ptys[i].used) {
            struct pty *p = &ptys[i];
            unsigned k;
            for (k = 0; k < sizeof(*p); k++) {
                ((uint8_t *)p)[k] = 0;
            }
            p->used = 1;
            p->masters = 1;
            p->locked = 1;
            p->tio = default_tio;
            p->ws[0] = 24;
            p->ws[1] = 80;
            return i;
        }
    }
    return -28;                         /* ENOSPC */
}

int rv_pty_open_slave(int n)
{
    if (n < 0 || n >= NPTY || !ptys[n].used || ptys[n].masters <= 0) {
        return -ENXIO;
    }
    if (ptys[n].locked) {
        return -EIO;
    }
    ptys[n].slaves++;
    return n;
}

int rv_pty_exists(int n)
{
    return n >= 0 && n < NPTY && ptys[n].used;
}

void rv_pty_ref(int n, int master)
{
    if (rv_pty_exists(n)) {
        if (master) ptys[n].masters++; else ptys[n].slaves++;
    }
}

void rv_pty_unref(int n, int master)
{
    struct pty *p;
    if (!rv_pty_exists(n)) {
        return;
    }
    p = &ptys[n];
    tk_dis_dsp();
    if (master) {
        p->masters--;
        wake(&p->s_wait);               /* スレーブの読む側は EOF / EIO で起きる */
    } else {
        p->slaves--;
        wake(&p->m_wait);
    }
    if (p->masters <= 0 && p->slaves <= 0) {
        p->used = 0;
    }
    tk_ena_dsp();
}

/* ---- 読み書き ---- */

long rv_pty_read(int n, int master, uintptr_t ubuf, size_t len, int nonblock)
{
    struct pty *p = &ptys[n];
    uint8_t chunk[512];
    size_t k;

    if (len == 0) {
        return 0;
    }
    for (;;) {
        k = 0;
        tk_dis_dsp();
        if (master) {
            while (k < len && k < sizeof(chunk) && p->out.count > 0) {
                chunk[k++] = (uint8_t)ring_get(&p->out);
            }
            if (k == 0 && p->slaves <= 0) {
                tk_ena_dsp();
                return -EIO;            /* スレーブがすべて閉じた */
            }
        } else {
            if (p->tio.lflag & T_ICANON) {
                /* 1 行まで（改行で止める） */
                while (k < len && k < sizeof(chunk) && p->in.count > 0) {
                    int c = ring_get(&p->in);
                    chunk[k++] = (uint8_t)c;
                    if (c == '\n') {
                        break;
                    }
                }
            } else {
                while (k < len && k < sizeof(chunk) && p->in.count > 0) {
                    chunk[k++] = (uint8_t)ring_get(&p->in);
                }
            }
            if (k == 0 && p->eof_pending) {
                p->eof_pending = 0;
                tk_ena_dsp();
                return 0;               /* Ctrl-D */
            }
            if (k == 0 && p->masters <= 0) {
                tk_ena_dsp();
                return 0;               /* マスターが閉じた */
            }
        }
        if (k > 0) {
            tk_ena_dsp();
            return rv_user_copy_out(ubuf, chunk, k) == 0 ? (long)k : -EFAULT;
        }
        if (nonblock) {
            tk_ena_dsp();
            return -EAGAIN;
        }
        if (master) {
            p->m_wait = tk_get_tid();
        } else {
            p->s_wait = tk_get_tid();
        }
        tk_ena_dsp();
        tk_slp_tsk(100);
        if (rv_linux_sig_interrupt()) {
            return -EINTR;
        }
    }
}

long rv_pty_write(int n, int master, uintptr_t ubuf, size_t len)
{
    struct pty *p = &ptys[n];
    uint8_t chunk[256];
    size_t done = 0;

    if (master ? p->slaves < 0 : p->masters <= 0) {
        return -EIO;
    }
    while (done < len) {
        size_t m = len - done, i;
        if (m > sizeof(chunk)) {
            m = sizeof(chunk);
        }
        if (rv_user_copy_in(chunk, ubuf + done, m) != 0) {
            return done ? (long)done : -EFAULT;
        }
        tk_dis_dsp();
        for (i = 0; i < m; i++) {
            if (master) {
                input_char(p, chunk[i]);
            } else {
                out_char(p, chunk[i]);
            }
        }
        wake(&p->m_wait);               /* エコー・出力 */
        wake(&p->s_wait);
        tk_ena_dsp();
        done += m;
    }
    return (long)done;
}

/* poll の形 */
int rv_pty_events(int n, int master)
{
    struct pty *p = &ptys[n];
    int ev = 0x4;
    if (master) {
        if (p->out.count > 0) ev |= 0x1;
        if (p->slaves <= 0) ev |= 0x10 | 0x1;
    } else {
        int ready = 0;
        if (p->tio.lflag & T_ICANON) {
            unsigned i, pos = p->in.tail;
            for (i = 0; i < p->in.count; i++) {
                if (p->in.b[pos] == '\n') {
                    ready = 1;
                    break;
                }
                pos = (pos + 1) % RING;
            }
            if (p->in.count >= RING) {
                ready = 1;
            }
        } else {
            ready = p->in.count > 0;
        }
        if (ready || p->eof_pending) ev |= 0x1;
        if (p->masters <= 0) ev |= 0x10 | 0x1;
    }
    return ev;
}

/* ioctl。扱わない番号なら 1 を返す（呼ぶ側が -ENOTTY などにする） */
int rv_pty_ioctl(int n, int master, unsigned long cmd, uintptr_t arg, long *ret)
{
    struct pty *p = &ptys[n];
    /* ioctl の番号は int で渡される: 0x80045430（TIOCGPTN）は符号が広がって届くので下位 32 ビットで見る */
    switch ((uint32_t)cmd) {
    case 0x5401:                                        /* TCGETS */
        *ret = rv_user_copy_out(arg, &p->tio, sizeof(p->tio)) == 0 ? 0 : -EFAULT;
        return 0;
    case 0x5402: case 0x5403: case 0x5404: {            /* TCSETS / TCSETSW / TCSETSF */
        struct lnx_termios nt;
        if (rv_user_copy_in(&nt, arg, sizeof(nt)) != 0) {
            *ret = -EFAULT;
            return 0;
        }
        tk_dis_dsp();
        if ((p->tio.lflag & T_ICANON) && !(nt.lflag & T_ICANON)) {
            unsigned i;                                 /* 編集中の行を読めるように渡す */
            for (i = 0; i < p->line_len; i++) {
                ring_put(&p->in, p->line[i]);
            }
            p->line_len = 0;
        }
        p->tio = nt;
        if (cmd == 0x5404) {
            p->in.count = p->in.head = p->in.tail = 0;
        }
        tk_ena_dsp();
        *ret = 0;
        return 0;
    }
    case 0x5413:                                        /* TIOCGWINSZ */
        *ret = rv_user_copy_out(arg, p->ws, sizeof(p->ws)) == 0 ? 0 : -EFAULT;
        return 0;
    case 0x5414: {                                      /* TIOCSWINSZ */
        uint16_t ws[4];
        if (rv_user_copy_in(ws, arg, sizeof(ws)) != 0) {
            *ret = -EFAULT;
            return 0;
        }
        if (ws[0] != p->ws[0] || ws[1] != p->ws[1]) {
            p->ws[0] = ws[0]; p->ws[1] = ws[1]; p->ws[2] = ws[2]; p->ws[3] = ws[3];
            if (p->fg_pgrp > 0) {
                rv_linux_signal_pgrp(p->fg_pgrp, 28);   /* SIGWINCH */
            }
        }
        *ret = 0;
        return 0;
    }
    case 0x80045430: {                                  /* TIOCGPTN */
        uint32_t v = (uint32_t)n;
        *ret = (master && rv_user_copy_out(arg, &v, 4) == 0) ? 0 : -EINVAL;
        return 0;
    }
    case 0x40045431: {                                  /* TIOCSPTLCK */
        int32_t v = 0;
        rv_user_copy_in(&v, arg, 4);
        p->locked = v != 0;
        *ret = 0;
        return 0;
    }
    case 0x540F: {                                      /* TIOCGPGRP */
        int32_t v = p->fg_pgrp;
        *ret = rv_user_copy_out(arg, &v, 4) == 0 ? 0 : -EFAULT;
        return 0;
    }
    case 0x5410: {                                      /* TIOCSPGRP */
        int32_t v;
        if (rv_user_copy_in(&v, arg, 4) != 0) {
            *ret = -EFAULT;
            return 0;
        }
        p->fg_pgrp = v;
        *ret = 0;
        return 0;
    }
    case 0x541B: {                                      /* FIONREAD */
        int32_t v = (int32_t)(master ? p->out.count : p->in.count);
        *ret = rv_user_copy_out(arg, &v, 4) == 0 ? 0 : -EFAULT;
        return 0;
    }
    case 0x540E:                                        /* TIOCSCTTY */
    case 0x5422:                                        /* TIOCNOTTY */
    case 0x540B:                                        /* TCFLSH */
    case 0x5409:                                        /* TCSBRK（tcdrain） */
        *ret = 0;
        return 0;
    default:
        return 1;
    }
}

/* 制御端末になったとき: 前面のグループがまだなければ pgid にする */
void rv_pty_claim(int n, int pgid)
{
    if (rv_pty_exists(n) && ptys[n].fg_pgrp <= 0) {
        ptys[n].fg_pgrp = pgid;
    }
}

void rv_pty_set_fg(int n, int pgid)
{
    if (rv_pty_exists(n)) {
        ptys[n].fg_pgrp = pgid;
    }
}

int rv_pty_count(void)
{
    int i, c = 0;
    for (i = 0; i < NPTY; i++) {
        c += ptys[i].used;
    }
    return c;
}
