/* devinit.c — Allwinner A733（Orange Pi 4 Pro）のボード初期化（reset.S から、knl_main の前）
 *
 *   U-Boot の booti から EL2 で入ってくる（デバイスツリーは x0）。reset.S が EL1 へ降りる。
 *   最初の段階は、起動・GICv3・タイマ・PSCI・MMU・EL0 のプログラム（M5〜M12）だけを確かめる。
 */
#include <stdint.h>

extern void uart_pl011_init(void);
extern void uart_puts(const char *s);
extern void gic_init(void);
extern int gic_v3_active(void);
extern void rv_mmu_init(void);
extern void psci_probe(void);
extern uint32_t boot_entry_el;
extern uint64_t rv_boot_dtb;

static void put_hex(uint64_t v)
{
    static const char h[] = "0123456789abcdef";
    char b[19];
    int i;
    b[0] = '0';
    b[1] = 'x';
    for (i = 0; i < 16; i++) {
        b[2 + i] = h[(v >> ((15 - i) * 4)) & 15];
    }
    b[18] = 0;
    uart_puts(b);
}

void board_init(void)
{
    uint64_t mpidr, midr;

    uart_pl011_init();
    uart_puts("\r\nTamazusa AArch64 (Allwinner A733, Orange Pi 4 Pro)\r\n");
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    asm volatile("mrs %0, midr_el1" : "=r"(midr));
    uart_puts("[board_init] MPIDR ");
    put_hex(mpidr);
    uart_puts("  MIDR ");
    put_hex(midr);
    uart_puts("  DTB ");
    put_hex(rv_boot_dtb);
    uart_puts("\r\n");

    gic_init();
    uart_puts(gic_v3_active() ? "[board_init] GIC OK (GICv3, system registers)\r\n"
                              : "[board_init] GIC NG (GICv3 CPU interface not enabled)\r\n");
    psci_probe();

    rv_mmu_init();
    uart_puts("[board_init] MMU on\r\n");
}
