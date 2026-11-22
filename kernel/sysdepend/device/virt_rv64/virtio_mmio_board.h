/* virtio_mmio_board.h — QEMU virt の virtio-mmio の窓の場所と割込み番号（RISC-V・AArch64 共通の装置の処理が使う）
 *
 *   RISC-V（QEMU virt）: 窓は 0x1000_1000 から 0x1000 おきに 8 個、窓 i の割込みは PLIC の 1+i 番
 *   AArch64（QEMU virt）: 窓は 0x0A00_0000 から 0x200 おきに 32 個、窓 i の割込みは SPI 16+i（INTID 48+i）
 *   VIRTIO_IRQ_ENABLE(n) は割込みを受け付けるようにする（PLIC か GIC）。
 *   VIRTIO_FENCE() は、装置とのやり取りの前後の順序を守るための区切り。
 */
#ifndef VIRTIO_MMIO_BOARD_H
#define VIRTIO_MMIO_BOARD_H

#include <rv_priv.h>

#if defined(__aarch64__)
#define VIRTIO_MMIO_BASE    RV_IO(0x0A000000UL)
#define VIRTIO_MMIO_STRIDE  0x200UL
#define VIRTIO_MMIO_COUNT   32
#define VIRTIO_MMIO_IRQ(i)  (48 + (i))
extern void gic_spi_enable(unsigned irq);
#define VIRTIO_IRQ_ENABLE(n) gic_spi_enable(n)
#define VIRTIO_FENCE()      __asm__ volatile("dmb sy" ::: "memory")
#else
#define VIRTIO_MMIO_BASE    RV_IO(0x10001000UL)
#define VIRTIO_MMIO_STRIDE  0x1000UL
#define VIRTIO_MMIO_COUNT   8
#define VIRTIO_MMIO_IRQ(i)  (1 + (i))
extern void rv_plic_enable(unsigned irq);
#define VIRTIO_IRQ_ENABLE(n) rv_plic_enable(n)
#define VIRTIO_FENCE()      __asm__ volatile("fence rw, rw" ::: "memory")
#endif

#endif
