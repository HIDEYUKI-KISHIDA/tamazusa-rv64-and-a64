#include <tk/typedef.h>
#include "m5b_trace.h"

extern void timer_init(unsigned int hz);
extern void timer_handler(void);
IMPORT RELTIM_U TIMER_PERIOD;

void virt_timer_init_for_tk(void)
{
    timer_init(100);
}

extern void timer_on_irq(void);
extern void rv_timeslice_tick(void);

void virt_timer_on_irq_tk(void)
{
    /* 次回の割込み時刻を必ず設定し直す（しないと割込みが出っぱなし／止まる） */
    timer_on_irq();
    if (TIMER_PERIOD == 0) {
        return;
    }
    if (m5b_ext_tsk_hold_int) {
        return;
    }
    timer_handler();
#if TK2_MMU
    rv_timeslice_tick();    /* Linux のプロセスどうしの時分割 */
#endif
}
