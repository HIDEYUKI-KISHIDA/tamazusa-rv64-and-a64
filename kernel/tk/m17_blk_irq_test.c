/* kernel/tk/m17_blk_irq_test.c — M17: ディスクの完了割込み（SMODE=1 かつ MMU=1）
 *
 *   1. 優先度の高いタスク R（優先度 5）がディスクを 200 回読む（毎回 8 セクタ）。
 *      優先度の低いタスク S（優先度 20）は、その間ずっと空回りして回数を数える。
 *      → 完了を見て待つ方式だと R が CPU を手放さず S は動けない。
 *         完了の割込みを待って眠る方式なら、読み込みの合間に S が動ける
 *   2. 完了の割込みの回数が増えている
 *   3. 読んだ中身が毎回同じ（先頭の tar の見出し "ustar" と、1 回目と同じデータ）
 *
 * 合格の目印: [M17] PASS
 */
#include <stdint.h>
#include <tk/tkernel.h>
#include <libstr.h>
#include "../fs/tarfs.h"

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern volatile unsigned long rv_blk_irq_count;

#define READS       200
#define SECTORS     8

static volatile int g_r_done;
static volatile int g_r_ok;
static volatile int g_s_stop;
static volatile unsigned long g_s_loops;
static volatile unsigned long g_s_loops_during;
static uint8_t g_first[SECTORS * 512] __attribute__((aligned(8)));
static uint8_t g_buf[SECTORS * 512] __attribute__((aligned(8)));

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
    uart_puts(ok ? "[M17] OK   " : "[M17] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

static void task_spinner(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;
    while (!g_s_stop) {
        g_s_loops++;
    }
    tk_ext_tsk();
}

static void task_reader(INT stacd, void *exinf)
{
    unsigned long s0;
    int i, ok = 1;
    (void)stacd;
    (void)exinf;

    s0 = g_s_loops;
    for (i = 0; i < READS && ok; i++) {
        if (virtio_blk_read_nocache(0, g_buf, SECTORS) != 0 || memcmp(g_buf, g_first, sizeof(g_buf)) != 0) {
            ok = 0;
        }
    }
    g_s_loops_during = g_s_loops - s0;
    g_r_ok = ok;
    g_r_done = 1;
    tk_ext_tsk();
}

static ID start(FP fn, PRI pri)
{
    T_CTSK ctsk;
    ID id;

    memset(&ctsk, 0, sizeof(ctsk));
    ctsk.tskatr = TA_HLNG | TA_RNG0;
    ctsk.itskpri = pri;
    ctsk.stksz = 8192;
    ctsk.task = fn;
    id = tk_cre_tsk(&ctsk);
    if (id > 0 && tk_sta_tsk(id, 0) != E_OK) {
        return -1;
    }
    return id;
}

int m17_run_blk_irq_test(void)
{
    unsigned long irq0;
    int pass = 1;
    int t;

    uart_puts("\r\n[M17] disk completion interrupt test\r\n");

    if (virtio_blk_read_nocache(0, g_first, SECTORS) != 0
        || g_first[257] != 'u' || g_first[258] != 's' || g_first[259] != 't') {
        uart_puts("[M17] first read failed\r\n[M17] FAIL\r\n");
        return 0;
    }

    g_r_done = 0;
    g_s_stop = 0;
    g_s_loops = 0;
    irq0 = rv_blk_irq_count;
    if (start((FP)task_spinner, 20) <= 0 || start((FP)task_reader, 5) <= 0) {
        uart_puts("[M17] task start failed\r\n[M17] FAIL\r\n");
        return 0;
    }
    /* init（優先度 12）は眠って待つ。S（20）は init より低いので、init が眠っている間だけ動く */
    for (t = 0; t < 5000 && !g_r_done; t += 10) {
        tk_dly_tsk(10);
    }
    g_s_stop = 1;
    tk_dly_tsk(10);

    uart_puts("[M17] ");
    put_num(READS);
    uart_puts(" reads: completion interrupts ");
    put_num(rv_blk_irq_count - irq0);
    uart_puts(", low-priority task loops during the reads ");
    put_num(g_s_loops_during);
    uart_puts("\r\n");

    pass &= check(g_r_done && g_r_ok, "200 reads finished, data the same every time");
    pass &= check(rv_blk_irq_count - irq0 >= READS, "each read completed by an interrupt");
    pass &= check(g_s_loops_during > 0,
                  "while the reader waited for the disk, a lower-priority task could run");

    uart_puts(pass ? "[M17] PASS\r\n" : "[M17] FAIL\r\n");
    return pass;
}
