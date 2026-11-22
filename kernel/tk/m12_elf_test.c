/* kernel/tk/m12_elf_test.c — M12: ユーザープログラム（ELF）の読み込み（SMODE=1 かつ MMU=1）
 *
 *   別にビルドした C のユーザープログラム user/hello（ELF）を読み込んで動かす。
 *   1. 読み込んで実行: .data の初期値・.bss の 0 を確かめて挨拶し、exit(90)
 *   2. 自分のコードに書く → ストアページフォールトで強制終了（コードは読み取り専用で写る）
 *   3. スタックの下に書く → ストアページフォールトで強制終了（スタックの下は未割り当て）
 *   4. 同じ ELF を 2 つ同時に読み込み、タスク 2 つで動かす → それぞれ自分の id で終わる
 *   5. 壊れた ELF（マジック不正・途中で切れている・カーネル領域に置こうとする）は読み込まない
 *   6. すべて終わったあと、割り当てたページがすべて返っている（漏れなし）
 *
 * 合格の目印: [M12] PASS
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>
#include <libstr.h>

extern void uart_puts(const char *s);
extern void uart_putc(char c);

struct rv_uproc;
extern struct rv_uproc *rv_uproc_load(const void *image, size_t len);
extern void rv_uproc_free(struct rv_uproc *p);
extern uint64_t  rv_uproc_satp(const struct rv_uproc *p);
extern uintptr_t rv_uproc_entry(const struct rv_uproc *p);
extern uintptr_t rv_uproc_sp(const struct rv_uproc *p);
extern int rv_uproc_last_error;
extern int rv_page_free_count(void);
extern long rv_run_user_in_space(uint64_t satp, uintptr_t entry, uintptr_t user_sp, long arg);
extern volatile uint64_t rv_user_fault_cause;

extern const uint8_t rv_blob_hello_elf[];
extern const uint8_t rv_blob_hello_elf_end[];

#if defined(__aarch64__)
#define CAUSE_STORE_PF  0x24UL      /* EC: 下の EL のデータアボート */
#define KERNEL_VA       0x40200000UL
#else
#define CAUSE_STORE_PF  15UL
#define KERNEL_VA       0x80200000UL
#endif
#define UPE_HEADER      1
#define UPE_RANGE       3

static volatile long g_exit[2];
static volatile int  g_done[2];
static uint64_t g_bad_elf[16384 / 8];   /* 壊した ELF を作る作業領域（8 バイト境界） */

static size_t blob_len(void)
{
    return (size_t)(rv_blob_hello_elf_end - rv_blob_hello_elf);
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
    uart_puts(ok ? "[M12] OK   " : "[M12] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

/* 読み込んで実行し、終わったら解放する。読み込めなければ -1000 - 理由 */
static long run_elf(const void *img, size_t len, long arg)
{
    struct rv_uproc *p = rv_uproc_load(img, len);
    long r;

    if (p == NULL) {
        return -1000 - rv_uproc_last_error;
    }
    r = rv_run_user_in_space(rv_uproc_satp(p), rv_uproc_entry(p), rv_uproc_sp(p), arg);
    rv_uproc_free(p);
    return r;
}

static void task_elf(INT stacd, void *exinf)
{
    int i = stacd;
    (void)exinf;
    g_exit[i] = run_elf(rv_blob_hello_elf, blob_len(), 2 | ((long)(0x31 + i) << 8));
    g_done[i] = 1;
    tk_ext_tsk();
}

int m12_run_elf_test(void)
{
    int pass = 1;
    int free0 = rv_page_free_count();
    size_t len = blob_len();
    long r;
    int i, t;

    uart_puts("\r\n[M12] ELF user program test (");
    put_num((long)len);
    uart_puts(" bytes)\r\n");

    /* 1 */
    r = run_elf(rv_blob_hello_elf, len, 0);
    pass &= check(r == 90, "loaded ELF, .data/.bss set up, printed, exit(90)");

    /* 2 */
    r = run_elf(rv_blob_hello_elf, len, 1);
    pass &= check(r == -1 && rv_user_fault_cause == CAUSE_STORE_PF,
                  "write to own code -> store page fault, killed (text is read-only)");

    /* 3 */
    r = run_elf(rv_blob_hello_elf, len, 3);
    pass &= check(r == -1 && rv_user_fault_cause == CAUSE_STORE_PF,
                  "write below the stack -> store page fault, killed (guard)");

    /* 4 */
    for (i = 0; i < 2; i++) {
        T_CTSK ctsk;
        ID id;

        g_done[i] = 0;
        memset(&ctsk, 0, sizeof(ctsk));
        ctsk.tskatr = TA_HLNG | TA_RNG0;
        ctsk.itskpri = 10;
        ctsk.stksz = 8192;
        ctsk.task = (FP)task_elf;
        id = tk_cre_tsk(&ctsk);
        if (id <= 0 || tk_sta_tsk(id, i) != E_OK) {
            uart_puts("[M12] task start failed\r\n[M12] FAIL\r\n");
            return 0;
        }
    }
    for (t = 0; t < 2000 && !(g_done[0] && g_done[1]); t += 10) {
        tk_dly_tsk(10);
    }
    pass &= check(g_done[0] && g_done[1] && g_exit[0] == 0x31 && g_exit[1] == 0x32,
                  "two instances of the same ELF ran together, each kept its own data");

    /* 5 */
    if (len <= sizeof(g_bad_elf)) {
        uint8_t *b = (uint8_t *)g_bad_elf;
        uint64_t phoff;
        uint64_t *vaddr;
        int k;

        memcpy(b, rv_blob_hello_elf, len);
        b[1] = 'X';
        r = run_elf(b, len, 0);
        pass &= check(r == -1000 - UPE_HEADER, "bad magic -> rejected");

        memcpy(b, rv_blob_hello_elf, len);
        r = run_elf(b, 40, 0);
        pass &= check(r == -1000 - UPE_HEADER, "truncated file -> rejected");

        memcpy(b, rv_blob_hello_elf, len);
        memcpy(&phoff, b + 32, 8);                  /* e_phoff */
        /* 最初の PT_LOAD（先頭は RISCV_ATTRIBUTES のことがある）の p_vaddr を書き換える */
        for (k = 0; k < 16; k++) {
            uint32_t type;
            memcpy(&type, b + phoff + k * 56, 4);
            if (type == 1) {
                break;
            }
        }
        vaddr = (uint64_t *)(void *)(b + phoff + k * 56 + 16);
        *vaddr = KERNEL_VA;                         /* カーネルの場所 */
        r = run_elf(b, len, 0);
        pass &= check(r == -1000 - UPE_RANGE, "segment in kernel area -> rejected");
    } else {
        pass &= check(0, "ELF too large for the corruption test buffer");
    }

    /* 6 */
    uart_puts("[M12] free pages: before ");
    put_num(free0);
    uart_puts(" after ");
    put_num(rv_page_free_count());
    uart_puts("\r\n");
    pass &= check(rv_page_free_count() == free0, "all pages returned (no leak)");

    uart_puts(pass ? "[M12] PASS\r\n" : "[M12] FAIL\r\n");
    return pass;
}
