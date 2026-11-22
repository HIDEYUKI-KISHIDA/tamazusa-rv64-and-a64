/* devinit.c — QEMU virt（AArch64）のボード初期化（reset.S から、knl_main の前） */
#include <stdint.h>

extern void uart_pl011_init(void);
extern void uart_puts(const char *s);
extern void gic_init(void);
extern int gic_v3_active(void);
extern void rv_mmu_init(void);
extern void psci_probe(void);

extern uint64_t rv_boot_dtb;

/* デバイスツリーの見出しの印（ビッグエンディアンの 0xD00DFEED） */
static int is_fdt(uint64_t pa)
{
    const volatile uint8_t *b = (const volatile uint8_t *)(uintptr_t)pa;
    return pa != 0 && (pa & 7) == 0 && b[0] == 0xD0 && b[1] == 0x0D && b[2] == 0xFE && b[3] == 0xED;
}

void board_init(void)
{
    /* x0 で渡されなかったとき（QEMU に ELF を -kernel で渡した場合）、QEMU は RAM の先頭に置く */
    if (!is_fdt(rv_boot_dtb)) {
        rv_boot_dtb = is_fdt(0x40000000UL) ? 0x40000000UL : 0;
    }
    uart_pl011_init();
    uart_puts("Tamazusa AArch64 (QEMU virt, GICv3)\r\n");

    gic_init();
    uart_puts(gic_v3_active() ? "[board_init] GIC OK (GICv3, system registers)\r\n"
                              : "[board_init] GIC NG (GICv3 CPU interface not enabled)\r\n");
    psci_probe();       /* PSCI（ファームウェアの電源の機能）の版と機能を表示する */

    rv_mmu_init();      /* これ以降、装置は高いアドレス（RV_IO）から触る */
    {
        extern void rv_uart_irq_init(void);
        rv_uart_irq_init(); /* UART の受信割込み（GIC の SPI 1） */
    }
    /* タイマの開始と割込みの許可は T-Kernel の起動（start_hw_timer）で行う */
}
