/* M5b T-Kernel dispatcher — 128-byte frame (cpu_attr.h / switch.S) */

#define _in_asm_source_

#include "offset.h"
#include "cpu_attr.h"
#include "rv_priv.h"

#define FRAME_SIZE  TK2_CTX_SIZE
#define MPP_BITS    XSTATUS_PP
#define MSTATUS_RUN XSTATUS_RUN

.section .text

.global dispatch_to_schedtsk
.global dispatch_entry
.global dispatch_saved_ctxtsk
.global _tk_ret_int
.global timer_handler_startup
.global knl_task_start_frame_tk
.global dispatch_loop_entry

knl_task_start_frame_tk:
    /* a0 = isstack (frame base, same as AArch64 M5b) */
    mv      t0, a0
    li      t1, 0
    li      t2, 16
.Lframe_zero:
    sd      zero, 0(t0)
    addi    t0, t0, 8
    addi    t1, t1, 1
    blt     t1, t2, .Lframe_zero
    sd      a1, TK2_CTX_RA(a0)
    sd      a1, TK2_CTX_S2(a0)          /* タスクの入口 */
    la      t1, knl_task_entry_tramp
    sd      t1, TK2_CTX_PC(a0)
    addi    t1, a0, FRAME_SIZE
    sd      t1, TK2_CTX_SP(a0)
    li      t2, TK2_INITIAL_MSTATUS_IRQ
    sd      t2, TK2_CTX_MSTATUS(a0)
    ret

/*
 * タスクの最初の入口: task(stacd, exinf) として呼ぶ
 *   setup_stacd が s0 = stacd、s1 = exinf を入れている（s2 = タスクの関数）
 *   以前は a0/a1 を渡さず、タスクが stacd を使うと不定値になっていた
 */
knl_task_entry_tramp:
    mv      a0, s0
    mv      a1, s1
    jr      s2

dispatch_entry:
    csrci   CSR_XSTATUS, XSTATUS_IE
    la      t4, ctxtsk
    ld      t5, 0(t4)
    beqz    t5, .Ldispatch_loop
    la      t6, dispatch_disabled
    sw      zero, 0(t6)

    addi    sp, sp, -FRAME_SIZE
    sd      ra,  TK2_CTX_RA(sp)
    addi    t0, sp, FRAME_SIZE
    sd      t0,  TK2_CTX_SP(sp)
    sd      s0,  TK2_CTX_S0(sp)
    sd      s1,  TK2_CTX_S1(sp)
    sd      s2,  TK2_CTX_S2(sp)
    sd      s3,  TK2_CTX_S3(sp)
    sd      s4,  TK2_CTX_S4(sp)
    sd      s5,  TK2_CTX_S5(sp)
    sd      s6,  TK2_CTX_S6(sp)
    sd      s7,  TK2_CTX_S7(sp)
    sd      s8,  TK2_CTX_S8(sp)
    sd      s9,  TK2_CTX_S9(sp)
    sd      s10, TK2_CTX_S10(sp)
    sd      s11, TK2_CTX_S11(sp)
    /* HLNG: resume with xret to saved ra (not csr xepc — dispatch() is a C call) */
    sd      ra,  TK2_CTX_PC(sp)
    csrr    t0, CSR_XSTATUS
    li      t1, MPP_BITS
    or      t0, t0, t1
    sd      t0,  TK2_CTX_MSTATUS(sp)
    addi    t6, t5, TCB_tskctxb
    sd      sp,  CTXB_ssp(t6)
    sd      zero, 0(t4)
    mv      s1, t5                  /* s レジスタはフレームに退避済み（自由に使える） */
    mv      a0, t5
    call    rv_fpu_switch_out       /* 浮動小数点・ベクトルのレジスタを退避 */
    mv      a0, s1
    call    dispatch_saved_ctxtsk

dispatch_loop_entry:
.Ldispatch_loop:
    la      t6, dispatch_disabled
    sw      zero, 0(t6)
    la      t5, schedtsk
    ld      t5, 0(t5)
    beqz    t5, .Ldispatch_wait
    la      t4, ctxtsk
    sd      t5, 0(t4)
    mv      s1, t5
    mv      a0, t5
    call    rv_fpu_switch_in        /* このタスクの浮動小数点・ベクトルのレジスタを戻す */
    mv      t5, s1
    addi    a0, t5, TCB_tskctxb
    ld      sp,  CTXB_ssp(a0)
    beqz    sp, .Ldispatch_wait
    ld      t0,  TK2_CTX_PC(sp)
    beqz    t0, .Ldispatch_wait
    ld      ra,  TK2_CTX_RA(sp)
    ld      s0,  TK2_CTX_S0(sp)
    ld      s1,  TK2_CTX_S1(sp)
    ld      s2,  TK2_CTX_S2(sp)
    ld      s3,  TK2_CTX_S3(sp)
    ld      s4,  TK2_CTX_S4(sp)
    ld      s5,  TK2_CTX_S5(sp)
    ld      s6,  TK2_CTX_S6(sp)
    ld      s7,  TK2_CTX_S7(sp)
    ld      s8,  TK2_CTX_S8(sp)
    ld      s9,  TK2_CTX_S9(sp)
    ld      s10, TK2_CTX_S10(sp)
    ld      s11, TK2_CTX_S11(sp)
    ld      t0,  TK2_CTX_MSTATUS(sp)
    li      t1, (XSTATUS_PP | XSTATUS_PIE | XSTATUS_FS_INITIAL | XSTATUS_VS_INITIAL)
    or      t0, t0, t1
    li      t1, XSTATUS_IE
    not     t1, t1
    and     t0, t0, t1          /* xret までは割込み禁止（xret が IE←PIE） */
    /* プリエンプトされたタスク（再開点 rv64_trap_resume）へは割込み禁止のまま戻る。
     * 許可して戻ると、フレームを戻す前に次の割込みが入り、そのたびにタスクの
     * スタックへフレーム（288+128B）が積み増される。割込みが続くとスタックが
     * あふれ、下にある T-Kernel の表（ミューテックスなど）を壊していた。
     * 割込みの許可は、フレームを戻す xret が保存値（元の IE）で行う */
    ld      t2,  TK2_CTX_PC(sp)
    la      t3, rv64_trap_resume
    bne     t2, t3, 5f
    li      t1, XSTATUS_PIE
    not     t1, t1
    and     t0, t0, t1
5:
    csrw    CSR_XSTATUS, t0
    ld      t0,  TK2_CTX_PC(sp)
    csrw    CSR_XEPC, t0
#if TK2_MMU
    /* タスクのユーザー空間へ切り替える（uatb = satp の値、0 ならカーネルのページ表）
     * カーネルの部分はどの空間でも同じなので、ここで切り替えても実行は続けられる */
    ld      t0,  CTXB_uatb(a0)
    bnez    t0, .Lsatp_have
    la      t0, rv_kernel_satp
    ld      t0, 0(t0)
.Lsatp_have:
    csrr    t1, satp
    beq     t0, t1, .Lsatp_same
    csrw    satp, t0
    sfence.vma zero, zero
.Lsatp_same:
#endif
    la      t0, knl_kernel_stack_top
    csrw    CSR_XSCRATCH, t0    /* S-mode のタスクへ: 次のトラップは共有スタック */
    ld      t0,  TK2_CTX_SP(sp)
    mv      sp, t0
    XRET

.Ldispatch_wait:
    /* 実行可能タスクなし: 割込みを許可して待つ（タイマ割込みで時刻を進めるため）
     * 待機中は実行中タスクなし（ctxtsk = NULL）。ここで残っていると、
     * 割込み出口でそのタスクを切り替え対象と誤認する */
    la      t4, ctxtsk
    sd      zero, 0(t4)
    csrsi   CSR_XSTATUS, XSTATUS_IE
    wfi
    csrci   CSR_XSTATUS, XSTATUS_IE
    j       .Ldispatch_loop

dispatch_to_schedtsk:
    la      sp, tmp_dispatch_stack + 4096
    la      t6, dispatch_disabled
    li      t0, 2
    sw      t0, 0(t6)
    la      t4, ctxtsk
    sd      zero, 0(t4)
    la      t6, g_sc_area
    sw      zero, 4(t6)
    j       .Ldispatch_loop

_tk_ret_int:
    j       dispatch_entry

timer_handler_startup:
    addi    sp, sp, -16
    sd      ra, 0(sp)
    call    timer_handler
    ld      ra, 0(sp)
    addi    sp, sp, 16
    ret

.section .bss
.align 12
.global tmp_dispatch_stack
tmp_dispatch_stack:
    .space 4096

.global m5b_badssp_reported
m5b_badssp_reported:
    .space 4
