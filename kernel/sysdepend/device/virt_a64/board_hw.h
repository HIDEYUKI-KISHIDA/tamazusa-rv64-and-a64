#ifndef BOARD_HW_H
#define BOARD_HW_H

/* QEMU virt — §4 of tk2-aarch64-port-spec.md */
#define BOARD_UART_BASE       0x09000000UL
#define BOARD_GICD_BASE       0x08000000UL
#define BOARD_GICC_BASE       0x08010000UL   /* GICv2 の CPU interface */
#define BOARD_GICR_BASE       0x080A0000UL   /* GICv3 の Redistributor（make GIC=3） */
#define BOARD_TIMER_IRQ       30
#define BOARD_KERNEL_LOAD     0x40200000UL
/* MMU: L1[0]=Device (MMIO low), L1[1+]=Normal (RAM @ 0x40000000+) */
#define BOARD_MMU_DEV_L1_END  1u

#endif /* BOARD_HW_H */
