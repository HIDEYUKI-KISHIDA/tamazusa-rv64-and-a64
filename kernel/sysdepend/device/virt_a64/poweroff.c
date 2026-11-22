/* poweroff.c — テストが終わったら QEMU を終える（PSCI の SYSTEM_OFF）
 *   PSCI には終了コードを渡す方法がないので、合否は直前の「[TEST] ALL PASS / FAILED」の表示で見る */
#include <stdint.h>

extern void uart_puts(const char *s);
extern struct { int checked; int smc; uint32_t version; int cpu_suspend; int system_suspend; int system_off; int system_reset; } psci_info;

void rv_poweroff(int pass)
{
    (void)pass;
    uart_puts("[poweroff] PSCI SYSTEM_OFF\r\n");
    if (psci_info.version != 0 && psci_info.system_off >= 0) {
        /* x0 は関数を呼んだあとで入れる（呼び出しで壊れるため） */
        register uint64_t x0 asm("x0") = 0x84000008u;   /* SYSTEM_OFF */
        if (psci_info.smc) {
            asm volatile("smc #0" : "+r"(x0) :: "memory");
        } else {
            asm volatile("hvc #0" : "+r"(x0) :: "memory");
        }
    }
    for (;;) {
        asm volatile("wfi");
    }
}
