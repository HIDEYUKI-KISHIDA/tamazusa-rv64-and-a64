/* kernel/io/som_pins.c — SoM のピン（GPIO・PWM）の中身「som」
 *
 *   SoM ごとに、ピンを動かす装置（IP）とその番地は違う。それはデバイスツリーに書いてあるので、起動のときに読んで、
 *   知っている装置（compatible の名前）を見つけたら使う。プログラムと /etc/io.conf は SoM が替わっても同じ。
 *   いま知っている装置（多くの SoC で使われている、よくある IP）:
 *     snps,dw-apb-gpio   Synopsys DesignWare の APB GPIO（ポート A〜D。子の節 snps,dw-apb-gpio-port ごとに）
 *     sifive,gpio0       SiFive の GPIO
 *     sifive,pwm0        SiFive の PWM（1 つに 4 チャンネル、16 ビット。周期はブロックで共通）
 *   SoM にほかの装置があれば、ここにドライバを足す（デバイスツリーの compatible で選ぶので、ほかはそのまま）。
 *
 *   番号: som.pinN は、デバイスツリーに書かれた順に GPIO の線を通しで数えた番号。som.pwmN・som.servoN も PWM のチャンネルを通しで。
 *   PWM のブロックは、初めに使った種類で周期が決まる（PWM は 1 ミリ秒前後、サーボは 20 ミリ秒）。同じブロックで混ぜるとエラー。
 *   サーボは 0〜180 度を、パルス幅 544〜2400 マイクロ秒にする（Arduino の Servo と同じ）。速さは io_task が 10 ミリ秒ごとに少しずつ。
 *   注意: 実機ではまだ確かめていない（QEMU virt にこれらの装置はない）。PWM の出力の向き（どちらが「入」か）は
 *   装置の説明書の読みに基づく。逆なら /etc/io.conf に invert と書く。
 */
#include <stdint.h>
#include <stddef.h>
#define IOMGR_TYPES_ONLY
#include "iomgr_core.h"
#include "rv_priv.h"
#include "fdt.h"

extern void uart_puts(const char *s);
extern void iomgr_add_backend(IoBackend *b);
extern uint64_t rv_boot_dtb;

#define MAXCTL   16
#define MAXLINE  256
#define MAXBLK   8
#define MAXCHAN  (MAXBLK * 4)

enum { C_DW, C_SIFIVE };
enum { PM_NONE, PM_PWM, PM_SERVO };

/* SiFive の PWM のレジスタ */
#define PWMCFG          0x00
#define PWMCMP(i)       (0x20 + 4 * (i))
#define PWMCFG_ENALWAYS (1u << 12)

/* SiFive の GPIO のレジスタ */
#define SG_INPUT_VAL    0x00
#define SG_INPUT_EN     0x04
#define SG_OUTPUT_EN    0x08
#define SG_OUTPUT_VAL   0x0c

struct gpio_ctl { int type; uintptr_t base; int port; int first, n; };
struct pwm_blk { uintptr_t base; uint32_t clk; int first; int mode; unsigned scale; uint64_t period_ns; };

struct som {
    struct gpio_ctl g[MAXCTL];
    int ng, nlines;
    struct pwm_blk p[MAXBLK];
    int np, nchan;
    uint8_t dir[MAXLINE];               /* 0 まだ 1 入力 2 出力 */
    int64_t pwm_val[MAXCHAN];
    int64_t servo_cur[MAXCHAN], servo_tgt[MAXCHAN];
    int32_t servo_spd[MAXCHAN];
    uint8_t chan_kind[MAXCHAN];
    /* 探しているあいだ: DesignWare の親の節 */
    int dw_off[MAXCTL];
    uintptr_t dw_base[MAXCTL];
    int ndw;
};

static struct som som_boot, som_test;
static IoBackend be_boot, be_test;

static uintptr_t map(uint64_t pa)
{
    return pa >= 0x80000000UL ? (uintptr_t)pa : RV_IO(pa);   /* RAM はそのまま、装置は高いアドレス */
}

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

/* ---------------------------------------------------------------- デバイスツリーから探す */
static int probe_cb(const void *fdt, const struct fdt_node *n, void *arg)
{
    struct som *S = (struct som *)arg;
    uint64_t a, sz;
    int i;

    if (!fdt_enabled(fdt, n->off)) {
        return 0;
    }
    if (fdt_compatible(fdt, n->off, "snps,dw-apb-gpio") && S->ndw < MAXCTL && fdt_reg(fdt, n, 0, &a, &sz)) {
        S->dw_off[S->ndw] = n->off;
        S->dw_base[S->ndw] = map(a);
        S->ndw++;
    } else if (fdt_compatible(fdt, n->off, "snps,dw-apb-gpio-port") && S->ng < MAXCTL) {
        for (i = 0; i < S->ndw; i++) {
            if (S->dw_off[i] == n->parent) {
                uint64_t port = 0;
                int lines = (int)fdt_getu32(fdt, n->off, "ngpios",
                                            fdt_getu32(fdt, n->off, "snps,nr-gpios", 32));
                fdt_reg(fdt, n, 0, &port, NULL);
                if (port > 3 || lines < 1 || lines > 32 || S->nlines + lines > MAXLINE) {
                    break;
                }
                S->g[S->ng].type = C_DW;
                S->g[S->ng].base = S->dw_base[i];
                S->g[S->ng].port = (int)port;
                S->g[S->ng].first = S->nlines;
                S->g[S->ng].n = lines;
                S->nlines += lines;
                S->ng++;
                break;
            }
        }
    } else if (fdt_compatible(fdt, n->off, "sifive,gpio0") && S->ng < MAXCTL && fdt_reg(fdt, n, 0, &a, &sz)) {
        int lines = (int)fdt_getu32(fdt, n->off, "ngpios", 16);
        if (lines >= 1 && lines <= 32 && S->nlines + lines <= MAXLINE) {
            S->g[S->ng].type = C_SIFIVE;
            S->g[S->ng].base = map(a);
            S->g[S->ng].port = 0;
            S->g[S->ng].first = S->nlines;
            S->g[S->ng].n = lines;
            S->nlines += lines;
            S->ng++;
        }
    } else if (fdt_compatible(fdt, n->off, "sifive,pwm0") && S->np < MAXBLK && fdt_reg(fdt, n, 0, &a, &sz)) {
        int clk = fdt_by_phandle(fdt, fdt_getu32(fdt, n->off, "clocks", 0));
        struct pwm_blk *b = &S->p[S->np++];
        b->base = map(a);
        b->clk = clk >= 0 ? fdt_getu32(fdt, clk, "clock-frequency", 0) : 0;
        b->first = S->nchan;
        b->mode = PM_NONE;
        b->scale = 0;
        b->period_ns = 0;
        S->nchan += 4;
    }
    return 0;
}

/* ---------------------------------------------------------------- GPIO */
static struct gpio_ctl *line_ctl(struct som *S, int line, uint32_t *bit)
{
    int i;
    for (i = 0; i < S->ng; i++) {
        if (line >= S->g[i].first && line < S->g[i].first + S->g[i].n) {
            *bit = 1u << (line - S->g[i].first);
            return &S->g[i];
        }
    }
    return NULL;
}

static void gpio_dir(struct gpio_ctl *c, uint32_t bit, int out)
{
    if (c->type == C_DW) {
        uintptr_t ddr = c->base + 0x04 + 0x0c * (unsigned)c->port;
        wr(ddr, out ? (rd(ddr) | bit) : (rd(ddr) & ~bit));
    } else {
        wr(c->base + SG_OUTPUT_EN, out ? (rd(c->base + SG_OUTPUT_EN) | bit) : (rd(c->base + SG_OUTPUT_EN) & ~bit));
        if (!out) {
            wr(c->base + SG_INPUT_EN, rd(c->base + SG_INPUT_EN) | bit);
        }
    }
}

static void gpio_write(struct gpio_ctl *c, uint32_t bit, int v)
{
    uintptr_t r = c->type == C_DW ? c->base + 0x0c * (unsigned)c->port : c->base + SG_OUTPUT_VAL;
    wr(r, v ? (rd(r) | bit) : (rd(r) & ~bit));
}

static int gpio_read(struct gpio_ctl *c, uint32_t bit, int out)
{
    uintptr_t r;
    if (c->type == C_DW) {
        r = out ? c->base + 0x0c * (unsigned)c->port : c->base + 0x50 + 4 * (unsigned)c->port;
    } else {
        r = c->base + (out ? SG_OUTPUT_VAL : SG_INPUT_VAL);
    }
    return (rd(r) & bit) != 0;
}

/* ---------------------------------------------------------------- PWM */
static struct pwm_blk *chan_blk(struct som *S, int ch)
{
    if (ch < 0 || ch >= S->nchan) {
        return NULL;
    }
    return &S->p[ch / 4];
}

/* ブロックの周期を決める。PWM: 1 ミリ秒以上で最も短く。サーボ: 20 ミリ秒以上で最も短く（時計がわからなければできない） */
static int blk_mode(struct pwm_blk *b, int mode)
{
    uint64_t want;
    unsigned s;
    if (b->mode == mode) {
        return IO_OK;
    }
    if (b->mode != PM_NONE) {
        return IO_EKIND;                /* 同じブロックで PWM とサーボは混ぜられない */
    }
    if (mode == PM_SERVO && b->clk == 0) {
        return IO_ENODEV;
    }
    want = mode == PM_SERVO ? (uint64_t)b->clk / 50 : (uint64_t)b->clk / 1000;   /* 必要な数え数 */
    for (s = 0; s < 15 && ((uint64_t)65536 << s) < want; s++) {
    }
    b->scale = s;
    b->period_ns = b->clk ? ((uint64_t)65536 << s) * 1000000000ULL / b->clk : 0;
    b->mode = mode;
    wr(b->base + PWMCFG, PWMCFG_ENALWAYS | s);
    return IO_OK;
}

/* 入っている割合（0〜65536 分の 1）を出す。数えた値が比べる値以上のあいだ出力が入る、として比べる値にする */
static void pwm_frac(struct pwm_blk *b, int ch, uint32_t on_frac)
{
    uint32_t cmp = on_frac >= 65536 ? 0 : 65536 - on_frac;
    if (cmp > 65535) {
        cmp = 65535;
    }
    wr(b->base + PWMCMP(ch % 4), cmp);
}

static void servo_out(struct som *S, int ch, int64_t angle_m)
{
    struct pwm_blk *b = chan_blk(S, ch);
    uint64_t pulse_ns = 544000 + (uint64_t)angle_m * (2400000 - 544000) / 180000;
    pwm_frac(b, ch, (uint32_t)(pulse_ns * 65536 / b->period_ns));
}

/* ---------------------------------------------------------------- 中身の口 */
static int som_get(void *ctx, int kind, int chan, int64_t *raw_m)
{
    struct som *S = (struct som *)ctx;
    uint32_t bit;
    struct gpio_ctl *c;
    switch (kind) {
    case IO_K_PIN:
        c = chan >= 0 && chan < S->nlines ? line_ctl(S, chan, &bit) : NULL;
        if (!c) {
            return IO_ENODEV;
        }
        *raw_m = gpio_read(c, bit, S->dir[chan] == 2) * 1000;
        return IO_OK;
    case IO_K_PWM:
        if (!chan_blk(S, chan)) {
            return IO_ENODEV;
        }
        *raw_m = S->pwm_val[chan];
        return IO_OK;
    case IO_K_SERVO:
        if (!chan_blk(S, chan)) {
            return IO_ENODEV;
        }
        *raw_m = S->servo_cur[chan];
        return IO_OK;
    }
    return IO_ENODEV;                   /* ADC はまだない */
}

static int som_set(void *ctx, int kind, int chan, int64_t raw_m, int32_t speed)
{
    struct som *S = (struct som *)ctx;
    uint32_t bit;
    struct gpio_ctl *c;
    struct pwm_blk *b;
    int r;
    switch (kind) {
    case IO_K_PIN:
        c = chan >= 0 && chan < S->nlines ? line_ctl(S, chan, &bit) : NULL;
        if (!c) {
            return IO_ENODEV;
        }
        gpio_write(c, bit, raw_m != 0);             /* 先に値、それから出力に（一瞬でも逆の値を出さない） */
        if (S->dir[chan] != 2) {
            gpio_dir(c, bit, 1);
            S->dir[chan] = 2;
        }
        return IO_OK;
    case IO_K_PWM:
        b = chan_blk(S, chan);
        if (!b) {
            return IO_ENODEV;
        }
        if (raw_m < 0 || raw_m > 100000) {
            return IO_ERANGE;
        }
        r = blk_mode(b, PM_PWM);
        if (r != IO_OK) {
            return r;
        }
        S->pwm_val[chan] = raw_m;
        S->chan_kind[chan] = PM_PWM;
        pwm_frac(b, chan, (uint32_t)(raw_m * 65536 / 100000));
        return IO_OK;
    case IO_K_SERVO:
        b = chan_blk(S, chan);
        if (!b) {
            return IO_ENODEV;
        }
        if (raw_m < 0 || raw_m > 180000) {
            return IO_ERANGE;
        }
        r = blk_mode(b, PM_SERVO);
        if (r != IO_OK) {
            return r;
        }
        S->chan_kind[chan] = PM_SERVO;
        S->servo_tgt[chan] = raw_m;
        S->servo_spd[chan] = speed;
        if (speed <= 0) {
            S->servo_cur[chan] = raw_m;
            servo_out(S, chan, raw_m);
        }
        return IO_OK;
    }
    return IO_ENODEV;
}

static void som_setup(void *ctx, int kind, int chan, int output)
{
    struct som *S = (struct som *)ctx;
    uint32_t bit;
    struct gpio_ctl *c;
    if (kind == IO_K_PIN && !output && chan >= 0 && chan < S->nlines && (c = line_ctl(S, chan, &bit)) != NULL) {
        gpio_dir(c, bit, 0);            /* 入力は起動のときに入力にしておく。出力は初めて出すときに */
        S->dir[chan] = 1;
    }
}

static void som_safe(void *ctx)
{
    struct som *S = (struct som *)ctx;
    int i;
    uint32_t bit;
    for (i = 0; i < S->nlines; i++) {
        struct gpio_ctl *c;
        if (S->dir[i] == 2 && (c = line_ctl(S, i, &bit)) != NULL) {
            gpio_write(c, bit, 0);
        }
    }
    for (i = 0; i < S->nchan; i++) {
        if (S->chan_kind[i] == PM_PWM && S->pwm_val[i]) {
            S->pwm_val[i] = 0;
            pwm_frac(chan_blk(S, i), i, 0);
        } else if (S->chan_kind[i] == PM_SERVO) {
            S->servo_tgt[i] = S->servo_cur[i];      /* その場で止める */
        }
    }
}

static void som_tick(void *ctx, int ms)
{
    struct som *S = (struct som *)ctx;
    int i;
    for (i = 0; i < S->nchan; i++) {
        if (S->chan_kind[i] == PM_SERVO && S->servo_cur[i] != S->servo_tgt[i]) {
            int64_t step = S->servo_spd[i] > 0 ? (int64_t)S->servo_spd[i] * ms : 180000;
            int64_t d = S->servo_tgt[i] - S->servo_cur[i];
            S->servo_cur[i] += d > step ? step : d < -step ? -step : d;
            servo_out(S, i, S->servo_cur[i]);
        }
    }
}

static void make_backend(IoBackend *b, struct som *S)
{
    b->name = "som";
    b->ctx = S;
    b->get = som_get;
    b->set = som_set;
    b->setup = som_setup;
    b->safe = som_safe;
    b->tick = som_tick;
}

static void zero(void *p, size_t n)
{
    uint8_t *q = (uint8_t *)p;
    while (n--) {
        *q++ = 0;
    }
}

static int probe(const void *fdt, struct som *S)
{
    zero(S, sizeof(*S));
    if (!fdt_valid(fdt)) {
        return 0;
    }
    fdt_walk(fdt, probe_cb, S);
    return S->ng + S->np;
}

static void put_dec(int v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i > 0);
    uart_puts(&b[i]);
}

/* 起動のとき: OpenSBI から渡されたデバイスツリーを読む */
void som_pins_init(void)
{
    const void *fdt = (const void *)(uintptr_t)rv_boot_dtb;
    if (!fdt_valid(fdt)) {
        uart_puts("[som] no device tree\r\n");
        return;
    }
    if (probe(fdt, &som_boot) == 0) {
        uart_puts("[som] no GPIO / PWM in the device tree (QEMU virt has none)\r\n");
        return;
    }
    uart_puts("[som] pins: ");
    put_dec(som_boot.nlines);
    uart_puts(" GPIO lines, ");
    put_dec(som_boot.nchan);
    uart_puts(" PWM channels\r\n");
    make_backend(&be_boot, &som_boot);
    iomgr_add_backend(&be_boot);
}

/* テスト用: 作ったデバイスツリーで探し、中身を返す（入出力の管理には加えない） */
IoBackend *rv_som_test_probe(const void *fdt, int *lines, int *chans)
{
    probe(fdt, &som_test);
    make_backend(&be_test, &som_test);
    *lines = som_test.nlines;
    *chans = som_test.nchan;
    return &be_test;
}

/* テスト用: 本物のデバイスツリーを読めるか（QEMU virt の UART・PLIC を探す） */
struct dt_count { int uart, plic, virtio; uint64_t uart_addr; };

static int count_cb(const void *fdt, const struct fdt_node *n, void *arg)
{
    struct dt_count *c = (struct dt_count *)arg;
    uint64_t a;
    if ((fdt_compatible(fdt, n->off, "ns16550a") || fdt_compatible(fdt, n->off, "arm,pl011"))
        && fdt_reg(fdt, n, 0, &a, NULL)) {
        c->uart++;
        c->uart_addr = a;
    }
    if (fdt_compatible(fdt, n->off, "riscv,plic0") || fdt_compatible(fdt, n->off, "sifive,plic-1.0.0")
        || fdt_compatible(fdt, n->off, "arm,gic-v3")) {     /* 割込みコントローラ（AArch64 は GICv3） */
        c->plic++;
    }
    if (fdt_compatible(fdt, n->off, "virtio,mmio")) {
        c->virtio++;
    }
    return 0;
}

int rv_dt_selftest(uint64_t *uart_addr, int *virtio)
{
    const void *fdt = (const void *)(uintptr_t)rv_boot_dtb;
    struct dt_count c;
    zero(&c, sizeof(c));
    if (!fdt_valid(fdt)) {
        return 0;
    }
    fdt_walk(fdt, count_cb, &c);
    *uart_addr = c.uart_addr;
    *virtio = c.virtio;
    return c.uart >= 1 && c.plic >= 1;
}
