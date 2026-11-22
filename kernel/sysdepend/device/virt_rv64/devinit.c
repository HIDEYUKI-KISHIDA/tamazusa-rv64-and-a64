#include "board_config.h"
#include "rv_priv.h"

extern void uart_pl011_init(void);
extern void uart_puts(const char *s);
extern void timer_init(unsigned int hz);

static inline void enable_irq(void)
{
    asm volatile("csrs " RV_STR(CSR_XSTATUS) ", %0" :: "r"((unsigned long)XSTATUS_IE) : "memory");
    asm volatile("csrs " RV_STR(CSR_XIE) ", %0" :: "r"((unsigned long)XIE_TIMER));
}

#if TK2_MMU
extern void rv_mmu_init(void);
extern void rv_plic_init(void);
#endif

void board_init(void)
{
    uart_pl011_init();
    uart_puts(BOARD_INIT_TAG "\r\n");
    uart_puts("[board_init] UART OK\r\n");
#if TK2_MMU
    rv_mmu_init();
#endif
    timer_init(100);
    #ifdef TK2_SMODE
    uart_puts("[board_init] SBI timer OK\r\n");
#else
    uart_puts("[board_init] CLINT timer OK\r\n");
#endif
#if TK2_MMU
    rv_plic_init();
    uart_puts("[board_init] PLIC OK (UART RX IRQ 10)\r\n");
#endif
    /* 浮動小数点ユニットとベクトルユニットを使える状態にする（RISC-V 用にコンパイルした
     * ユーザープログラムは浮動小数点の命令を普通に使い、RVA23 向けならベクトル命令も使う） */
    asm volatile("csrs " RV_STR(CSR_XSTATUS) ", %0"
                 :: "r"((unsigned long)(XSTATUS_FS_INITIAL | XSTATUS_VS_INITIAL)));
    {
        extern void rv_fpu_init(void);
        extern int rv_fpu_vlenb(void);
        rv_fpu_init();
        uart_puts(rv_fpu_vlenb() > 0 ? "[board_init] FPU + vector context switch OK\r\n"
                                     : "[board_init] FPU context switch OK (no vector)\r\n");
    }
    enable_irq();
    uart_puts("[board_init] IRQ enabled\r\n");
}
