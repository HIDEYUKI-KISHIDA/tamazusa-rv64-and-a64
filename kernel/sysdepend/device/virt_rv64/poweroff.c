/*
 * poweroff.c — QEMU virt の電源断（テスト終了時に QEMU を自動で終わらせる）
 *
 *   構成 A（M-mode）: QEMU の終了用デバイス sifive_test（0x100000）に書く
 *                      0x5555 = 正常終了（QEMU の終了コード 0）
 *                      (code << 16) | 0x3333 = 異常終了（終了コード = code）
 *   構成 B（S-mode）: 成功時は SBI SRST 拡張（system_reset）で OpenSBI に頼む。
 *                      失敗時は sifive_test に直接書く（OpenSBI の shutdown は
 *                      理由に関係なく QEMU を終了コード 0 で終わらせるため）。
 *                      OpenSBI は 0x100000 を S-mode から読み書き可にしている。
 */
#include "rv_priv.h"

extern void uart_puts(const char *s);

#ifdef TK2_SMODE
#define SBI_EXT_SRST            0x53525354UL    /* "SRST" */
#define SBI_SRST_RESET          0
#define SBI_SRST_TYPE_SHUTDOWN  0
#define SBI_SRST_REASON_NONE    0
#define SBI_SRST_REASON_FAILURE 1

static void sbi_system_reset(unsigned long type, unsigned long reason)
{
    register unsigned long a0 asm("a0") = type;
    register unsigned long a1 asm("a1") = reason;
    register unsigned long a6 asm("a6") = SBI_SRST_RESET;
    register unsigned long a7 asm("a7") = SBI_EXT_SRST;

    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a6), "r"(a7)
                 : "memory");
}
#endif

#define VIRT_TEST_BASE  RV_IO(0x100000UL)
#define VIRT_TEST_PASS  0x5555U
#define VIRT_TEST_FAIL  0x3333U

static void virt_test_finish(int pass)
{
    volatile unsigned int *test = (volatile unsigned int *)VIRT_TEST_BASE;
    *test = pass ? VIRT_TEST_PASS : ((1U << 16) | VIRT_TEST_FAIL);
}

void rv_poweroff(int pass)
{
    uart_puts(pass ? "[poweroff] PASS -> QEMU exit\r\n"
                   : "[poweroff] FAIL -> QEMU exit\r\n");
#ifdef TK2_SMODE
    if (!pass) {
        virt_test_finish(0);    /* 終了コード 1 */
    }
    sbi_system_reset(SBI_SRST_TYPE_SHUTDOWN,
                     pass ? SBI_SRST_REASON_NONE : SBI_SRST_REASON_FAILURE);
#else
    virt_test_finish(pass);
#endif
    for (;;) {  /* 電源断に失敗した場合はここで止まる */
        asm volatile("wfi");
    }
}
