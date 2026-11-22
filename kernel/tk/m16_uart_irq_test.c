/* kernel/tk/m16_uart_irq_test.c — M16: UART の受信割込み（PLIC）（SMODE=1 かつ MMU=1）
 *
 *   UART を折り返し（ループバック）にして、送った文字を自分で受け取る。
 *   キーボードがなくても、割込みで文字が届くことを自動で確かめられる。
 *   1. 入力待ちのタスク R（優先度 5）が rv_uart_getc_wait で眠る
 *   2. init が「tz!」の 3 文字を送る → 受信割込みで R が起き、3 文字を順に受け取る
 *   3. 割込みの回数が増えている（見に行く方式ではなく割込みで届いた）
 *   4. 送ってから R が受け取るまでが短い（以前の 10ms おきの確認より速い）
 *
 * 合格の目印: [M16] PASS
 */
#include <stdint.h>
#include <tk/tkernel.h>
#include <libstr.h>
#include <rv_priv.h>

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern int  rv_uart_getc_wait(void);
extern void rv_uart_loopback(int on);
extern volatile unsigned long rv_uart_irq_count;

#define TICKS_PER_US    10UL        /* QEMU virt の time CSR は 10MHz */

static volatile char g_got[4];
static volatile int  g_n;
static volatile unsigned long g_t_first;

static unsigned long rdtime_now(void)
{
    unsigned long t;
    t = RV_RDTIME();
    return t;
}

static void put_num(unsigned long v)
{
    char buf[24];
    int n = 0;

    do {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        uart_putc(buf[--n]);
    }
}

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M16] OK   " : "[M16] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

static void task_reader(INT stacd, void *exinf)
{
    int i;
    (void)stacd;
    (void)exinf;
    for (i = 0; i < 3; i++) {
        int c = rv_uart_getc_wait();
        if (i == 0) {
            g_t_first = rdtime_now();
        }
        g_got[i] = (char)c;
        g_n = i + 1;
    }
    tk_ext_tsk();
}

int m16_run_uart_irq_test(void)
{
    T_CTSK ctsk;
    ID id;
    unsigned long irq0, t_sent, lat_us;
    int pass = 1;
    int t;

    uart_puts("\r\n[M16] UART receive interrupt (PLIC) test, loopback\r\n");

    g_n = 0;
    memset(&ctsk, 0, sizeof(ctsk));
    ctsk.tskatr = TA_HLNG | TA_RNG0;
    ctsk.itskpri = 5;
    ctsk.stksz = 8192;
    ctsk.task = (FP)task_reader;
    id = tk_cre_tsk(&ctsk);
    if (id <= 0 || tk_sta_tsk(id, 0) != E_OK) {     /* R はすぐ眠る */
        uart_puts("[M16] task start failed\r\n[M16] FAIL\r\n");
        return 0;
    }

    irq0 = rv_uart_irq_count;
    rv_uart_loopback(1);                /* ここから画面には出ない */
    t_sent = rdtime_now();
    uart_putc('t');
    uart_putc('z');
    uart_putc('!');
    for (t = 0; t < 100 && g_n < 3; t++) {
        tk_dly_tsk(1);
    }
    rv_uart_loopback(0);
#if defined(__aarch64__)
    /* QEMU の PL011 が折り返し（CR の LBE）を持たないと（QEMU 9.1 より前）、送った文字は画面に出るだけで戻らない。
     * そのときは確かめられないので SKIP にする（受信割込みそのものは、キーボードからの入力で使える） */
    if (g_n == 0 && rv_uart_irq_count == irq0) {
        uart_puts("\r\n[M16] SKIP (this QEMU's PL011 has no loopback mode; QEMU 9.1 or later needed)\r\n");
        return 1;
    }
#endif

    pass &= check(g_n == 3 && g_got[0] == 't' && g_got[1] == 'z' && g_got[2] == '!',
                  "a waiting task received 't', 'z', '!' in order");
    pass &= check(rv_uart_irq_count > irq0, "characters arrived by UART receive interrupts");
    lat_us = (g_n >= 1) ? (g_t_first - t_sent) / TICKS_PER_US : 0;
    if (g_n >= 1) {
        uart_puts("[M16] first character delivered after ");
        put_num(lat_us);
        uart_puts(" us");
    } else {
        uart_puts("[M16] nothing delivered");
    }
    uart_puts(", interrupts: ");
    put_num(rv_uart_irq_count - irq0);
    uart_puts("\r\n");
    pass &= check(g_n >= 1 && lat_us < 5000, "delivered quickly (well under the old 10 ms polling)");

    uart_puts(pass ? "[M16] PASS\r\n" : "[M16] FAIL\r\n");
    return pass;
}
