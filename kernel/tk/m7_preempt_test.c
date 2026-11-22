/* kernel/tk/m7_preempt_test.c — M7: 割込みによるタスク切替（プリエンプション）の確認
 *
 *   1. 高優先度タスク H（優先度 5）を起動。H は tk_dly_tsk(100) で眠る。
 *   2. init タスク（優先度 12）はシステムコールを一切呼ばずに最大 1 秒空回りする。
 *   3. 100ms 後のタイマ割込みで H が起き、割込み出口で H に切り替われば合格。
 *
 * 割込み出口で切り替えない実装では、H は init が空回りを終える 1 秒後まで
 * 動けないので不合格になる。
 *
 * 合格の目印: [M7] PASS
 */

#include <tk/tkernel.h>
#include <libstr.h>

extern void uart_puts(const char *s);
extern void uart_putc(char c);

#define M7_DELAY_MS     100
#define M7_TOL_MS       20
#define M7_SPIN_MS      1000
#define M7_TICKS_PER_MS (timebase_hz() / 1000UL)

static volatile int g_h_woke;
static volatile unsigned long g_h_time;

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

static void task_h(void)
{
    tk_dly_tsk(M7_DELAY_MS);
    g_h_time = rdtime_now();
    g_h_woke = 1;
    tk_ext_tsk();
}

int m7_run_preempt_test(void)
{
    T_CTSK ctsk;
    ID id_h;
    unsigned long t0, spin_end, lat_ms;
    unsigned long loops = 0;

    uart_puts("\r\n[M7] preempt test: H(pri 5) wakes after 100ms while init spins\r\n");

    g_h_woke = 0;
    memset(&ctsk, 0, sizeof(ctsk));
    ctsk.tskatr = TA_HLNG | TA_RNG0;
    ctsk.itskpri = 5;
    ctsk.stksz = 8192;
    ctsk.task = (FP)task_h;
    id_h = tk_cre_tsk(&ctsk);
    if (id_h <= 0) {
        uart_puts("[M7] tk_cre_tsk(H) failed\r\n[M7] FAIL\r\n");
        return 0;
    }

    t0 = rdtime_now();
    if (tk_sta_tsk(id_h, 0) != E_OK) {     /* H がすぐ走り、tk_dly_tsk で眠る */
        uart_puts("[M7] tk_sta_tsk(H) failed\r\n[M7] FAIL\r\n");
        return 0;
    }

    /* システムコールなしで空回り（切り替えは割込み出口でしか起きない） */
    spin_end = t0 + M7_SPIN_MS * M7_TICKS_PER_MS;
    while (!g_h_woke && (long)(rdtime_now() - spin_end) < 0) {
        loops++;
    }

    if (!g_h_woke) {
        uart_puts("[M7] H did not run during the spin (no preemption)\r\n[M7] FAIL\r\n");
        return 0;
    }

    lat_ms = (g_h_time - t0) / M7_TICKS_PER_MS;
    uart_puts("[M7] H ran at +");
    put_dec(lat_ms);
    uart_puts(" ms (init spun ");
    put_dec(loops);
    uart_puts(" loops before)\r\n");

    if (lat_ms + M7_TOL_MS < M7_DELAY_MS || lat_ms > M7_DELAY_MS + M7_TOL_MS) {
        uart_puts("[M7] FAIL (timing)\r\n");
        return 0;
    }
    if (loops == 0) {
        uart_puts("[M7] FAIL (init never spun)\r\n");
        return 0;
    }
    uart_puts("[M7] PASS\r\n");
    return 1;
}
