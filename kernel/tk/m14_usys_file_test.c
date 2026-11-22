/* kernel/tk/m14_usys_file_test.c — M14: ファイルとプログラム起動のシステムコール（SMODE=1 かつ MMU=1）
 *
 *   ディスクの fsdemo.elf（user/fsdemo）を読み込んで動かす。
 *   1. fsdemo が open / read / close / spawn を一通り確かめて exit(77)
 *      （読み込み・オフセット・終わり・エラー・不正なアドレス・fd の上限・
 *        hello.elf の起動と終了コード・ないプログラム・ELF でないファイル）
 *   2. 開いたまま終わったファイルは、プログラムの後始末で閉じられる
 *   3. fsdemo 2 つが同時に hello.elf をそれぞれ起動 → 各自の id が返る
 *   4. すべて終わったあと、ページがすべて返っている
 *
 * 合格の目印: [M14] PASS
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>
#include <libstr.h>

extern void uart_puts(const char *s);
extern void uart_putc(char c);

struct rv_uproc;
extern struct rv_uproc *rv_load_program(const char *path, long *err);
extern void rv_uproc_free(struct rv_uproc *p);
extern uint64_t  rv_uproc_satp(const struct rv_uproc *p);
extern uintptr_t rv_uproc_entry(const struct rv_uproc *p);
extern uintptr_t rv_uproc_sp(const struct rv_uproc *p);
extern int rv_uproc_slot(const struct rv_uproc *p);
extern int rv_ufile_open_count(int slot);
extern int rv_page_free_count(void);
extern long rv_run_user_in_space(uint64_t satp, uintptr_t entry, uintptr_t user_sp, long arg);

static volatile long g_exit[2];
static volatile int  g_done[2];

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
    uart_puts(ok ? "[M14] OK   " : "[M14] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

/* ディスクから読み込んで実行。open_left に、終了直前（解放前）に開いていたファイルの数 */
static long run_program(const char *path, long arg, int *open_left)
{
    long err = 0;
    struct rv_uproc *p = rv_load_program(path, &err);
    long r;

    if (p == NULL) {
        return -1000 + err;
    }
    r = rv_run_user_in_space(rv_uproc_satp(p), rv_uproc_entry(p), rv_uproc_sp(p), arg);
    if (open_left != NULL) {
        *open_left = rv_ufile_open_count(rv_uproc_slot(p));
    }
    rv_uproc_free(p);
    return r;
}

static void task_fsdemo(INT stacd, void *exinf)
{
    int i = stacd;
    (void)exinf;
    g_exit[i] = run_program("fsdemo.elf", 1 | ((long)(0x51 + i) << 8), NULL);
    g_done[i] = 1;
    tk_ext_tsk();
}

int m14_run_usys_file_test(void)
{
    int pass = 1;
    int free0 = rv_page_free_count();
    int left = -1;
    long r;
    int i, t;

    uart_puts("\r\n[M14] file / spawn system call test\r\n");

    /* 1 */
    r = run_program("fsdemo.elf", 0, NULL);
    uart_puts("[M14] fsdemo exit=");
    put_num(r);
    uart_puts("\r\n");
    pass &= check(r == 77, "fsdemo: open/read/close/spawn checks all passed (exit 77)");

    /* 2 */
    r = run_program("fsdemo.elf", 2, &left);
    {
        /* 同じスロットが空いて再利用されるので、解放後は 0 になっているはず */
        long err = 0;
        struct rv_uproc *p = rv_load_program("hello.elf", &err);
        int after = (p != NULL) ? rv_ufile_open_count(rv_uproc_slot(p)) : -1;
        if (p != NULL) {
            rv_uproc_free(p);
        }
        pass &= check(r == 3 && left == 1 && after == 0,
                      "file left open at exit is closed by the cleanup");
    }

    /* 3 */
    for (i = 0; i < 2; i++) {
        T_CTSK ctsk;
        ID id;

        g_done[i] = 0;
        memset(&ctsk, 0, sizeof(ctsk));
        ctsk.tskatr = TA_HLNG | TA_RNG0;
        ctsk.itskpri = 10;
        ctsk.stksz = 8192;
        ctsk.task = (FP)task_fsdemo;
        id = tk_cre_tsk(&ctsk);
        if (id <= 0 || tk_sta_tsk(id, i) != E_OK) {
            uart_puts("[M14] task start failed\r\n[M14] FAIL\r\n");
            return 0;
        }
    }
    for (t = 0; t < 3000 && !(g_done[0] && g_done[1]); t += 10) {
        tk_dly_tsk(10);
    }
    pass &= check(g_done[0] && g_done[1] && g_exit[0] == 0x51 && g_exit[1] == 0x52,
                  "two programs spawned hello.elf at the same time, each got its own result");

    /* 4 */
    uart_puts("[M14] free pages: before ");
    put_num(free0);
    uart_puts(" after ");
    put_num(rv_page_free_count());
    uart_puts("\r\n");
    pass &= check(rv_page_free_count() == free0, "all pages returned (parents and children)");

    uart_puts(pass ? "[M14] PASS\r\n" : "[M14] FAIL\r\n");
    return pass;
}
