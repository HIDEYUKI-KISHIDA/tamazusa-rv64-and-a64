/* kernel/tk/m10_multi_user_test.c — M10: タスクごとのカーネルスタック（SMODE=1 かつ MMU=1）
 *
 *   1. 同じ優先度のタスク A・B が、それぞれユーザープログラムを同時に持つ。
 *      各プログラムは 3 回「SYS_PUTS → SYS_SLEEP(30ms)」を繰り返して exit(id)。
 *      → システムコールの中で眠り（T-Kernel の待ち）、その間にもう一方が動く。
 *         SYS_PUTS の呼び出し順が A B A B A B のように交互になり、
 *         どちらも自分の id で終わる（s1 レジスタがシステムコール・切替をまたいで保たれる）
 *   2. 優先度の低いタスク L が U-mode で長い空ループを回す。
 *      高優先度タスク H は 50ms 眠ってから起きる。
 *      → 割込み出口で U-mode の L から H に切り替わる（L はまだ回っている）。
 *         L は後で再開され、最後まで走って exit(7)
 *
 * 合格の目印: [M10] PASS
 */
#include <stdint.h>
#include <tk/tkernel.h>
#include <libstr.h>

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern long rv_run_user(uintptr_t entry, uintptr_t user_sp, long arg);
extern uint8_t __user_start[];
extern void rv_user_prog_entry(void);
extern volatile int rv_user_puts_log[];
extern volatile int rv_user_puts_count;

#if defined(__aarch64__)
#include "rv_priv.h"                /* USER_VA_BASE（0x0040_0000） */
#else
#define USER_VA_BASE    0x40000000UL
#endif
#define USER_STACK(i)   (USER_VA_BASE + 0x3000UL + (uintptr_t)(i) * 0x1000UL)   /* 追加スタック i の上端 */
#if defined(__aarch64__)
static unsigned long ticks_per_ms(void)
{
    unsigned long f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f / 1000UL;
}
#define TICKS_PER_MS    ticks_per_ms()
#else
#define TICKS_PER_MS    10000UL     /* QEMU virt の time CSR は 10MHz */
#endif
#define H_DELAY_MS      50
#define H_TOL_MS        20

static volatile long g_exit[2];
static volatile int  g_done[2];
static volatile ID   g_tid[2];

static volatile int  g_h_woke;
static volatile int  g_l_done_at_h;
static volatile unsigned long g_h_time;
static volatile long g_l_exit;
static volatile int  g_l_done;

static unsigned long rdtime_now(void)
{
    unsigned long t;
#if defined(__aarch64__)
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
#else
    asm volatile("rdtime %0" : "=r"(t));
#endif
    return t;
}

static uintptr_t user_entry(void)
{
    return USER_VA_BASE + ((uintptr_t)rv_user_prog_entry - (uintptr_t)__user_start);
}

static void put_num(long v)
{
    char buf[24];
    int n = 0;
    unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;

    if (v < 0) {
        uart_putc('-');
    }
    do {
        buf[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u != 0);
    while (n > 0) {
        uart_putc(buf[--n]);
    }
}

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M10] OK   " : "[M10] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

/* ---- 1. 2 つのユーザープログラムを同時に ---- */

static void task_user(INT stacd, void *exinf)
{
    int i = stacd;
    (void)exinf;
    g_tid[i] = tk_get_tid();
    g_exit[i] = rv_run_user(user_entry(), USER_STACK(i), 4 | ((long)(10 + i) << 8));
    g_done[i] = 1;
    tk_ext_tsk();
}

/* ---- 2. U-mode の空回りを高優先度タスクが割り込む ---- */

static void task_h(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;
    tk_dly_tsk(H_DELAY_MS);
    g_h_time = rdtime_now();
    g_l_done_at_h = g_l_done;
    g_h_woke = 1;
    tk_ext_tsk();
}

static void task_l(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;
    g_l_exit = rv_run_user(user_entry(), USER_STACK(2), 3);
    g_l_done = 1;
    tk_ext_tsk();
}

static ID start_task(FP fn, PRI pri, INT stacd)
{
    T_CTSK ctsk;
    ID id;

    memset(&ctsk, 0, sizeof(ctsk));
    ctsk.tskatr = TA_HLNG | TA_RNG0;
    ctsk.itskpri = pri;
    ctsk.stksz = 8192;
    ctsk.task = fn;
    id = tk_cre_tsk(&ctsk);
    if (id <= 0) {
        return id;
    }
    if (tk_sta_tsk(id, stacd) != E_OK) {
        return -1;
    }
    return id;
}

static int wait_flags(volatile int *a, volatile int *b, int max_ms)
{
    int t;
    for (t = 0; t < max_ms; t += 10) {
        if (*a && (b == NULL || *b)) {
            return 1;
        }
        tk_dly_tsk(10);
    }
    return 0;
}

int m10_run_multi_user_test(void)
{
    int pass = 1;
    int n, k, alternating;
    unsigned long t0, lat_ms;

    uart_puts("\r\n[M10] per-task kernel stack test\r\n");

    /* 1 */
    g_done[0] = g_done[1] = 0;
    rv_user_puts_count = 0;
    if (start_task((FP)task_user, 10, 0) <= 0 || start_task((FP)task_user, 10, 1) <= 0) {
        uart_puts("[M10] task start failed\r\n[M10] FAIL\r\n");
        return 0;
    }
    if (!wait_flags(&g_done[0], &g_done[1], 2000)) {
        uart_puts("[M10] user programs did not finish\r\n[M10] FAIL\r\n");
        return 0;
    }
    n = rv_user_puts_count;
    uart_puts("[M10] SYS_PUTS order (task id):");
    for (k = 0; k < n; k++) {
        uart_putc(' ');
        put_num(rv_user_puts_log[k]);
    }
    uart_puts("\r\n");
    alternating = (n == 6);
    for (k = 0; alternating && k < n; k++) {
        if (rv_user_puts_log[k] != g_tid[k & 1]) {
            alternating = 0;
        }
    }
    pass &= check(alternating,
                  "two user programs ran together, sleeping in syscalls (A B A B A B)");
    pass &= check(g_exit[0] == 10 && g_exit[1] == 11,
                  "each program exited with its own id (registers kept across switches)");

    /* 2 */
    g_h_woke = 0;
    g_l_done = 0;
    t0 = rdtime_now();
    if (start_task((FP)task_h, 5, 0) <= 0) {    /* H はすぐ眠る */
        uart_puts("[M10] task start failed\r\n[M10] FAIL\r\n");
        return 0;
    }
    if (start_task((FP)task_l, 11, 0) <= 0) {   /* L は init より優先、U-mode で回る */
        uart_puts("[M10] task start failed\r\n[M10] FAIL\r\n");
        return 0;
    }
    if (!wait_flags(&g_l_done, &g_h_woke, 5000)) {
        uart_puts("[M10] spinner did not finish\r\n[M10] FAIL\r\n");
        return 0;
    }
    lat_ms = (g_h_time - t0) / TICKS_PER_MS;
    uart_puts("[M10] H ran at +");
    put_num((long)lat_ms);
    uart_puts(" ms, L exit=");
    put_num(g_l_exit);
    uart_puts("\r\n");
    pass &= check(!g_l_done_at_h && lat_ms + H_TOL_MS >= H_DELAY_MS
                  && lat_ms <= H_DELAY_MS + H_TOL_MS,
                  "high-priority task preempted the U-mode spinner on time");
    pass &= check(g_l_exit == 7, "the preempted U-mode program resumed and finished (exit 7)");

    uart_puts(pass ? "[M10] PASS\r\n" : "[M10] FAIL\r\n");
    return pass;
}
