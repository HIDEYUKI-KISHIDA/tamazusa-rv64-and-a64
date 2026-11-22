/* kernel/tk/m27_rt_test.c — M27: リアルタイム性の測定（SMODE=1 かつ MMU=1）
 *
 *   T-Kernel の上で、Linux のプログラム（bash・busybox）と同時に、リアルタイムの仕事が
 *   決まった時間で動けるかを測る。
 *   優先度 5 のタスク（Linux のプロセスは 10）が tk_dly_tsk(10) で眠っては起きる。起きるたびに
 *   「タイマの割込みが本来鳴る時刻」から「タスクが動き出した時刻」までの遅れを、
 *   time CSR（10MHz = 0.1 マイクロ秒）で測る。
 *     1. 何もしていないとき（1 秒）
 *     2. bash が重い仕事をしているとき: 計算し続ける yes を 2 つ、fork・exec の繰り返し、
 *        パイプ（seq | sort | md5sum）、ディスクの大きな読み込み、ループバックの ping
 *   合格: どちらも 100 回以上測れて、負荷のある間の最大の遅れが 1 ミリ秒未満
 *   （QEMU の上の値。実機では別に測る）
 *   bash がないときは、負荷の部分を飛ばす。
 *
 * 合格の目印: [M27] PASS
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>
#include <libstr.h>
#include <rv_priv.h>
#include "../fs/tarfs.h"

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern volatile uint64_t rv_tick_deadline, rv_tick_entered;
struct rv_uproc;
extern struct rv_uproc *rv_linux_load_path(const char *path, const char *const argv[],
                                           const char *const envp[], long *err);
extern long rv_linux_run_process(struct rv_uproc *p);
extern int rv_linux_proc_count(void);
extern void rv_console_set_test_input(const char *s);
extern void rv_console_capture(int on);
extern ER tk_del_tsk(ID tskid);

static const char load_script[] =
    "yes > /dev/null & yes > /dev/null &\n"
    "for i in $(seq 1 20); do /bin/true; done\n"
    "seq 1 3000 | sort -r | md5sum\n"
    "cat /bash.elf /busybox.elf > /dev/null\n"
    "ping -c 3 -i 0.2 127.0.0.1 > /dev/null\n"
    "kill %1 %2; wait\n"
    "exit 0\n";

struct stat_rt {
    unsigned long n;
    uint64_t sum, max;                  /* タイマの割込みの処理に入ってから、タスクが動き出すまで */
    unsigned long over100us, over500us;
    uint64_t irq_sum, irq_max;          /* 本来の時刻から、割込みの処理に入るまで（QEMU の届け方の分） */
};
static struct stat_rt st_idle, st_load;
static struct stat_rt *volatile cur;
static volatile int rt_stop, rt_done;

static inline uint64_t now_ticks(void)
{
    uint64_t t;
    t = RV_RDTIME();
    return t;
}

static void rt_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;
    while (!rt_stop) {
        uint64_t t, d;
        tk_dly_tsk(10);
        t = now_ticks();
        d = t - rv_tick_entered;        /* 0.1 マイクロ秒単位 */
        if (cur != NULL && d < 10000000ULL) {
            uint64_t q = rv_tick_entered - rv_tick_deadline;
            cur->irq_sum += q;
            if (q > cur->irq_max) cur->irq_max = q;
            cur->n++;
            cur->sum += d;
            if (d > cur->max) cur->max = d;
            if (d > 1000) cur->over100us++;
            if (d > 5000) cur->over500us++;
        }
    }
    rt_done = 1;
    tk_ext_tsk();
}

static void put_num(unsigned long v)
{
    char b[24];
    int n = 0;
    do {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        uart_putc(b[--n]);
    }
}

/* 0.1 マイクロ秒単位の値を「12.3us」の形で */
static void put_us(uint64_t t)
{
    put_num((unsigned long)(t / 10));
    uart_putc('.');
    uart_putc((char)('0' + t % 10));
    uart_puts("us");
}

static void report(const char *name, const struct stat_rt *s)
{
    uart_puts("[M27] ");
    uart_puts(name);
    uart_puts(": samples ");
    put_num(s->n);
    uart_puts(", average ");
    put_us(s->n ? s->sum / s->n : 0);
    uart_puts(", max ");
    put_us(s->max);
    uart_puts(", over 100us ");
    put_num(s->over100us);
    uart_puts(", over 500us ");
    put_num(s->over500us);
    uart_puts("\r\n[M27]   (timer interrupt delivered by QEMU: average ");
    put_us(s->n ? s->irq_sum / s->n : 0);
    uart_puts(", max ");
    put_us(s->irq_max);
    uart_puts(")\r\n");
}

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M27] OK   " : "[M27] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

int m27_run_rt_test(void)
{
    static const char *const bash_argv[] = { "bash", "--norc", "--noediting", 0 };
    static const char *const envp[] = { "HOME=/", "PATH=/bin:/", "TERM=vt100", 0 };
    struct tarfs_file f;
    T_CTSK ct;
    ID tid;
    long r;
    int pass = 1, have_load = 0;

    uart_puts("\r\n[M27] real-time latency test (priority 5 task vs Linux processes)\r\n");
    memset(&st_idle, 0, sizeof(st_idle));
    memset(&st_load, 0, sizeof(st_load));
    rt_stop = 0;
    rt_done = 0;
    cur = NULL;

    memset(&ct, 0, sizeof(ct));
    ct.tskatr = TA_HLNG | TA_RNG0;
    ct.itskpri = 5;
    ct.stksz = 4096;
    ct.task = (FP)rt_task;
    tid = tk_cre_tsk(&ct);
    if (tid <= 0 || tk_sta_tsk(tid, 0) != E_OK) {
        uart_puts("[M27] could not start the task\r\n[M27] FAIL\r\n");
        return 0;
    }

    /* 1. 何もしていないとき（その前に、画面に出す文字がたまっていれば出し終える。
     *    QEMU は画面の写し（virtio-gpu）を処理するあいだ、タイマの割込みを最大 2 ミリ秒ほど遅らせることがある） */
    {
        extern void desktop_wait_idle(void);
        desktop_wait_idle();
    }
    cur = &st_idle;
    tk_dly_tsk(1500);

    /* 2. Linux のプログラムが重い仕事をしているとき */
    if (tarfs_find("bash.elf", &f) == 1) {
        struct rv_uproc *p = rv_linux_load_path("bash.elf", bash_argv, envp, &r);
        if (p != NULL) {
            have_load = 1;
            cur = &st_load;
            rv_console_set_test_input(load_script);
            rv_console_capture(1);
            r = rv_linux_run_process(p);
            rv_console_capture(0);
            rv_console_set_test_input(0);
        }
    }
    cur = NULL;
    rt_stop = 1;
    while (!rt_done) {
        tk_dly_tsk(10);
    }
    while (tk_del_tsk(tid) == E_OBJ) {
        tk_dly_tsk(1);
    }

    report("idle            ", &st_idle);
    pass &= check(st_idle.n >= 100 && st_idle.max < 10000, "idle: 100+ samples, max latency under 1 ms");
    if (have_load) {
        report("under Linux load", &st_load);
        pass &= check(st_load.n >= 100 && st_load.max < 10000,
                      "under load (yes x2, fork/exec, pipes, disk, ping): 100+ samples, max latency under 1 ms");
        pass &= check(r == 0 && rv_linux_proc_count() == 0, "load script finished and all processes ended");
    } else {
        uart_puts("[M27] (bash is not on the disk: load part skipped)\r\n");
    }
    uart_puts(pass ? "[M27] PASS\r\n" : "[M27] FAIL\r\n");
    return pass;
}
