/* kernel/tk/m6_tick_test.c — M6: システム時刻とタイマ待ちの確認
 *
 * tk_dly_tsk(500) で 500ms 待ち、その前後で
 *   (1) T-Kernel のシステム時刻（tk_get_otm）
 *   (2) ハードウェアの時刻カウンタ（rdtime、10MHz）
 * を読み、両方が約 500ms 進んでいれば合格。
 *
 * 合格の目印: [M6] PASS
 */

#include <tk/tkernel.h>

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern ER tk_get_otm(SYSTIM *pk_tim);

#define M6_DELAY_MS     500
#define M6_TOL_MS       30          /* 許容誤差（QEMU の揺らぎ分） */
#define M6_TIMEBASE_HZ  timebase_hz()

static void put_dec(unsigned long v)
{
    char buf[24];
    int n = 0;

    do {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0 && n < (int)sizeof(buf));
    while (n > 0) {
        uart_putc(buf[--n]);
    }
}

static unsigned long rdtime_now(void)
{
    unsigned long t;
#if defined(__aarch64__)
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t));   /* Arm の汎用タイマのカウンタ */
#else
    asm volatile("rdtime %0" : "=r"(t));
#endif
    return t;
}

/* カウンタの周波数（Hz） */
static unsigned long timebase_hz(void)
{
#if defined(__aarch64__)
    unsigned long f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
#else
    return 10000000UL;      /* QEMU virt の time CSR は 10MHz */
#endif
}

static long long systim_ms(const SYSTIM *t)
{
    return ((long long)t->hi << 32) | (unsigned long long)t->lo;
}

static int within(long v, long want, long tol)
{
    return (v >= want - tol) && (v <= want + tol);
}

int m6_run_tick_test(void)
{
    SYSTIM t0, t1;
    unsigned long h0, h1;
    long sys_ms, hw_ms;
    ER ercd;

    uart_puts("\r\n[M6] tick test: tk_dly_tsk(500)\r\n");

    tk_get_otm(&t0);
    h0 = rdtime_now();

    ercd = tk_dly_tsk(M6_DELAY_MS);

    tk_get_otm(&t1);
    h1 = rdtime_now();

    sys_ms = (long)(systim_ms(&t1) - systim_ms(&t0));
    hw_ms = (long)((h1 - h0) / (M6_TIMEBASE_HZ / 1000));

    uart_puts("[M6] tk_dly_tsk ercd=");
    if (ercd < 0) {
        uart_putc('-');
        put_dec((unsigned long)(-ercd));
    } else {
        put_dec((unsigned long)ercd);
    }
    uart_puts("\r\n[M6] system time  +");
    put_dec((unsigned long)(sys_ms < 0 ? 0 : sys_ms));
    uart_puts(" ms\r\n[M6] hardware time +");
    put_dec((unsigned long)hw_ms);
    uart_puts(" ms\r\n");

    if (ercd == E_OK
        && within(sys_ms, M6_DELAY_MS, M6_TOL_MS)
        && within(hw_ms, M6_DELAY_MS, M6_TOL_MS)) {
        uart_puts("[M6] PASS\r\n");
        return 1;
    }
    uart_puts("[M6] FAIL\r\n");
    return 0;
}
