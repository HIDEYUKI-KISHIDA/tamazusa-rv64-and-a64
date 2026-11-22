extern void uart_puts(const char *s);
#if defined(__aarch64__)
#define KNL_HELLO     "Hello AArch64 TK2\r\n"
#define RV_MODE_NAME  "EL1"
#else
#include "rv_priv.h"
#define KNL_HELLO     "Hello RV64 TK2\r\n"
#endif

#if defined(M5B_TKERNEL) || defined(TK2_TKERNEL)
extern void tk_port_boot(void);

void knl_main(void)
{
    uart_puts(KNL_HELLO);
    uart_puts("Running at " RV_MODE_NAME "\r\n");
    tk_port_boot();
}
#else
extern int m5_run_tk_task(void);

void knl_main(void)
{
    uart_puts("Hello RV64 TK2\r\n");
    m5_run_tk_task();
    for (;;) {
        asm volatile("wfi");
    }
}
#endif
