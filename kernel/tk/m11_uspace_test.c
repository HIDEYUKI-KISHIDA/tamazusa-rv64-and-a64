/* kernel/tk/m11_uspace_test.c — M11: タスクごとのユーザー空間（SMODE=1 かつ MMU=1）
 *
 *   タスク A・B が同じユーザープログラムを、それぞれ専用のページ表で動かす。
 *   どちらも同じ仮想アドレス 0x4000_1000（user_var）に自分の値を書き、
 *   眠っている間にもう一方が同じアドレスへ書く。起きて読み戻したとき、
 *   自分の値のままなら合格（空間が共有だと相手の値に変わる）。
 *
 *   1. A は exit(0x11)、B は exit(0x22)（読み戻しが自分の値だった）
 *   2. A と B の satp が違い、カーネルのページ表とも違う
 *   3. 各スロットのデータページ（物理）にそれぞれの値が入っている
 *   4. 元の .user のデータページ（M9/M10 が使う共通の空間）は書き換えられていない
 *   5. 終わったあと、init タスクはカーネルのページ表に戻っている
 *
 * 合格の目印: [M11] PASS
 */
#include <stdint.h>
#include <tk/tkernel.h>
#include <libstr.h>

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern long rv_run_user_in_space(uint64_t satp, uintptr_t entry, uintptr_t user_sp, long arg);
extern uint64_t rv_uspace_create(int slot);
extern uint8_t *rv_uspace_data_page(int slot);
extern uint64_t rv_mmu_read_satp(void);
extern uint64_t rv_kernel_satp;
extern uint8_t __user_start[];
extern void rv_user_prog_entry(void);
extern uint64_t user_var;

#if defined(__aarch64__)
#include "rv_priv.h"                /* USER_VA_BASE（0x0040_0000） */
#define USER_DATA_VA    "0x00401000"
#else
#define USER_VA_BASE    0x40000000UL
#define USER_DATA_VA    "0x40001000"
#endif
#define USER_SPACE_SP   (USER_VA_BASE + 0x3000UL)    /* 各空間のスタックページの上端 */

static const long g_val[2] = { 0x11, 0x22 };
static volatile long g_exit[2];
static volatile int  g_done[2];
static volatile uint64_t g_satp[2];

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M11] OK   " : "[M11] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

static void put_hex8(long v)
{
    static const char hex[] = "0123456789abcdef";
    uart_puts("0x");
    uart_putc(hex[(v >> 4) & 0xf]);
    uart_putc(hex[v & 0xf]);
}

static void task_space(INT stacd, void *exinf)
{
    int i = stacd;
    uintptr_t entry = USER_VA_BASE +
        ((uintptr_t)rv_user_prog_entry - (uintptr_t)__user_start);
    (void)exinf;

    g_satp[i] = rv_uspace_create(i);
    if (g_satp[i] != 0) {
        g_exit[i] = rv_run_user_in_space(g_satp[i], entry, USER_SPACE_SP, 5 | (g_val[i] << 8));
    } else {
        g_exit[i] = -2;
    }
    g_done[i] = 1;
    tk_ext_tsk();
}

int m11_run_uspace_test(void)
{
    uintptr_t var_off = (uintptr_t)&user_var - ((uintptr_t)__user_start + 0x1000);
    int pass = 1;
    int i, t;

    uart_puts("\r\n[M11] per-task user address space test\r\n");

    for (i = 0; i < 2; i++) {
        T_CTSK ctsk;
        ID id;

        g_done[i] = 0;
        memset(&ctsk, 0, sizeof(ctsk));
        ctsk.tskatr = TA_HLNG | TA_RNG0;
        ctsk.itskpri = 10;
        ctsk.stksz = 8192;
        ctsk.task = (FP)task_space;
        id = tk_cre_tsk(&ctsk);
        if (id <= 0 || tk_sta_tsk(id, i) != E_OK) {
            uart_puts("[M11] task start failed\r\n[M11] FAIL\r\n");
            return 0;
        }
    }
    for (t = 0; t < 2000 && !(g_done[0] && g_done[1]); t += 10) {
        tk_dly_tsk(10);
    }
    if (!(g_done[0] && g_done[1])) {
        uart_puts("[M11] user programs did not finish\r\n[M11] FAIL\r\n");
        return 0;
    }

    uart_puts("[M11] exit A=");
    put_hex8(g_exit[0]);
    uart_puts(" B=");
    put_hex8(g_exit[1]);
    uart_puts("\r\n");
    pass &= check(g_exit[0] == g_val[0] && g_exit[1] == g_val[1],
                  "same VA " USER_DATA_VA ", each task read back its own value");
    pass &= check(g_satp[0] != g_satp[1] && g_satp[0] != rv_kernel_satp
                  && g_satp[1] != rv_kernel_satp,
                  "each task had its own page table (satp)");
    pass &= check(*(volatile uint64_t *)(rv_uspace_data_page(0) + var_off) == (uint64_t)g_val[0]
                  && *(volatile uint64_t *)(rv_uspace_data_page(1) + var_off) == (uint64_t)g_val[1],
                  "values landed in different physical pages");
    pass &= check(*(volatile uint64_t *)&user_var == 0,
                  "shared .user data page untouched");
    pass &= check(rv_mmu_read_satp() == rv_kernel_satp,
                  "init task is back on the kernel page table");

    uart_puts(pass ? "[M11] PASS\r\n" : "[M11] FAIL\r\n");
    return pass;
}
