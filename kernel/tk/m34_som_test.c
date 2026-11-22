/* kernel/tk/m34_som_test.c — M34: SoM のピンのドライバ（デバイスツリーで選ぶ）（SMODE=1 かつ MMU=1）
 *
 *   QEMU virt には GPIO・PWM の装置がないので、メモリの上に「偽のレジスタ」を置き、それを指すデバイスツリーを作って試す。
 *   1. 本物のデバイスツリー（OpenSBI から渡されたもの）を読める: UART（0x1000_0000）・PLIC・virtio を見つける
 *   2. 作ったデバイスツリーから、DesignWare の GPIO（ポート 2 つ）・SiFive の GPIO・SiFive の PWM（2 ブロック）を見つけ、
 *      止めてある（status = "disabled"）装置は飛ばす: GPIO 28 本、PWM 8 チャンネル
 *   3. 名前の口（自前の /etc/io.conf）で、出力・入力・invert・PWM・サーボ（速さと上限）・同じブロックでの混ぜ・止めるときを、
 *      レジスタの値で確かめる
 * 実機の装置の振る舞い（出力の向きなど）は、ここでは確かめられない。
 *
 * 合格の目印: [M34] PASS
 */
#include <stdint.h>
#include <stddef.h>
#pragma GCC diagnostic ignored "-Wunused-function"     /* 共通の管理の一部だけを使う */
#include "iomgr_core.h"

extern void uart_puts(const char *s);
extern IoBackend *rv_som_test_probe(const void *fdt, int *lines, int *chans);
extern int rv_dt_selftest(uint64_t *uart_addr, int *virtio);

static uint32_t fake[5][64] __attribute__((aligned(256)));      /* 偽のレジスタ: DW・SiFive GPIO・止めた GPIO・PWM 2 つ */

/* ---- 小さなデバイスツリーを作る */
static uint8_t dtb[2048] __attribute__((aligned(8)));
static uint8_t st_buf[1024], str_buf[512];
static unsigned st_n, str_n;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void tok(uint32_t v)
{
    put32(st_buf + st_n, v);
    st_n += 4;
}

static void raw(const void *p, unsigned n)
{
    const uint8_t *q = (const uint8_t *)p;
    unsigned i;
    for (i = 0; i < n; i++) {
        st_buf[st_n++] = q[i];
    }
    while (st_n & 3) {
        st_buf[st_n++] = 0;
    }
}

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void begin(const char *name)
{
    tok(1);
    raw(name, slen(name) + 1);
}

static void prop(const char *name, const void *v, unsigned len)
{
    unsigned i, off = str_n;
    for (i = 0; i <= slen(name); i++) {
        str_buf[str_n++] = (uint8_t)name[i];
    }
    tok(3);
    tok(len);
    tok(off);
    raw(v, len);
}

static void prop_u32(const char *name, uint32_t v)
{
    uint8_t b[4];
    put32(b, v);
    prop(name, b, 4);
}

static void prop_reg64(const char *name, uintptr_t a, uint32_t size)
{
    uint8_t b[16];
    put32(b, (uint32_t)((uint64_t)a >> 32)); put32(b + 4, (uint32_t)a);
    put32(b + 8, 0); put32(b + 12, size);
    prop(name, b, 16);
}

static void prop_str(const char *name, const char *s, unsigned len)
{
    prop(name, s, len);
}

static void end(void)
{
    tok(2);
}

static void build(void)
{
    unsigned i, total;
    st_n = str_n = 0;
    begin("");
    prop_u32("#address-cells", 2);
    prop_u32("#size-cells", 2);
      begin("clock");
      prop_str("compatible", "fixed-clock", 12);
      prop_u32("clock-frequency", 100000000);
      prop_u32("phandle", 1);
      end();
      begin("gpio@a");
      prop_str("compatible", "snps,dw-apb-gpio", 17);
      prop_reg64("reg", (uintptr_t)fake[0], 0x100);
      prop_u32("#address-cells", 1);
      prop_u32("#size-cells", 0);
        begin("gpio-port@0");
        prop_str("compatible", "snps,dw-apb-gpio-port", 22);
        prop_u32("reg", 0);
        prop_u32("ngpios", 8);
        end();
        begin("gpio-port@1");
        prop_str("compatible", "snps,dw-apb-gpio-port", 22);
        prop_u32("reg", 1);
        prop_u32("snps,nr-gpios", 4);
        end();
      end();
      begin("gpio@b");
      prop_str("compatible", "sifive,fu540-c000-gpio\0sifive,gpio0", 36);
      prop_reg64("reg", (uintptr_t)fake[1], 0x100);
      prop_u32("ngpios", 16);
      end();
      begin("gpio@c");
      prop_str("compatible", "sifive,gpio0", 13);
      prop_str("status", "disabled", 9);
      prop_reg64("reg", (uintptr_t)fake[2], 0x100);
      end();
      begin("pwm@d");
      prop_str("compatible", "sifive,fu540-c000-pwm\0sifive,pwm0", 34);
      prop_reg64("reg", (uintptr_t)fake[3], 0x100);
      prop_u32("clocks", 1);
      end();
      begin("pwm@e");
      prop_str("compatible", "sifive,pwm0", 12);
      prop_reg64("reg", (uintptr_t)fake[4], 0x100);
      prop_u32("clocks", 1);
      end();
    end();
    tok(9);
    for (i = 0; i < sizeof(dtb); i++) {
        dtb[i] = 0;
    }
    total = 40 + 16 + st_n + str_n;
    put32(dtb, 0xd00dfeed);
    put32(dtb + 4, total);
    put32(dtb + 8, 56);                 /* 構造 */
    put32(dtb + 12, 56 + st_n);         /* 文字列 */
    put32(dtb + 16, 40);                /* 予約の表（空） */
    put32(dtb + 20, 17);
    put32(dtb + 24, 16);
    put32(dtb + 32, str_n);
    put32(dtb + 36, st_n);
    for (i = 0; i < st_n; i++) {
        dtb[56 + i] = st_buf[i];
    }
    for (i = 0; i < str_n; i++) {
        dtb[56 + st_n + i] = str_buf[i];
    }
}

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M34] OK   " : "[M34] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

static char conf[] =
    "SLED   = som.pin3\n"
    "SIN    = som.pin9   input\n"
    "SBTN   = gpio14     input\n"
    "SRELAY = gpio20     invert\n"
    "SDIM   = som.pwm1\n"
    "SSRV   = som.servo4  max 170  speed 90\n"
    "SBAD   = som.servo2\n";

int m34_run_som_test(void)
{
    static IoMgr m;
    IoBackend *b;
    int pass = 1, lines = 0, chans = 0, virtio = 0, i, r1, r2;
    uint64_t uart = 0;
    int64_t v1 = 0, v2 = 0, mid = 0, fin = 0;

    uart_puts("\r\n[M34] SoM pin drivers chosen by the device tree\r\n");
#if defined(__aarch64__)
    pass &= check(rv_dt_selftest(&uart, &virtio) && uart == 0x09000000 && virtio == 32,
                  "real device tree from QEMU: UART (PL011) at 0x09000000, GICv3, 32 virtio-mmio");
#else
    pass &= check(rv_dt_selftest(&uart, &virtio) && uart == 0x10000000 && virtio == 8,
                  "real device tree from OpenSBI: UART at 0x10000000, PLIC, 8 virtio-mmio");
#endif

    build();
    b = rv_som_test_probe(dtb, &lines, &chans);
    pass &= check(lines == 28 && chans == 8,
                  "test tree: DesignWare GPIO (2 ports: 8+4), SiFive GPIO (16), disabled one skipped, SiFive PWM x2 (8 channels)");

    io_init(&m);
    io_parse_conf(&m, conf, sizeof(conf) - 1);
    io_add_backend(&m, b);              /* 入力の向きを決め、止めた状態（invert は切れる側）にする */
    pass &= check((fake[0][0x10 / 4] & 2) == 0 && (fake[1][1] & 4) && (fake[1][2] & (1u << 8)) && (fake[1][3] & (1u << 8)),
                  "setup: inputs set as input; invert output (SRELAY) driven to its off side (high)");
    io_release(&m, 0);

    io_set(&m, io_lookup(&m, "SLED"), 1000, 0, 0);
    pass &= check((fake[0][0] & 8) && (fake[0][1] & 8), "DesignWare port A: SLED (line 3) output high (DR, DDR)");

    fake[0][0x54 / 4] |= 2;             /* ポート B の入力（EXT_PORTB）の 1 番を立てる */
    fake[1][0] |= 4;                    /* SiFive の input_val の 2 番 */
    io_get(&m, io_lookup(&m, "SIN"), &v1, 0);
    io_get(&m, io_lookup(&m, "SBTN"), &v2, 0);
    pass &= check(v1 == 1000 && v2 == 1000, "inputs: DesignWare port B (line 9), SiFive (line 14)");

    io_set(&m, io_lookup(&m, "SRELAY"), 1000, 0, 0);
    pass &= check((fake[1][3] & (1u << 8)) == 0, "invert: SRELAY on drives the line low");

    io_set(&m, io_lookup(&m, "SDIM"), 25000, 0, 0);
    pass &= check(fake[3][0] == 0x1001 && fake[3][0x24 / 4] == 49152,
                  "SiFive PWM: 1 kHz-ish (scale 1 at 100 MHz), 25% on channel 1 (cmp 49152)");

    io_set(&m, io_lookup(&m, "SSRV"), 90000, 0, 0);
    for (i = 0; i < 50; i++) {
        io_tick_backends(&m, 10);
    }
    io_get(&m, io_lookup(&m, "SSRV"), &mid, 0);
    for (i = 0; i < 60; i++) {
        io_tick_backends(&m, 10);
    }
    io_get(&m, io_lookup(&m, "SSRV"), &fin, 0);
    r1 = io_set(&m, io_lookup(&m, "SSRV"), 175000, 0, 0);
    r2 = io_set(&m, io_lookup(&m, "SBAD"), 10000, 0, 0);
    pass &= check(fake[4][0] == 0x1005 && mid == 45000 && fin == 90000 && fake[4][0x20 / 4] == 60936
                  && r1 == IO_ERANGE && r2 == IO_EKIND,
                  "servo on PWM block 2: 20 ms period, io.conf speed 90 deg/s (45 at 0.5 s, 90 at 1.1 s), "
                  "1.472 ms pulse; max 170 refused; servo in a PWM block refused");

    io_stop(&m, IO_STOPPED);
    pass &= check((fake[0][0] & 8) == 0 && fake[3][0x24 / 4] == 65535 && (fake[1][3] & (1u << 8)),
                  "stop: SLED low, PWM 0%, invert SRELAY back to its off side");

    uart_puts(pass ? "[M34] PASS\r\n" : "[M34] FAIL\r\n");
    return pass;
}
