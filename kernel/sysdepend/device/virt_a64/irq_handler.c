#include <stdint.h>
#include <board_hw.h>   /* ボードごと（-I の順で選ぶ） */

extern unsigned int gic_ack(void);
extern void         gic_eoi(unsigned int irq);

#if defined(M5B_TKERNEL)
extern void virt_timer_on_irq_tk(void);
#else
extern void timer_on_irq(void);
#endif

#if TK2_LINUX
extern void rv_dev_irq_entry(unsigned irq);
#endif

#define GIC_SPURIOUS   1023

/* 受け付けたタイマ割り込みの数（M5c の試験が見る） */
volatile unsigned long gic_timer_irq_count;

void irq_handler(void)
{
    unsigned int irq = gic_ack();

    if (irq == GIC_SPURIOUS) { return; }
    if (irq == BOARD_TIMER_IRQ) { gic_timer_irq_count++; }

    switch (irq) {
    case BOARD_TIMER_IRQ:
#if defined(M5B_TKERNEL)
        virt_timer_on_irq_tk();
#else
        timer_on_irq();
#endif
        break;
    default:
#if TK2_LINUX
        rv_dev_irq_entry(irq);          /* 装置（virtio-blk など） */
#endif
        break;
    }

    gic_eoi(irq);
}
