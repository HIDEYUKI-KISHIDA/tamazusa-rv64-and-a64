/* tk_port_api.c — M5b: tk_* API → _tk_* kernel entry points */



#if defined(M5B_TKERNEL)



#include "kernel.h"

#include "task.h"

#include "wait.h"

#include "cpu_status.h"

#include "cpu_task.h"

#include "m5b_trace.h"



extern void uart_putc(char c);

extern void uart_puts(const char *s);



/* Same steps as LOCAL _ter_tsk() in task_manage.c (not exported upstream) */

static void m5b_ter_tsk(TCB *tcb)

{

    TSTAT state;



    if (tcb == NULL) {

        return;

    }



    if (tcb->svclocked != NULL) {

        AllUnlockSVC(tcb);

    }



    state = (TSTAT)tcb->state;

    if (state == TS_READY) {

        make_non_ready(tcb);

    } else if ((state & TS_WAIT) != 0) {

        wait_cancel(tcb);

        if (tcb->wspec != NULL && tcb->wspec->rel_wai_hook != NULL) {

            (*tcb->wspec->rel_wai_hook)(tcb);

        }

    }



#ifdef NUM_MTXID

    signal_all_mutex(tcb);

#endif

    cleanup_context(tcb);

}



void tk_dispatch_start(void)

{

    force_dispatch();

}



ID tk_cre_tsk(CONST T_CTSK *pk)

{

    return _tk_cre_tsk(pk);

}



ER tk_sta_tsk(ID id, INT st)

{

    return _tk_sta_tsk(id, st);

}



ER tk_dis_dsp(void)

{

    return _tk_dis_dsp();

}



ER tk_ena_dsp(void)

{

    return _tk_ena_dsp();

}



ER tk_chg_pri(ID tskid, PRI tskpri)

{

    return _tk_chg_pri(tskid, tskpri);

}



ER tk_ref_tsk(ID tskid, T_RTSK *pk)

{

    return _tk_ref_tsk(tskid, pk);

}



ER tk_rot_rdq(PRI pri)

{

    return _tk_rot_rdq(pri);

}



void tk_ext_tsk(void)

{

    TCB *tcb = ctxtsk;



    /*

     * Do NOT call upstream _tk_ext_tsk():

     *  - DORMANT_STACK_SIZE dummy on stack can corrupt LR before force_dispatch

     *  - nested END_CRITICAL re-enables IRQ (hold_int mitigates, but bypass is safer)

     *

     * Run termination on tmp_dispatch_stack: make_dormant/setup_context touch

     * isstack while C still uses the task SP — overlapping use can smash LR.

     */

    m5b_ext_tsk_hold_int = 1;

    m5b_use_tmp_stack();

    (void)disint();



    m5b_log_event(M5B_LOG_EXT, tcb, (tcb != NULL) ? tcb->tskctxb.ssp : NULL);



    m5b_ter_tsk(tcb);

    make_dormant(tcb);

    rv64_force_dispatch();

    /* no return */

}



ID tk_cre_mtx(CONST T_CMTX *pk)

{

    return _tk_cre_mtx(pk);

}



ER tk_loc_mtx(ID id, TMO tmout)

{

    return _tk_loc_mtx(id, tmout);

}



ER tk_dly_tsk(RELTIM dlytim)
{
    return _tk_dly_tsk(dlytim);
}

ER tk_get_otm(SYSTIM *pk_tim)
{
    return _tk_get_otm(pk_tim);
}

ID tk_get_tid(void)
{
    return _tk_get_tid();
}

ER tk_slp_tsk(TMO tmout)
{
    return _tk_slp_tsk(tmout);
}

ER tk_wup_tsk(ID tskid)
{
    return _tk_wup_tsk(tskid);
}

ER tk_del_tsk(ID tskid)
{
    return _tk_del_tsk(tskid);
}

ER tk_unl_mtx(ID id)

{

    return _tk_unl_mtx(id);

}



#endif

