#ifndef _TKDEV_TIMER_
#define _TKDEV_TIMER_

#include <tk/typedef.h>

#define MIN_TIMER_PERIOD    1000
#define MAX_TIMER_PERIOD    1000000

IMPORT void virt_timer_init_for_tk(void);
IMPORT void virt_timer_on_irq_tk(void);

Inline void init_hw_timer(void) { virt_timer_init_for_tk(); }
Inline void start_hw_timer(void)
{
    virt_timer_init_for_tk();
    /* tk_port_boot でマスクした IRQ を、タイマ準備完了後に許可 */
    asm volatile("msr daifclr, #0x2" ::: "memory");
}
Inline void terminate_hw_timer(void) { }
Inline void clear_hw_timer_interrupt(void) { }
Inline void end_of_hw_timer_interrupt(void) { }

/* Stub: elapsed us since last tick (M5b uses 0 until virt counter wired) */
Inline UINT get_hw_timer_usec(void) { return 0; }
Inline UINT get_hw_timer_nsec(void) { return 0; }

#endif
