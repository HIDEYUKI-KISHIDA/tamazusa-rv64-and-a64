/* kernel/io/iomgr.c — 入出力の管理（/dev/io の中身）
 *
 *   名前の口と /etc/io.conf（include/iomgr_core.h）をカーネルに 1 つ持つ。中身（backend）は、
 *   vio（仮想の機器、いつもある）・usbio（USB の I/O ボード、つながったとき）・som（SoM のピン、デバイスツリーで見つけたとき）。
 *   io_task が 10 ミリ秒ごとにウォッチドッグを見る（読み書きがなくても、時間が来たら出力を止める）。
 *   すべての操作は割込みを止めて行う（中身の操作は待たない作りにしてある）。
 *
 *   安全の決まり 6（誰が動かせるか）: 読むのは誰でもよい。動かす（出力・解除・ウォッチドッグの設定）のは、
 *   root か、/etc/io.conf の access group のグループの人だけ。ネットワークのソケットを開いたプロセスと
 *   その子（ssh で入った人など）は、network allow と書かないかぎり動かせない。止めること（ESTOP・安全な状態へ）は誰でもできる。
 *   安全の決まり 7（記録）: いつ・どのプログラムが・どの口に・何を出したか（値が変わったときだけ）、解除・停止・
 *   ウォッチドッグ・断ったことを、カーネルの中に 256 件ためる。/dev/iolog で読め、/data があれば
 *   /data/log/io.log に 1 秒ごとに書き足す（電源を切っても残る）。
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>
#include <rv_priv.h>
#include "iomgr_core.h"
#include <libstr.h>
#include "../fs/tarfs.h"

extern void uart_puts(const char *s);
extern int rv_linux_may_control(int io_gid, int net_ok);
extern int rv_linux_who(int *pid, int *uid, char *comm, int max);
extern int64_t rv_now_unix_ms(void);
#include "../fs/ext2.h"

static VioState vio;
static IoMgr mgr;
static IoBackend vio_be;
static int ready;
static char conf_buf[8192];
volatile unsigned long rv_io_ticks;

#define LOCK(st)   ((st) = RV_IRQ_SAVE())
#define UNLOCK(st) RV_IRQ_RESTORE(st)

/* ---------------------------------------------------------------- 記録（安全の決まり 7） */
enum { LG_SET = 1, LG_RAW, LG_ARM, LG_STOP, LG_SAFE, LG_DENY_NET, LG_DENY_PERM, LG_WDT, LG_WDTSET };
struct iolog {
    int64_t ms;                         /* 1970 年からのミリ秒 */
    int pid, uid;
    char comm[16];
    char what[16];
    int64_t value;
    uint8_t kind, net;
};
#define NLOG 256
static struct iolog logq[NLOG];
static volatile uint32_t log_head;      /* これまでに入れた数 */
static uint32_t log_saved;              /* /data/log/io.log に書いた数 */
static int64_t last_set[IO_MAXENT];
static uint8_t last_set_has[IO_MAXENT];
static int32_t last_raw[128];
static uint8_t last_raw_has[128];
volatile unsigned long rv_io_log_count, rv_io_denied;

struct who { int pid, uid, net; char comm[16]; };

static void who_now(struct who *w)
{
    w->net = rv_linux_who(&w->pid, &w->uid, w->comm, sizeof(w->comm));
}

/* 1 件入れる（割込みを止めた中で呼んでよい） */
static void log_put(const struct who *w, int kind, const char *what, int64_t value)
{
    struct iolog *e = &logq[log_head % NLOG];
    int k;
    e->ms = rv_now_unix_ms();
    e->pid = w->pid;
    e->uid = w->uid;
    e->net = (uint8_t)w->net;
    for (k = 0; k < 15 && w->comm[k]; k++) {
        e->comm[k] = w->comm[k];
    }
    e->comm[k] = 0;
    for (k = 0; k < 15 && what && what[k]; k++) {
        e->what[k] = what[k];
    }
    e->what[k] = 0;
    e->value = value;
    e->kind = (uint8_t)kind;
    log_head++;
    rv_io_log_count++;
}

static char *put_s(char *p, const char *s)
{
    while (*s) {
        *p++ = *s++;
    }
    return p;
}

static char *put_n(char *p, long v, int width)
{
    char b[24];
    int i = 23, neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + u % 10);
        u /= 10;
        width--;
    } while (u || width > 0);
    if (neg) {
        b[--i] = '-';
    }
    return put_s(p, b + i);
}

/* ミリ単位の値を「12.345」の形に */
static char *put_milli(char *p, int64_t v)
{
    if (v < 0) {
        *p++ = '-';
        v = -v;
    }
    p = put_n(p, (long)(v / 1000), 1);
    *p++ = '.';
    return put_n(p, (long)(v % 1000), 3);
}

/* 1 件を 1 行の文字に（JST の日時。ISO 8601 の形）。長さを返す */
static int log_line(const struct iolog *e, char *out)
{
    static const char *const kn[] = { "?", "SET", "OUT", "ARM", "STOP", "SAFE", "DENY-NET", "DENY-PERM", "WATCHDOG", "WDT" };
    int64_t t = e->ms / 1000 + 9 * 3600;
    long z = (long)(t / 86400) + 719468, era = z / 146097, doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    long sec = (long)(t % 86400);
    char *p = out;
    if (m <= 2) {
        y++;
    }
    p = put_n(p, y, 4); *p++ = '-'; p = put_n(p, m, 2); *p++ = '-'; p = put_n(p, d, 2);
    *p++ = 'T';
    p = put_n(p, sec / 3600, 2); *p++ = ':'; p = put_n(p, sec / 60 % 60, 2); *p++ = ':'; p = put_n(p, sec % 60, 2);
    *p++ = '.';
    p = put_n(p, (long)(e->ms % 1000), 3);
    p = put_s(p, "+09:00 pid ");
    p = put_n(p, e->pid, 1);
    *p++ = ' ';
    p = put_s(p, e->comm);
    p = put_s(p, " uid ");
    p = put_n(p, e->uid, 1);
    if (e->net) {
        p = put_s(p, " net");
    }
    *p++ = ' ';
    p = put_s(p, kn[e->kind < 10 ? e->kind : 0]);
    if (e->what[0]) {
        *p++ = ' ';
        p = put_s(p, e->what);
    }
    if (e->kind == LG_SET) {
        *p++ = ' ';
        p = put_milli(p, e->value);
    } else if (e->kind == LG_RAW || e->kind == LG_WDTSET) {
        *p++ = ' ';
        p = put_n(p, (long)e->value, 1);
    }
    *p++ = '\n';
    *p = 0;
    return (int)(p - out);
}

/* /dev/iolog: *seq 番目から、入るだけ行にして kbuf へ。書いたバイト数（古くて消えた分は飛ばす） */
long iomgr_log_read(uint64_t *seq, char *kbuf, size_t max)
{
    size_t n = 0;
    char line[160];
    uint32_t s = (uint32_t)*seq;
    if (log_head - s > NLOG) {
        s = log_head - NLOG;
    }
    while (s != log_head) {
        struct iolog e;
        unsigned long st;
        int len;
        LOCK(st);
        e = logq[s % NLOG];
        UNLOCK(st);
        len = log_line(&e, line);
        if (n + (size_t)len > max) {
            break;
        }
        memcpy(kbuf + n, line, (size_t)len);
        n += (size_t)len;
        s++;
    }
    *seq = s;
    return (long)n;
}

/* /data/log/io.log に書き足す（1 秒ごと、io_log_task から） */
static void log_save(void)
{
    static char buf[4096];
    char redir[EXT2_PATH_MAX];
    uint32_t ino;
    struct ext2_stat st;
    uint64_t seq;
    long n;
    if (log_saved == log_head || !ext2_mounted() || ext2_readonly()) {
        return;
    }
    ext2_mkdir("log", 0755, redir);                 /* あれば EEXIST（かまわない） */
    if (ext2_open("log/io.log", EXT2_CREAT, 0644, &ino, redir) != 0 || ext2_stat(ino, &st) != 0) {
        return;
    }
    seq = log_saved;
    while ((n = iomgr_log_read(&seq, buf, sizeof(buf))) > 0) {
        if (ext2_write(ino, st.size, buf, (size_t)n) != n) {
            break;
        }
        st.size += (uint64_t)n;
    }
    log_saved = (uint32_t)seq;
}

static void io_log_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;
    for (;;) {
        tk_dly_tsk(1000);
        log_save();
    }
}


int64_t rv_io_now_ms(void)
{
    uint64_t t;
    t = RV_RDTIME();
    return (int64_t)(t / 10000);            /* QEMU virt の time は 10MHz */
}

static void put_dec(long v)
{
    char b[24];
    int i = 23;
    int neg = v < 0;
    b[i] = 0;
    if (neg) {
        v = -v;
    }
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    if (neg) {
        b[--i] = '-';
    }
    uart_puts(b + i);
}

static void io_task(INT stacd, void *exinf)
{
    unsigned long st;
    (void)stacd;
    (void)exinf;
    for (;;) {
        int before;
        tk_dly_tsk(10);
        LOCK(st);
        io_vio_now = rv_io_now_ms();
        before = mgr.state;
        io_tick(&mgr, io_vio_now);
        if (before != IO_WATCHDOG && mgr.state == IO_WATCHDOG) {
            static const struct who k = { 0, 0, 0, "kernel" };
            log_put(&k, LG_WDT, "", 0);             /* ウォッチドッグで止めた */
        }
        io_tick_backends(&mgr, 10);
        rv_io_ticks++;
        UNLOCK(st);
    }
}

/* 起動のとき 1 回（ディスクが読めるようになってから） */
void iomgr_init(void)
{
    struct tarfs_file f;
    int64_t n = -1;
    T_CTSK ct;
    ID tid;

    if (ready) {
        return;
    }
    vio_init(&vio, rv_io_now_ms());
    io_init(&mgr);
    if (tarfs_find("etc/io.conf", &f) == 1) {
        n = tarfs_read_all(&f, conf_buf, sizeof(conf_buf) - 1);
    }
    if (n < 0) {
        const char *d = io_default_conf;
        for (n = 0; d[n] && n < (int64_t)sizeof(conf_buf) - 1; n++) {
            conf_buf[n] = d[n];
        }
        uart_puts("[io] no /etc/io.conf: built-in names\r\n");
    }
    io_parse_conf(&mgr, conf_buf, (size_t)n);
    io_vio_now = rv_io_now_ms();
    io_vio_backend(&vio_be, &vio);
    io_add_backend(&mgr, &vio_be);
    uart_puts("[io] /dev/io: ");
    put_dec(mgr.nent);
    uart_puts(" names");
    if (mgr.conf_errors) {
        uart_puts(", ");
        put_dec(mgr.conf_errors);
        uart_puts(" bad lines (first: line ");
        put_dec(mgr.conf_err_line);
        uart_puts(")");
    }
    uart_puts(". Outputs are off until ESTOP 0\r\n");
    ready = 1;
    {
        extern void firmata_init(void);
        firmata_init();                     /* USB の I/O ボードがつながったら usbio として加わる */
    }
    {
        extern void som_pins_init(void);
        som_pins_init();                    /* デバイスツリーに知っている GPIO・PWM があれば som として加わる */
    }

    memset(&ct, 0, sizeof(ct));
    ct.tskatr = TA_HLNG | TA_RNG0;
    ct.itskpri = 9;
    ct.stksz = 2048;
    ct.task = (FP)io_task;
    tid = tk_cre_tsk(&ct);
    if (tid > 0) {
        tk_sta_tsk(tid, 0);
    }
    ct.itskpri = 11;                        /* 記録を /data へ（急がない） */
    ct.stksz = 4096;
    ct.task = (FP)io_log_task;
    tid = tk_cre_tsk(&ct);
    if (tid > 0) {
        tk_sta_tsk(tid, 0);
    }
}

/* ドライバが中身を足す（usbio・som） */
void iomgr_add_backend(IoBackend *b)
{
    unsigned long st;
    LOCK(st);
    io_vio_now = rv_io_now_ms();
    io_add_backend(&mgr, b);
    UNLOCK(st);
}

static long io_errno(int r)
{
    switch (r) {
    case IO_OK: return 0;
    case IO_ENODEV: return -19;             /* ENODEV */
    case IO_ERANGE: return -34;             /* ERANGE */
    case IO_ESTOP: return -1;               /* EPERM: 止まっている */
    case IO_EKIND: return -22;              /* EINVAL */
    }
    return -5;                              /* EIO */
}

/* 番号の書き方: pread・pwrite で、位置 = ポート番号 × 4 に 32 ビット。n 個まとめて */
static void port_name(unsigned port, char *b)
{
    static const char hx[] = "0123456789ABCDEF";
    unsigned a = 0xE000 + port;
    b[0] = '&'; b[1] = 'H';
    b[2] = hx[(a >> 12) & 15]; b[3] = hx[(a >> 8) & 15]; b[4] = hx[(a >> 4) & 15]; b[5] = hx[a & 15];
    b[6] = 0;
}

long iomgr_raw(unsigned port, int32_t *v, size_t n, int wr)
{
    unsigned long st;
    size_t i;
    int r = IO_OK;
    int64_t now = rv_io_now_ms();
    struct who w;
    int perm = 0;

    if (!ready) {
        return -19;
    }
    if (wr) {
        who_now(&w);
        /* 止めること（ESTOP に 0 でない値）だけなら、誰でもできる */
        if (!(n == 1 && port == VIO_P_ESTOP && v[0] != 0)) {
            perm = rv_linux_may_control(mgr.io_gid, mgr.net_ok);
        }
        if (perm < 0) {
            char pn[8];
            port_name(port, pn);
            LOCK(st);
            log_put(&w, perm == -1 ? LG_DENY_NET : LG_DENY_PERM, pn, 0);
            UNLOCK(st);
            rv_io_denied++;
            return -13;                     /* EACCES */
        }
    }
    LOCK(st);
    io_vio_now = now;
    for (i = 0; i < n && r == IO_OK; i++) {
        unsigned pt = port + (unsigned)i;
        r = wr ? io_raw_write(&mgr, &vio, pt, v[i], now)
               : io_raw_read(&mgr, &vio, pt, &v[i], now);
        if (wr && r == IO_OK) {
            char pn[8];
            if (pt == VIO_P_ESTOP) {
                log_put(&w, v[i] ? LG_STOP : LG_ARM, "", 0);
            } else if (pt == VIO_P_WDT) {
                log_put(&w, LG_WDTSET, "", v[i]);
            } else if (pt < 128 && (!last_raw_has[pt] || last_raw[pt] != v[i])) {
                port_name(pt, pn);
                log_put(&w, LG_RAW, pn, v[i]);  /* 値が変わったときだけ */
                last_raw[pt] = v[i];
                last_raw_has[pt] = 1;
            }
        }
    }
    UNLOCK(st);
    return r == IO_OK ? (long)n : io_errno(r);
}

/* 名前の口（ioctl） */
struct io_req {
    char name[32];
    int32_t id;
    int32_t kind;
    int32_t speed;
    int32_t flags;              /* 1 = 出力 */
    int64_t value;              /* ミリ単位 */
};

#define IOC_LOOKUP  0x4f01
#define IOC_GET     0x4f02
#define IOC_SET     0x4f03
#define IOC_SAFE    0x4f04

long iomgr_ioctl(unsigned long cmd, struct io_req *q)
{
    unsigned long st;
    int r = IO_OK;
    int64_t now = rv_io_now_ms();
    struct who w;

    if (!ready) {
        return -19;
    }
    if (cmd == IOC_SET || cmd == IOC_SAFE) {
        who_now(&w);
    }
    if (cmd == IOC_SET) {
        int perm = rv_linux_may_control(mgr.io_gid, mgr.net_ok);
        if (perm < 0) {
            LOCK(st);
            log_put(&w, perm == -1 ? LG_DENY_NET : LG_DENY_PERM,
                    q->id >= 0 && q->id < mgr.nent ? mgr.ent[q->id].name : "?", 0);
            UNLOCK(st);
            rv_io_denied++;
            return -13;                     /* EACCES */
        }
    }
    LOCK(st);
    io_vio_now = now;
    switch (cmd) {
    case IOC_LOOKUP:
        q->name[31] = 0;
        r = io_lookup(&mgr, q->name);
        if (r >= 0) {
            q->id = r;
            q->kind = mgr.ent[r].kind;
            q->flags = mgr.ent[r].output | (io_find_be(&mgr, mgr.ent[r].be) ? 2 : 0);   /* 2 = 中身がある */
            r = IO_OK;
        }
        break;
    case IOC_GET:
        r = io_get(&mgr, q->id, &q->value, now);
        break;
    case IOC_SET:
        r = io_set(&mgr, q->id, q->value, q->speed, now);
        if (r == IO_OK && q->id >= 0 && q->id < IO_MAXENT
            && (!last_set_has[q->id] || last_set[q->id] != q->value)) {
            log_put(&w, LG_SET, mgr.ent[q->id].name, q->value);    /* 値が変わったときだけ */
            last_set[q->id] = q->value;
            last_set_has[q->id] = 1;
        }
        break;
    case IOC_SAFE:
        io_safe_all(&mgr);
        log_put(&w, LG_SAFE, "", 0);
        break;
    default:
        r = IO_EKIND;
    }
    UNLOCK(st);
    return io_errno(r);
}

/* テスト用 */
int rv_io_state(void)
{
    return mgr.state;
}

int rv_io_names(void)
{
    return mgr.nent;
}

int rv_io_has_backend(const char *name)
{
    return io_find_be(&mgr, name) != NULL;
}

/* テストから、名前の口を読む・出す（ミリ単位） */
int rv_io_get_named(const char *name, int64_t *v)
{
    unsigned long st;
    int r;
    LOCK(st);
    io_vio_now = rv_io_now_ms();
    r = io_lookup(&mgr, name);
    if (r >= 0) {
        r = io_get(&mgr, r, v, io_vio_now);
    }
    UNLOCK(st);
    return r;
}

int rv_io_set_named(const char *name, int64_t v, int32_t speed)
{
    unsigned long st;
    int r;
    LOCK(st);
    io_vio_now = rv_io_now_ms();
    r = io_lookup(&mgr, name);
    if (r >= 0) {
        r = io_set(&mgr, r, v, speed, io_vio_now);
    }
    UNLOCK(st);
    return r;
}

void rv_io_release(void)
{
    unsigned long st;
    LOCK(st);
    io_release(&mgr, rv_io_now_ms());
    UNLOCK(st);
}

void rv_io_stop(int why)
{
    unsigned long st;
    LOCK(st);
    io_vio_now = rv_io_now_ms();
    io_stop(&mgr, why);
    UNLOCK(st);
}

/* テスト用: 記録の数・断った数・いまの決まり */
unsigned long rv_io_logs(void)
{
    return rv_io_log_count;
}

void rv_io_policy(int *gid, int *net_ok)
{
    *gid = mgr.io_gid;
    *net_ok = mgr.net_ok;
}

/* テスト用: /data/log/io.log へすぐ書く */
void rv_io_log_flush(void)
{
    log_save();
}
