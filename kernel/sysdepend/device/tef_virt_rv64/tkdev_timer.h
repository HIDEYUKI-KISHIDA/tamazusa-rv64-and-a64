#ifndef _TKDEV_TIMER_
#define _TKDEV_TIMER_

#include <tk/typedef.h>
#include "rv_priv.h"

#define MIN_TIMER_PERIOD    1000
#define MAX_TIMER_PERIOD    1000000

IMPORT void virt_timer_init_for_tk(void);
IMPORT void virt_timer_on_irq_tk(void);

Inline void init_hw_timer(void) { virt_timer_init_for_tk(); }
Inline void start_hw_timer(void)
{
    virt_timer_init_for_tk();
    asm volatile("csrs " RV_STR(CSR_XSTATUS) ", %0" :: "r"((unsigned long)XSTATUS_IE) : "memory");
    asm volatile("csrs " RV_STR(CSR_XIE) ", %0" :: "r"((unsigned long)XIE_TIMER));
}
Inline void terminate_hw_timer(void) { }
Inline void clear_hw_timer_interrupt(void) { }
Inline void end_of_hw_timer_interrupt(void) { }

Inline UINT get_hw_timer_usec(void) { return 0; }
/* 前回ティックからの経過（未実装: ティック単位の精度で返す） */
Inline UINT get_hw_timer_nsec(void) { return 0; }

#endif
