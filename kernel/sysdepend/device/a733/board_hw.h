#ifndef BOARD_HW_H
#define BOARD_HW_H

/* Allwinner A733（Orange Pi 4 Pro）
 *   出典: Orange Pi の Linux（orange-pi-5.15-sun60iw2）のデバイスツリー、U-Boot（v2018.05-a733）
 *   UART0  0x0250_0000（16550 互換、レジスタは 4 バイトおき）、割込み SPI 2（INTID 34）
 *   GICv3  GICD 0x0340_0000、GICR 0x0346_0000
 *   タイマ 24MHz、EL1 の物理タイマは PPI 14（INTID 30）
 *   RAM    0x4000_0000 から
 */
#define BOARD_UART_BASE       0x02500000UL
#define BOARD_UART_IRQ        34
#define BOARD_GICD_BASE       0x03400000UL
#define BOARD_GICR_BASE       0x03460000UL
#define BOARD_TIMER_IRQ       30
#define BOARD_KERNEL_LOAD     0x40200000UL
#define BOARD_A733            1

#endif /* BOARD_HW_H */
