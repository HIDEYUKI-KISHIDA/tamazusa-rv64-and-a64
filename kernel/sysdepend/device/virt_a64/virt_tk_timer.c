/* virt_tk_timer.c — Arm の汎用タイマを T-Kernel の timer_handler へつなぐ（10ms 周期） */
#include <tk/typedef.h>
#include "m5b_trace.h"

extern void timer_init(unsigned int hz);
extern void timer_on_irq(void);
extern void timer_handler(void);
#if TK2_LINUX
extern void rv_timeslice_tick(void);
#endif
IMPORT RELTIM_U TIMER_PERIOD;

void virt_timer_init_for_tk(void)
{
    timer_init(100);
}

void virt_timer_on_irq_tk(void)
{
    /* 次の割込みの時刻を入れ直す（しないと割込みが立ったままになる） */
    timer_on_irq();
    if (TIMER_PERIOD == 0) {        /* timer_initialize の前: timer_queue がまだない */
        return;
    }
    if (m5b_ext_tsk_hold_int) {     /* tk_ext_tsk から force_dispatch までは触らない */
        return;
    }
    timer_handler();
#if TK2_LINUX
    rv_timeslice_tick();    /* Linux のプロセスどうしの時分割 */
#endif
}
