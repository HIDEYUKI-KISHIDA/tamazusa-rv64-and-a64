/* linux_evfd.c — eventfd・timerfd・入力のできごと（/dev/input/event*）（S-mode + MMU のみ）
 *
 *   Wayland の下回りの 3 つ目と 4 つ目。Wayland の合成役は、待つものを epoll にまとめる（epoll は linux_fs.c）。
 *     ・eventfd: 数を足す・読み出すだけの fd（スレッドやプロセスの間で「起きて」を伝える）
 *     ・timerfd: 時間が来ると読める fd（キーの繰り返し・アニメーション）
 *     ・/dev/input/event0（キーボード）・event1（マウス・タブレット）: Linux の evdev の形の
 *       struct input_event（24 バイト: 秒・マイクロ秒・型・コード・値）。合成役が直接読む。
 *       キーは Linux のキーの番号（KEY_A = 30 など。USB の HID の番号から換算）、
 *       位置は画面全体（1024×768）の絶対値（ABS_X・ABS_Y）、ボタンは BTN_LEFT など、ホイールは REL_WHEEL。
 *   できごとは装置ごとに 256 個の輪にためる。開いた fd は、それぞれ自分の読む位置を持つ。
 */
#include <stdint.h>
#include <stddef.h>
#include <rv_priv.h>

extern int rv_user_copy_in(void *dst, uintptr_t uva, size_t len);
extern int rv_user_copy_out(uintptr_t udst, const void *src, size_t n);
extern int tk_dis_dsp(void);
extern int tk_ena_dsp(void);
extern int tk_dly_tsk(int ms);
extern int rv_linux_sig_interrupt(void);
extern uint32_t rv_now_unix(void);

#define EINTR   4
#define EAGAIN  11
#define EFAULT  14
#define EINVAL  22
#define EMFILE  24

static uint64_t now_ns(void)
{
    uint64_t t;
    t = RV_RDTIME();
    return t * 100;                     /* QEMU virt の time は 10MHz */
}

/* ---------------------------------------------------------------- eventfd */
#define NEV 16
static struct { int used, refs, sem; uint64_t count; } evs[NEV];

int rv_eventfd_create(unsigned initval, int flags)
{
    int i;
    tk_dis_dsp();
    for (i = 0; i < NEV; i++) {
        if (!evs[i].used) {
            evs[i].used = 1;
            evs[i].refs = 1;
            evs[i].count = initval;
            evs[i].sem = (flags & 1) != 0;      /* EFD_SEMAPHORE */
            tk_ena_dsp();
            return i;
        }
    }
    tk_ena_dsp();
    return -EMFILE;
}

void rv_eventfd_ref(int i) { evs[i].refs++; }
void rv_eventfd_unref(int i)
{
    if (--evs[i].refs <= 0) {
        evs[i].used = 0;
    }
}

long rv_eventfd_read(int i, uintptr_t ubuf, size_t len, int nonblock)
{
    uint64_t v;
    if (len < 8) {
        return -EINVAL;
    }
    for (;;) {
        tk_dis_dsp();
        if (evs[i].count > 0) {
            v = evs[i].sem ? 1 : evs[i].count;
            evs[i].count -= v;
            tk_ena_dsp();
            return rv_user_copy_out(ubuf, &v, 8) == 0 ? 8 : -EFAULT;
        }
        tk_ena_dsp();
        if (nonblock) {
            return -EAGAIN;
        }
        tk_dly_tsk(2);
        if (rv_linux_sig_interrupt()) {
            return -EINTR;
        }
    }
}

long rv_eventfd_write(int i, uintptr_t ubuf, size_t len)
{
    uint64_t v;
    if (len < 8) {
        return -EINVAL;
    }
    if (rv_user_copy_in(&v, ubuf, 8) != 0) {
        return -EFAULT;
    }
    if (v == ~0ULL) {
        return -EINVAL;
    }
    tk_dis_dsp();
    evs[i].count += v;
    tk_ena_dsp();
    return 8;
}

int rv_eventfd_events(int i)
{
    return (evs[i].count > 0 ? 1 : 0) | 4;
}

/* ---------------------------------------------------------------- timerfd */
#define NTM 16
static struct { int used, refs; uint64_t next, interval, ticks; } tms[NTM];

int rv_timerfd_create(int clock)
{
    int i;
    if (clock != 0 && clock != 1 && clock != 7) {   /* REALTIME・MONOTONIC・BOOTTIME */
        return -EINVAL;
    }
    tk_dis_dsp();
    for (i = 0; i < NTM; i++) {
        if (!tms[i].used) {
            tms[i].used = 1;
            tms[i].refs = 1;
            tms[i].next = tms[i].interval = tms[i].ticks = 0;
            tk_ena_dsp();
            return i;
        }
    }
    tk_ena_dsp();
    return -EMFILE;
}

void rv_timerfd_ref(int i) { tms[i].refs++; }
void rv_timerfd_unref(int i)
{
    if (--tms[i].refs <= 0) {
        tms[i].used = 0;
    }
}

/* 時間が来た回数を数え直す */
static void tm_update(int i)
{
    uint64_t now = now_ns();
    if (tms[i].next == 0 || now < tms[i].next) {
        return;
    }
    if (tms[i].interval) {
        uint64_t n = (now - tms[i].next) / tms[i].interval + 1;
        tms[i].ticks += n;
        tms[i].next += n * tms[i].interval;
    } else {
        tms[i].ticks++;
        tms[i].next = 0;
    }
}

static void put_its(int i, int64_t *its)
{
    uint64_t now = now_ns(), left = tms[i].next > now ? tms[i].next - now : (tms[i].next ? 1 : 0);
    its[0] = (int64_t)(tms[i].interval / 1000000000ULL);
    its[1] = (int64_t)(tms[i].interval % 1000000000ULL);
    its[2] = (int64_t)(left / 1000000000ULL);
    its[3] = (int64_t)(left % 1000000000ULL);
}

/* struct itimerspec { interval {sec, nsec}, value {sec, nsec} } */
long rv_timerfd_settime(int i, int flags, uintptr_t unew, uintptr_t uold, int clock)
{
    int64_t its[4], old[4];
    uint64_t val, now = now_ns();
    if (rv_user_copy_in(its, unew, sizeof(its)) != 0) {
        return -EFAULT;
    }
    if (its[1] < 0 || its[1] >= 1000000000 || its[3] < 0 || its[3] >= 1000000000 || its[0] < 0 || its[2] < 0) {
        return -EINVAL;
    }
    tk_dis_dsp();
    tm_update(i);
    put_its(i, old);
    val = (uint64_t)its[2] * 1000000000ULL + (uint64_t)its[3];
    tms[i].interval = (uint64_t)its[0] * 1000000000ULL + (uint64_t)its[1];
    tms[i].ticks = 0;
    if (val == 0) {
        tms[i].next = 0;                        /* 止める */
    } else if (flags & 1) {                     /* TFD_TIMER_ABSTIME */
        uint64_t base = clock == 0 ? (uint64_t)rv_now_unix() * 1000000000ULL : now;
        tms[i].next = val > base ? now + (val - base) : now;
    } else {
        tms[i].next = now + val;
    }
    tk_ena_dsp();
    if (uold && rv_user_copy_out(uold, old, sizeof(old)) != 0) {
        return -EFAULT;
    }
    return 0;
}

long rv_timerfd_gettime(int i, uintptr_t ucur)
{
    int64_t its[4];
    tk_dis_dsp();
    tm_update(i);
    put_its(i, its);
    tk_ena_dsp();
    return rv_user_copy_out(ucur, its, sizeof(its)) == 0 ? 0 : -EFAULT;
}

long rv_timerfd_read(int i, uintptr_t ubuf, size_t len, int nonblock)
{
    uint64_t v;
    if (len < 8) {
        return -EINVAL;
    }
    for (;;) {
        tk_dis_dsp();
        tm_update(i);
        if (tms[i].ticks > 0) {
            v = tms[i].ticks;
            tms[i].ticks = 0;
            tk_ena_dsp();
            return rv_user_copy_out(ubuf, &v, 8) == 0 ? 8 : -EFAULT;
        }
        tk_ena_dsp();
        if (nonblock) {
            return -EAGAIN;
        }
        tk_dly_tsk(1);
        if (rv_linux_sig_interrupt()) {
            return -EINTR;
        }
    }
}

int rv_timerfd_events(int i)
{
    tm_update(i);
    return tms[i].ticks > 0 ? 1 : 0;
}

/* ---------------------------------------------------------------- 入力のできごと（evdev） */
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112
#define REL_WHEEL  8
#define ABS_X 0
#define ABS_Y 1
#define DESK_W 1024
#define DESK_H 768

struct inev { int64_t sec, usec; uint16_t type, code; int32_t value; };
#define NRING 256
static struct { struct inev e[NRING]; uint32_t head; } dev[2];
volatile unsigned long rv_evdev_count[2];

static void push_ev(int d, uint16_t type, uint16_t code, int32_t value)
{
    uint64_t ns = now_ns();
    struct inev *e = &dev[d].e[dev[d].head % NRING];
    e->sec = (int64_t)(ns / 1000000000ULL);
    e->usec = (int64_t)(ns % 1000000000ULL / 1000);
    e->type = type;
    e->code = code;
    e->value = value;
    dev[d].head++;
    rv_evdev_count[d]++;
}

/* USB の HID の番号（0〜0x9f）→ Linux のキーの番号（Linux の drivers/hid/hid-input.c の表と同じ） */
static const uint8_t hid_key[0xa0] = {
      0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
     50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
      4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
     27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
     65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
    105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
     72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
    191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
    115,114,  0,  0,  0,121,  0, 89, 93,124, 92, 94, 95,  0,  0,  0,
    122,123, 90, 91, 85,  0,  0,  0,  0,  0,  0,  0,111,  0,  0,  0,
};
/* 修飾キー（報告の 1 バイト目のビット 0〜7）: 左 Ctrl・Shift・Alt・GUI、右 Ctrl・Shift・Alt・GUI */
static const uint8_t mod_key[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };

/* キーボードの報告（8 バイト）。prev は前の報告 */
void rv_evdev_kbd(const uint8_t *r, const uint8_t *prev)
{
    int i, j, any = 0;
    tk_dis_dsp();
    for (i = 0; i < 8; i++) {
        int was = (prev[0] >> i) & 1, now = (r[0] >> i) & 1;
        if (was != now) {
            push_ev(0, EV_KEY, mod_key[i], now);
            any = 1;
        }
    }
    for (i = 2; i < 8; i++) {                   /* 離した */
        int still = 0;
        if (prev[i] < 4 || prev[i] >= 0xa0) {
            continue;
        }
        for (j = 2; j < 8; j++) {
            still |= r[j] == prev[i];
        }
        if (!still && hid_key[prev[i]]) {
            push_ev(0, EV_KEY, hid_key[prev[i]], 0);
            any = 1;
        }
    }
    for (i = 2; i < 8; i++) {                   /* 押した */
        int was = 0;
        if (r[i] < 4 || r[i] >= 0xa0) {
            continue;
        }
        for (j = 2; j < 8; j++) {
            was |= prev[j] == r[i];
        }
        if (!was && hid_key[r[i]]) {
            push_ev(0, EV_KEY, hid_key[r[i]], 1);
            any = 1;
        }
    }
    if (any) {
        push_ev(0, EV_SYN, 0, 0);
    }
    tk_ena_dsp();
}

/* キーの繰り返し（値 2） */
void rv_evdev_kbd_repeat(uint8_t hid)
{
    if (hid < 0xa0 && hid_key[hid]) {
        tk_dis_dsp();
        push_ev(0, EV_KEY, hid_key[hid], 2);
        push_ev(0, EV_SYN, 0, 0);
        tk_ena_dsp();
    }
}

/* マウス・タブレット: 画面全体の位置・ボタン（ビット 0 左、1 右、2 中）・ホイール */
void rv_evdev_ptr(int x, int y, int buttons, int wheel)
{
    static int px = -1, py = -1, pb;
    static const uint16_t btn[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    int k, any = 0;
    tk_dis_dsp();
    if (x != px) {
        push_ev(1, EV_ABS, ABS_X, x);
        px = x;
        any = 1;
    }
    if (y != py) {
        push_ev(1, EV_ABS, ABS_Y, y);
        py = y;
        any = 1;
    }
    for (k = 0; k < 3; k++) {
        if (((buttons ^ pb) >> k) & 1) {
            push_ev(1, EV_KEY, btn[k], (buttons >> k) & 1);
            any = 1;
        }
    }
    pb = buttons;
    if (wheel) {
        push_ev(1, EV_REL, REL_WHEEL, wheel);
        any = 1;
    }
    if (any) {
        push_ev(1, EV_SYN, 0, 0);
    }
    tk_ena_dsp();
}

/* 開いたときの読む位置（いまの頭から） */
uint32_t rv_evdev_open(int d)
{
    return dev[d & 1].head;
}

/* 読む: *pos から、できごとを len / 24 個まで。読めたバイト数か負のエラー。古すぎる位置は追いつかせる */
long rv_evdev_read(int d, uint64_t *pos, uintptr_t ubuf, size_t len, int nonblock)
{
    size_t n = 0;
    d &= 1;
    if (len < sizeof(struct inev)) {
        return -EINVAL;
    }
    for (;;) {
        uint32_t p = (uint32_t)*pos;
        tk_dis_dsp();
        if (dev[d].head - p > NRING) {
            p = dev[d].head - NRING;            /* あふれた分は捨てる */
        }
        if (p != dev[d].head) {
            struct inev tmp[16];
            while (p != dev[d].head && n < 16 && (n + 1) * sizeof(struct inev) <= len) {
                tmp[n++] = dev[d].e[p % NRING];
                p++;
            }
            *pos = p;
            tk_ena_dsp();
            return rv_user_copy_out(ubuf, tmp, n * sizeof(struct inev)) == 0 ? (long)(n * sizeof(struct inev)) : -EFAULT;
        }
        tk_ena_dsp();
        if (nonblock) {
            return -EAGAIN;
        }
        tk_dly_tsk(5);
        if (rv_linux_sig_interrupt()) {
            return -EINTR;
        }
    }
}

int rv_evdev_events(int d, uint64_t pos)
{
    return dev[d & 1].head != (uint32_t)pos ? 1 : 0;
}

/* ioctl（EVIOC…）。扱ったら 1（*r に結果） */
int rv_evdev_ioctl(int d, unsigned long cmd, uintptr_t arg, long *r)
{
    unsigned nr = cmd & 0xff, type = (cmd >> 8) & 0xff, size = (cmd >> 16) & 0x3fff;
    uint8_t bits[96];
    unsigned k;
    d &= 1;
    if (type != 'E') {
        return 0;
    }
    for (k = 0; k < sizeof(bits); k++) {
        bits[k] = 0;
    }
    if (nr == 0x01) {                           /* EVIOCGVERSION */
        int32_t v = 0x010001;
        *r = rv_user_copy_out(arg, &v, 4) == 0 ? 0 : -EFAULT;
        return 1;
    }
    if (nr == 0x02) {                           /* EVIOCGID: bustype（USB 3）・vendor・product・version */
        uint16_t id[4] = { 3, 0x0627, (uint16_t)(d ? 0x0002 : 0x0001), 1 };
        *r = rv_user_copy_out(arg, id, 8) == 0 ? 0 : -EFAULT;
        return 1;
    }
    if (nr == 0x06) {                           /* EVIOCGNAME(len) */
        const char *nm = d ? "Tamazusa USB pointer" : "Tamazusa USB keyboard";
        unsigned n = 0;
        while (nm[n]) n++;
        n++;
        if (n > size) n = size;
        *r = rv_user_copy_out(arg, nm, n) == 0 ? (long)n : -EFAULT;
        return 1;
    }
    if (nr == 0x07 || nr == 0x08) {             /* EVIOCGPHYS・EVIOCGUNIQ */
        *r = rv_user_copy_out(arg, "", size ? 1 : 0) == 0 ? (size ? 1 : 0) : -EFAULT;
        return 1;
    }
    if (nr == 0x09 || nr == 0x18 || nr == 0x19 || nr == 0x1a || nr == 0x1b) {
        /* EVIOCGPROP・EVIOCGKEY・EVIOCGLED・EVIOCGSND・EVIOCGSW: 何も立っていない */
        if (size > sizeof(bits)) size = sizeof(bits);
        *r = rv_user_copy_out(arg, bits, size) == 0 ? (long)size : -EFAULT;
        return 1;
    }
    if (nr >= 0x20 && nr < 0x40) {              /* EVIOCGBIT(ev, len) */
        unsigned ev = nr - 0x20;
        if (ev == 0) {
            bits[0] = (1 << EV_SYN) | (1 << EV_KEY) | (d ? (1 << EV_REL) | (1 << EV_ABS) : 0);
        } else if (ev == EV_KEY) {
            if (d) {
                bits[BTN_LEFT / 8] |= 7 << (BTN_LEFT % 8);
            } else {
                for (k = 4; k < 0xa0; k++) {
                    if (hid_key[k]) bits[hid_key[k] / 8] |= (uint8_t)(1 << (hid_key[k] % 8));
                }
                for (k = 0; k < 8; k++) {
                    bits[mod_key[k] / 8] |= (uint8_t)(1 << (mod_key[k] % 8));
                }
            }
        } else if (ev == EV_REL && d) {
            bits[REL_WHEEL / 8] = 1 << (REL_WHEEL % 8);
        } else if (ev == EV_ABS && d) {
            bits[0] = (1 << ABS_X) | (1 << ABS_Y);
        }
        if (size > sizeof(bits)) size = sizeof(bits);
        *r = rv_user_copy_out(arg, bits, size) == 0 ? (long)size : -EFAULT;
        return 1;
    }
    if (nr >= 0x40 && nr < 0x80 && d) {         /* EVIOCGABS(abs): value・min・max・fuzz・flat・resolution */
        int32_t a[6] = { 0, 0, 0, 0, 0, 0 };
        unsigned axis = nr - 0x40;
        if (axis == ABS_X) { a[2] = DESK_W - 1; }
        else if (axis == ABS_Y) { a[2] = DESK_H - 1; }
        else { *r = -EINVAL; return 1; }
        *r = rv_user_copy_out(arg, a, size < sizeof(a) ? size : sizeof(a)) == 0 ? 0 : -EFAULT;
        return 1;
    }
    if (nr == 0x90 || nr == 0x91 || nr == 0xa0) { /* EVIOCGRAB・EVIOCREVOKE・EVIOCSCLOCKID: 受け付けるだけ */
        *r = 0;
        return 1;
    }
    *r = -EINVAL;
    return 1;
}
