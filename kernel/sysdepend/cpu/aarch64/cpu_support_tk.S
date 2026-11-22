/* cpu_support_tk.S — AArch64 の T-Kernel ディスパッチャ（128 バイトのフレーム、cpu_attr.h）
 *
 *   RISC-V 版（cpu/rv64/cpu_support_tk.S）と同じ作り:
 *     dispatch_entry   … dispatch()（C の呼び出し）から。呼んだタスクのレジスタをフレームに残して切り替える
 *     dispatch_loop    … schedtsk のフレームを戻し、eret で再開する
 *     割込みからの切り替えは exception.S（フレームの PC = a64_trap_resume）
 *   再開は eret で行う。SPSR に保存した割込みの禁止・許可がそのまま戻る。
 */

#define _in_asm_source_

#include "offset.h"
#include "cpu_attr.h"

#define FRAME_SIZE  TK2_CTX_SIZE

.section .text

.global dispatch_to_schedtsk
.global dispatch_entry
.global dispatch_saved_ctxtsk
.global _tk_ret_int
.global timer_handler_startup
.global knl_task_start_frame_tk
.global dispatch_loop_entry

/* x0 = フレームの位置（isstack）、x1 = タスクの関数。フレームの位置を返す */
knl_task_start_frame_tk:
    mov     x2, #0
.Lframe_zero:
    str     xzr, [x0, x2]
    add     x2, x2, #8
    cmp     x2, #FRAME_SIZE
    b.lo    .Lframe_zero
    str     x1, [x0, #TK2_CTX_X30]
    str     x1, [x0, #TK2_CTX_X21]          /* タスクの入口 */
    adr     x2, knl_task_entry_tramp
    str     x2, [x0, #TK2_CTX_PC]
    add     x2, x0, #FRAME_SIZE
    str     x2, [x0, #TK2_CTX_SP]
    mov     x2, #TK2_INITIAL_SPSR_IRQ
    str     x2, [x0, #TK2_CTX_SPSR]
    ret

/* タスクの最初の入口: task(stacd, exinf)。setup_stacd が x19 = stacd、x20 = exinf を入れている */
knl_task_entry_tramp:
    mov     x0, x19
    mov     x1, x20
    mov     x30, xzr                        /* タスクの関数から戻ったら 0 番地（例外で分かる） */
    br      x21

dispatch_entry:
    mrs     x7, daif                        /* 呼ばれたときの割込みの禁止・許可（再開時に戻す） */
    msr     daifset, #2
    ldr     x4, =ctxtsk
    ldr     x5, [x4]
    cbz     x5, .Ldispatch_loop
    ldr     x6, =dispatch_disabled
    str     wzr, [x6]

    sub     sp, sp, #FRAME_SIZE
    stp     x19, x20, [sp, #TK2_CTX_X19]
    stp     x21, x22, [sp, #TK2_CTX_X21]
    stp     x23, x24, [sp, #TK2_CTX_X23]
    stp     x25, x26, [sp, #TK2_CTX_X25]
    stp     x27, x28, [sp, #TK2_CTX_X27]
    stp     x29, x30, [sp, #TK2_CTX_X29]
    add     x0, sp, #FRAME_SIZE
    str     x0, [sp, #TK2_CTX_SP]
    str     x30, [sp, #TK2_CTX_PC]          /* dispatch() の呼び出し元へ戻る */
    mov     x1, #A64_SPSR_EL1H
    orr     x0, x7, x1                      /* 臨界区間からなら禁止のまま、tk_ena_dsp などからなら許可で戻る */
    str     x0, [sp, #TK2_CTX_SPSR]
    add     x6, x5, #TCB_tskctxb
    mov     x0, sp
    str     x0, [x6, #CTXB_ssp]
    str     xzr, [x4]
    ldr     x0, =tmp_dispatch_stack + 4096
    mov     sp, x0
    mov     x0, x5
    bl      dispatch_saved_ctxtsk

dispatch_loop_entry:
.Ldispatch_loop:
    msr     daifset, #2
    ldr     x6, =dispatch_disabled
    str     wzr, [x6]
    ldr     x5, =schedtsk
    ldr     x5, [x5]
    cbz     x5, .Ldispatch_wait
    ldr     x4, =ctxtsk
    str     x5, [x4]
    mov     x19, x5                         /* x19 以降は次のタスクのフレームから戻すので使ってよい */
    mov     x0, x5
    bl      rv_fpu_switch_in                /* 浮動小数点のレジスタをこのタスクのものに */
    mov     x5, x19
    add     x0, x5, #TCB_tskctxb
    ldr     x1, [x0, #CTXB_ssp]
    cbz     x1, .Ldispatch_wait
    ldr     x2, [x1, #TK2_CTX_PC]
    cbz     x2, .Ldispatch_wait
#if TK2_MMU
    /* タスクのユーザー空間へ切り替える（uatb = TTBR0 の値、0 ならカーネルのページ表）
     * カーネルの部分はどの空間でも同じなので、ここで切り替えても実行は続けられる */
    ldr     x2, [x0, #CTXB_uatb]
    cbnz    x2, 1f
    ldr     x2, =rv_kernel_satp
    ldr     x2, [x2]
1:  mrs     x3, ttbr0_el1
    cmp     x2, x3
    b.eq    2f
    dsb     ishst
    msr     ttbr0_el1, x2
    isb
    tlbi    vmalle1
    dsb     ish
    isb
2:
#endif
    mov     sp, x1
    ldp     x19, x20, [sp, #TK2_CTX_X19]
    ldp     x21, x22, [sp, #TK2_CTX_X21]
    ldp     x23, x24, [sp, #TK2_CTX_X23]
    ldp     x25, x26, [sp, #TK2_CTX_X25]
    ldp     x27, x28, [sp, #TK2_CTX_X27]
    ldp     x29, x30, [sp, #TK2_CTX_X29]
    ldr     x2, [sp, #TK2_CTX_PC]
    ldr     x3, [sp, #TK2_CTX_SPSR]
    msr     elr_el1, x2
    msr     spsr_el1, x3
    ldr     x2, [sp, #TK2_CTX_SP]
    mov     sp, x2
    eret

.Ldispatch_wait:
    /* 実行できるタスクがない: 割込みを許可して待つ（タイマの割込みで時刻を進めるため）
     * 待つ間は実行中のタスクなし（ctxtsk = NULL）。割込みの出口で切り替えの相手と間違えないように */
    ldr     x4, =ctxtsk
    str     xzr, [x4]
    msr     daifclr, #2
    wfi
    msr     daifset, #2
    b       .Ldispatch_loop

dispatch_to_schedtsk:
    ldr     x0, =tmp_dispatch_stack + 4096
    mov     sp, x0
    ldr     x6, =dispatch_disabled
    mov     w0, #2
    str     w0, [x6]
    ldr     x4, =ctxtsk
    str     xzr, [x4]
    ldr     x6, =g_sc_area
    str     wzr, [x6, #4]
    b       .Ldispatch_loop

_tk_ret_int:
    b       dispatch_entry

timer_handler_startup:
    stp     x29, x30, [sp, #-16]!
    bl      timer_handler
    ldp     x29, x30, [sp], #16
    ret

.section .bss
.balign 4096
.global tmp_dispatch_stack
tmp_dispatch_stack:
    .space 4096

.global m5b_badssp_reported
m5b_badssp_reported:
    .space 4
