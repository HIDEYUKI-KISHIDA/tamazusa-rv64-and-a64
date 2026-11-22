/*

 * Dispatch hooks for M5b RV64 port.

 */



#include <stddef.h>

#include <stdint.h>



#include "kernel.h"

#include "task.h"

#include "ready_queue.h"

#include "cpu_attr.h"
#include "cpu_status.h"
#include "m5b_trace.h"



IMPORT TCB *ctxtsk;

IMPORT TCB *schedtsk;

IMPORT TCB *tcb_table;

IMPORT ID   max_tskid;

IMPORT RDYQUE ready_queue;



extern void dispatch_to_schedtsk(void);

extern void uart_puts(const char *s);

extern void uart_putc(char c);



/* Set to 1 only for interactive UART tracing (slow); ring log needs 0 */
#define M5B_FD_VERBOSE  0

static void m5b_kernel_idle_halt(void)
{
    static int reported;

    if (!reported) {
        reported = 1;
        uart_puts("[M5b] kernel idle (no runnable tasks)\r\n");
    }
    m5b_ext_tsk_hold_int = 0;
    dispatch_to_schedtsk();
}



#if M5B_FD_VERBOSE
static void fd_put_hex64(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    uart_puts("0x");
    for (i = 60; i >= 0; i -= 4) {
        uart_putc(hex[(v >> i) & 0xf]);
    }
}
#endif

static void fd_dump_next(const char *tag, TCB *next)

{

    uintptr_t ssp;

    uintptr_t elr96;

    uintptr_t spsr104;



#if M5B_FD_VERBOSE

    uart_puts(tag);

    if (next == NULL) {

        uart_puts(" next=NULL\r\n");

        return;

    }

    uart_puts(" tskid=");

    fd_put_hex64((uint64_t)next->tskid);

    uart_puts(" state=");

    fd_put_hex64((uint64_t)next->state);

    uart_puts(" pri=");

    fd_put_hex64((uint64_t)next->priority);

    ssp = (uintptr_t)next->tskctxb.ssp;

    uart_puts(" ssp=");

    fd_put_hex64(ssp);

    if (ssp != 0) {

        elr96 = *(uintptr_t *)(ssp + TK2_CTX_ELR);

        spsr104 = *(uintptr_t *)(ssp + TK2_CTX_SPSR);

        uart_puts(" [+96]=");

        fd_put_hex64(elr96);

        uart_puts(" [+104]=");

        fd_put_hex64(spsr104);

    }

    uart_puts("\r\n");

#else

    (void)tag;

    (void)next;

    (void)ssp;

    (void)elr96;

    (void)spsr104;

#endif

}



static BOOL tcb_on_ready_queue(TCB *tcb)

{

    QUEUE *head = &ready_queue.tskque[tcb->priority];

    QUEUE *q;



    for (q = head->next; q != head; q = q->next) {

        if ((TCB *)q == tcb) {

            return TRUE;

        }

    }

    return FALSE;

}



void dispatch_saved_ctxtsk(TCB *tcb)

{

    if (tcb == NULL) {

        return;

    }

    m5b_log_event(M5B_LOG_SAVE, tcb, tcb->tskctxb.ssp);

    if (tcb->state == TS_READY && !tcb_on_ready_queue(tcb)) {

        make_ready(tcb);

    }

}



static TCB *find_runnable_tcb(void)

{

    TCB *best = NULL;

    ID i;



    for (i = MIN_TSKID; i <= max_tskid; i++) {

        TCB *t = &tcb_table[INDEX_TSK(i)];



        if (t->state != TS_READY) {

            continue;

        }

        if (t->tskctxb.ssp == NULL) {

            continue;

        }

        if (best == NULL || t->priority < best->priority) {

            best = t;

        }

    }

    return best;

}



/* M5 app tasks run at priority 10; init_task is 12 and stays READY on the queue */

static TCB *find_ready_at_priority(UB pri)

{

    TCB *found = NULL;

    ID i;



    for (i = MIN_TSKID; i <= max_tskid; i++) {

        TCB *t = &tcb_table[INDEX_TSK(i)];



        if (t->state != TS_READY || t->tskctxb.ssp == NULL) {

            continue;

        }

        if (t->priority != pri) {

            continue;

        }

        if (found == NULL || t->tskid < found->tskid) {

            found = t;

        }

    }

    return found;

}



static BOOL tcb_pointer_valid(TCB *t)

{

    ptrdiff_t idx;



    if (t == NULL || tcb_table == NULL) {

        return FALSE;

    }

    idx = t - tcb_table;

    if (idx < 0 || idx >= NUM_TSKID) {

        return FALSE;

    }

    if (t->tskid == 0) {

        return FALSE;

    }

    return TRUE;

}



static BOOL schedtsk_is_runnable(TCB *t)

{

    if (!tcb_pointer_valid(t)) {

        return FALSE;

    }

    if (t->state == TS_DORMANT || t->state == TS_NONEXIST) {

        return FALSE;

    }

    /* Only TS_READY (incl. RUN) may be dispatched — not TS_WAIT */
    if (t->state != TS_READY) {

        return FALSE;

    }

    if (t->tskctxb.ssp == NULL) {

        return FALSE;

    }

    return TRUE;

}



void rv64_force_dispatch(void)
{
    TCB *next;

    m5b_use_tmp_stack();
    (void)disint();

    m5b_log_event(M5B_LOG_FD, schedtsk, NULL);

    ctxtsk = NULL;

    /* Match upstream: head of global ready queue (post make_non_ready / mutex wake) */
    next = ready_queue_top(&ready_queue);
    fd_dump_next("[FD] top", next);

    if (!schedtsk_is_runnable(next)) {
        next = find_ready_at_priority(10);
        fd_dump_next("[FD] pri10", next);
    }
    if (!schedtsk_is_runnable(next)) {
        next = find_runnable_tcb();
        fd_dump_next("[FD] scan", next);
    }

    if (next != NULL) {
        m5b_frame_fixup_lr(next->tskctxb.ssp);
    }

    schedtsk = next;

    if (schedtsk == NULL) {
        m5b_kernel_idle_halt();
        return;
    }

    m5b_log_event(M5B_LOG_FD, schedtsk, schedtsk->tskctxb.ssp);
    fd_dump_next("[FD] sched", schedtsk);

    m5b_ext_tsk_hold_int = 0;
    dispatch_to_schedtsk();
}



/* fpu_ctx.c が TCB の番号を求めるため */
unsigned long rv_tcb_size(void)
{
    return sizeof(TCB);
}

/* タスク ID → TCB（linux_proc.c が子のタスクに浮動小数点のレジスタを写すため） */
void *rv_tcb_of(ID tskid)
{
    if (tskid < MIN_TSKID || tskid > max_tskid) {
        return NULL;
    }
    return &tcb_table[INDEX_TSK(tskid)];
}

#if TK2_MMU
/*
 * 実行中タスクに専用のユーザー空間（satp）を割り当ててユーザープログラムを走らせる。
 * uatb に入れておけば、切り替えて戻ってきたときもディスパッチャがこの空間に戻す。
 */
extern long rv_run_user(uintptr_t entry, uintptr_t user_sp, long arg);
extern void rv_mmu_switch(uint64_t satp);
extern uint64_t rv_kernel_satp;

extern long rv_run_user2(uintptr_t entry, uintptr_t user_sp, long arg, long arg2);

long rv_run_user_in_space2(uint64_t satp, uintptr_t entry, uintptr_t user_sp, long arg, long arg2)
{
    TCB *self = ctxtsk;
    long r;

    self->tskctxb.uatb = (void *)(uintptr_t)satp;
    rv_mmu_switch(satp);
    r = rv_run_user2(entry, user_sp, arg, arg2);
    self->tskctxb.uatb = NULL;
    rv_mmu_switch(rv_kernel_satp);
    return r;
}

/*
 * 外部割込み（PLIC）の入口。処理の間は「割込み処理中（タスク独立部）」にする。
 * 割込みで起こす相手がちょうど実行中のタスク自身（眠る直前）でも、
 * T-Kernel が起床要求として数えるので取りこぼさない。
 */
extern void rv_ext_irq_handler(void);

/*
 * タイマの割込みごと（10ms）: Linux のプロセスの優先度（RV_LINUX_PRI）の実行可能キューを回す。
 * T-Kernel は同じ優先度のタスクを自動では切り替えないので、計算し続けるプロセスが
 * あると、同じ優先度のほかのプロセス（シェルなど）が動けなくなるため（時分割）
 */
#define RV_LINUX_PRI 10
void rv_timeslice_tick(void)
{
    extern void rv_linux_timer_tick(void);
    EnterTaskIndependent();
    rv_linux_timer_tick();              /* setitimer・alarm の SIGALRM */
    LeaveTaskIndependent();
    /* TCB の priority は内部の値（優先度 - MIN_PRI）なので int_priority で比べる */
    if (ctxtsk != NULL && ctxtsk->priority == int_priority(RV_LINUX_PRI)) {
        EnterTaskIndependent();
        _tk_rot_rdq(RV_LINUX_PRI);
        LeaveTaskIndependent();
    }
}

void rv_ext_irq_entry(void)
{
    EnterTaskIndependent();
    rv_ext_irq_handler();
    LeaveTaskIndependent();
}

/* fork の子: 空間 satp で、フレームのレジスタのまま U-mode へ戻る */
extern long rv_run_user_frame(const uint64_t *frame);

long rv_run_user_frame_in_space(uint64_t satp, const uint64_t *frame)
{
    TCB *self = ctxtsk;
    long r;

    self->tskctxb.uatb = (void *)(uintptr_t)satp;
    rv_mmu_switch(satp);
    r = rv_run_user_frame(frame);
    self->tskctxb.uatb = NULL;
    rv_mmu_switch(rv_kernel_satp);
    return r;
}

/* execve: 実行中のタスクの空間を入れ替える（切り替えて戻ってきたときもこの空間になる） */
void rv_set_current_space(uint64_t satp)
{
    ctxtsk->tskctxb.uatb = (void *)(uintptr_t)satp;
    rv_mmu_switch(satp);
}

long rv_run_user_in_space(uint64_t satp, uintptr_t entry, uintptr_t user_sp, long arg)
{
    return rv_run_user_in_space2(satp, entry, user_sp, arg, 0);
}
#endif

/*
 * 割込み出口でタスクを切り替えるか（exception.S から呼ばれる）
 *   実行中タスクがあり、より優先すべきタスク（schedtsk）が決まっていて、
 *   ディスパッチ禁止でなければ切り替える。
 */
int rv64_irq_should_preempt(const uint64_t *frame)
{
    /* U-mode から割り込まれた場合も切り替える（第3段: フレームはタスクごとの
     * カーネルスタック上にあるので、そのまま退避先として使える） */
    (void)frame;
    if (ctxtsk == NULL || schedtsk == NULL || ctxtsk == schedtsk) {
        return 0;
    }
    if (dispatch_disabled != 0 || m5b_ext_tsk_hold_int) {
        return 0;
    }
    if (isTaskIndependent()) {
        return 0;
    }
    return 1;
}
