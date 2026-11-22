/* kernel/tk/m9_user_test.c — M9: U-mode（ユーザーモード）の分離（SMODE=1 かつ MMU=1）
 *
 *   1. ユーザープログラムを U-mode で起動し、ecall（SYS_PUTS）で文字を出して
 *      SYS_EXIT(42) で終わる → rv_run_user が 42 を返す
 *   2. ユーザープログラムがカーネルのメモリ（0x8020_0000）を読む
 *      → ロードページフォールト（scause=13）で強制終了、カーネルは無事
 *   3. ユーザープログラムが特権命令（csrr sstatus）を実行する
 *      → 不正命令例外（scause=2）で強制終了、カーネルは無事
 *   4. ユーザープログラムが長い空ループを回す。その間にタイマ割込みが入り、
 *      U-mode に正しく戻って最後まで走る（システム時刻が進み、exit(7)）
 *
 * 合格の目印: [M9] PASS
 */
#include <stdint.h>
#include <tk/tkernel.h>

extern void uart_puts(const char *s);
extern long rv_run_user(uintptr_t entry, uintptr_t user_sp, long arg);
extern uint8_t __user_start[];
extern void rv_user_prog_entry(void);
extern volatile uint64_t rv_user_fault_cause;
extern volatile uint64_t rv_user_fault_tval;
extern ER tk_get_otm(SYSTIM *pk_tim);

#if defined(__aarch64__)
/* USER_VA_BASE は rv_priv.h（0x0040_0000） */
#include "rv_priv.h"
#define USER_STACK_TOP  (USER_VA_BASE + 0x2000UL)
#define CAUSE_ILLEGAL   0x00UL      /* EC: 未定義命令 */
#define CAUSE_LOAD_PF   0x24UL      /* EC: 下の EL のデータアボート */
#define KERNEL_PEEK_VA  0x40200000UL
#else
#define USER_VA_BASE    0x40000000UL
#define USER_STACK_TOP  0x40002000UL
#define CAUSE_ILLEGAL   2UL
#define CAUSE_LOAD_PF   13UL
#define KERNEL_PEEK_VA  0x80200000UL
#endif

static void put_num(long v)
{
    char buf[24];
    int n = 0;
    unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
    extern void uart_putc(char c);

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
    uart_puts(ok ? "[M9] OK   " : "[M9] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

int m9_run_user_test(void)
{
    uintptr_t entry = USER_VA_BASE +
        ((uintptr_t)rv_user_prog_entry - (uintptr_t)__user_start);
    long r;
    int pass = 1;

    uart_puts("\r\n[M9] U-mode test\r\n");

    r = rv_run_user(entry, USER_STACK_TOP, 0);
    pass &= check(r == 42 && rv_user_fault_cause == 0,
                  "user program ran in U-mode, printed via ecall, exit(42)");

    r = rv_run_user(entry, USER_STACK_TOP, 1);
    pass &= check(r == -1 && rv_user_fault_cause == CAUSE_LOAD_PF
                  && rv_user_fault_tval == KERNEL_PEEK_VA,
                  "user read of kernel memory -> load page fault, killed");

    r = rv_run_user(entry, USER_STACK_TOP, 2);
    pass &= check(r == -1 && rv_user_fault_cause == CAUSE_ILLEGAL,
                  "user privileged instruction -> illegal instruction, killed");

    {
        SYSTIM t0, t1;
        long long ms;
        tk_get_otm(&t0);
        r = rv_run_user(entry, USER_STACK_TOP, 3);
        tk_get_otm(&t1);
        ms = (((long long)t1.hi << 32) | t1.lo) - (((long long)t0.hi << 32) | t0.lo);
        uart_puts("[M9] spin: exit=");
        put_num(r);
        uart_puts(" system time +");
        put_num((long)ms);
        uart_puts(" ms\r\n");
        pass &= check(r == 7 && rv_user_fault_cause == 0 && ms >= 50,
                      "timer interrupts during U-mode spin returned to U-mode correctly");
    }

    uart_puts(pass ? "[M9] PASS\r\n" : "[M9] FAIL\r\n");
    return pass;
}
