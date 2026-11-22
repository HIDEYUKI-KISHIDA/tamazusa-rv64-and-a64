/* kernel/tk/m19_fpvec_test.c — M19: 浮動小数点・ベクトルのレジスタがタスクの切替で保たれる
 *                               （SMODE=1 かつ MMU=1）
 *
 *   ディスクの fpvec_test.elf（user/linux/fpvec_test.c）を 2 つ同時に、別の種（11 と 22）で動かす。
 *   それぞれ 20 回、レジスタに自分の値を入れて眠り、その間にもう一方が同じレジスタを使う。
 *   起きたとき自分の値が残っていれば exit(0)。
 *   1. 浮動小数点のレジスタ 32 本が保たれる
 *   2. ベクトルのレジスタ 32 本が保たれる（V 拡張がある CPU のみ）
 *   3. 退避・復元が実際に行われた（回数が増えている）
 *   musl が用意されていないとき（ディスクに fpvec_test.elf がない）は SKIP。
 *
 * 合格の目印: [M19] PASS
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>
#include <libstr.h>
#include "../fs/tarfs.h"

extern void uart_puts(const char *s);
struct rv_uproc;
extern struct rv_uproc *rv_uproc_load_linux(const void *image, size_t len,
                                            const char *const argv[], const char *const envp[]);
extern void rv_uproc_free(struct rv_uproc *p);
extern uint64_t  rv_uproc_satp(const struct rv_uproc *p);
extern uintptr_t rv_uproc_entry(const struct rv_uproc *p);
extern uintptr_t rv_uproc_sp(const struct rv_uproc *p);
extern long rv_run_user_in_space(uint64_t satp, uintptr_t entry, uintptr_t user_sp, long arg);
extern int rv_fpu_vlenb(void);
extern volatile unsigned long rv_fpu_saves, rv_fpu_restores, rv_vec_saves, rv_vec_restores;

#define ELF_MAX (512 * 1024)
static uint64_t elf_buf[ELF_MAX / 8];
static size_t elf_len;
static volatile long g_exit[2];
static volatile int  g_done[2];
static int g_vec;

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M19] OK   " : "[M19] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

static void task_run(INT stacd, void *exinf)
{
    static const char *const argv_a[] = { "fpvec_test", "11", "vec", 0 };
    static const char *const argv_b[] = { "fpvec_test", "22", "vec", 0 };
    static const char *const argv_a_nv[] = { "fpvec_test", "11", 0 };
    static const char *const argv_b_nv[] = { "fpvec_test", "22", 0 };
    static const char *const envp[] = { "HOME=/root", 0 };
    const char *const *argv;
    struct rv_uproc *p;
    int i = stacd;
    (void)exinf;

    argv = g_vec ? (i == 0 ? argv_a : argv_b) : (i == 0 ? argv_a_nv : argv_b_nv);
    p = rv_uproc_load_linux(elf_buf, elf_len, argv, envp);
    if (p == NULL) {
        g_exit[i] = -100;
    } else {
        g_exit[i] = rv_run_user_in_space(rv_uproc_satp(p), rv_uproc_entry(p), rv_uproc_sp(p), 0);
        rv_uproc_free(p);
    }
    g_done[i] = 1;
    tk_ext_tsk();
}

int m19_run_fpvec_test(void)
{
    struct tarfs_file f;
    unsigned long fs0 = rv_fpu_saves, fr0 = rv_fpu_restores, vs0 = rv_vec_saves, vr0 = rv_vec_restores;
    int pass = 1;
    int i, t;

    uart_puts("\r\n[M19] FP / vector registers across task switches\r\n");
    if (tarfs_find("fpvec_test.elf", &f) != 1) {
        uart_puts("[M19] SKIP (fpvec_test.elf is not on the disk: run tools/build_musl_rv64.sh)\r\n");
        return 1;
    }
    if (f.size > ELF_MAX || tarfs_read_all(&f, elf_buf, ELF_MAX) != (int64_t)f.size) {
        uart_puts("[M19] could not read fpvec_test.elf\r\n[M19] FAIL\r\n");
        return 0;
    }
    elf_len = (size_t)f.size;
    g_vec = rv_fpu_vlenb() == 16;       /* テストは vlenb = 16（QEMU の既定）を前提にしている */

    for (i = 0; i < 2; i++) {
        T_CTSK ctsk;
        ID id;
        g_done[i] = 0;
        memset(&ctsk, 0, sizeof(ctsk));
        ctsk.tskatr = TA_HLNG | TA_RNG0;
        ctsk.itskpri = 10;
        ctsk.stksz = 8192;
        ctsk.task = (FP)task_run;
        id = tk_cre_tsk(&ctsk);
        if (id <= 0 || tk_sta_tsk(id, i) != E_OK) {
            uart_puts("[M19] task start failed\r\n[M19] FAIL\r\n");
            return 0;
        }
    }
    for (t = 0; t < 5000 && !(g_done[0] && g_done[1]); t += 10) {
        tk_dly_tsk(10);
    }

    pass &= check(g_done[0] && g_done[1] && g_exit[0] == 0 && g_exit[1] == 0,
                  g_vec ? "two programs kept their FP and vector registers through 20 switches each"
                        : "two programs kept their FP registers through 20 switches each");
    pass &= check(rv_fpu_saves > fs0 && rv_fpu_restores > fr0, "FP registers were saved and restored");
    if (g_vec) {
        pass &= check(rv_vec_saves > vs0 && rv_vec_restores > vr0, "vector registers were saved and restored");
    } else {
        uart_puts("[M19] (vector part skipped: no V extension or vlenb != 16)\r\n");
    }

    uart_puts(pass ? "[M19] PASS\r\n" : "[M19] FAIL\r\n");
    return pass;
}
