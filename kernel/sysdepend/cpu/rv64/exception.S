/* exception.S — trap for TK2 (構成A: M-mode MTIMER / 構成B: S-mode STIMER) */
#define _in_asm_source_
#include "rv_priv.h"
#include "cpu_attr.h"
.section .text
.align 4
.globl trap_entry

.extern irq_handler
.extern kernel_sync_exception_handler

trap_entry:
    csrrw sp, CSR_XSCRATCH, sp
    csrci CSR_XSTATUS, XSTATUS_IE
    addi  sp, sp, -288

    sd    x1,   0*8(sp)
    sd    x3,   2*8(sp)
    sd    x4,   3*8(sp)
    sd    x5,   4*8(sp)
    sd    x6,   5*8(sp)
    sd    x7,   6*8(sp)
    sd    x8,   7*8(sp)
    sd    x9,   8*8(sp)
    sd    x10,  9*8(sp)
    sd    x11, 10*8(sp)
    sd    x12, 11*8(sp)
    sd    x13, 12*8(sp)
    sd    x14, 13*8(sp)
    sd    x15, 14*8(sp)
    sd    x16, 15*8(sp)
    sd    x17, 16*8(sp)
    sd    x18, 17*8(sp)
    sd    x19, 18*8(sp)
    sd    x20, 19*8(sp)
    sd    x21, 20*8(sp)
    sd    x22, 21*8(sp)
    sd    x23, 22*8(sp)
    sd    x24, 23*8(sp)
    sd    x25, 24*8(sp)
    sd    x26, 25*8(sp)
    sd    x27, 26*8(sp)
    sd    x28, 27*8(sp)
    sd    x29, 28*8(sp)
    sd    x30, 29*8(sp)
    sd    x31, 30*8(sp)

    csrr  t0, CSR_XEPC
    sd    t0,  31*8(sp)
    csrr  t0, CSR_XSTATUS
    sd    t0,  32*8(sp)
    csrr  t0, CSR_XCAUSE
    sd    t0,  33*8(sp)
    csrr  t0, CSR_XTVAL
    sd    t0,  34*8(sp)

    csrr  t0, CSR_XSCRATCH
    sd    t0,   1*8(sp)
    csrw  CSR_XSCRATCH, sp

    /* U-mode から来た場合 gp はユーザーの値なので、カーネルの gp を入れ直す */
.option push
.option norelax
    la    gp, __global_pointer$
.option pop

    csrr  t0, CSR_XCAUSE
    li    t1, (1 << 63) | XCAUSE_TIMER
#if TK2_MMU
    li    t2, (1 << 63) | 9       /* S-mode の外部割込み（PLIC） */
    beq   t0, t2, .Lext_irq
#endif
    bne   t0, t1, .Lsync_trap

    call  irq_handler
.Lirq_exit:

    /* 割込みで優先度の高いタスクが起きたら、ここでタスクを切り替える */
    mv    a0, sp
    call  rv64_irq_should_preempt
    bnez  a0, .Lpreempt

/*
 * sp が指す 288B トラップフレームから復帰する（共通出口）。
 *   通常: sp = カーネルスタック上のフレーム
 *   再開: sp = タスクスタックへ退避したフレーム（rv64_trap_resume から）
 * v0.1.0 の出口は、全レジスタ復元後に t0/t1 を壊し、sp をフレーム外から
 * 読んでいた（タスク実行中に割込みが入らなかったため表面化していなかった）。
 */
.Lrestore_frame:
#if TK2_MMU
    /* U-mode へ戻る直前: 保留中のシグナルを届ける（処理関数へ飛ぶようにフレームを
     * 書き換える）か、既定の動作でプロセスを終わらせる（2 が返る） */
    ld    t0,  32*8(sp)
    li    t1, XSTATUS_PP
    and   t0, t0, t1
    bnez  t0, 6f
    mv    a0, sp
    call  rv_signal_user_return
    li    t0, 2
    beq   a0, t0, .Luser_exit
6:
#endif
    ld    t0,  31*8(sp)
    csrw  CSR_XEPC, t0
    ld    t0,  32*8(sp)
    /* 保存値のまま戻す: PP（戻り先のモード）も PIE も変えない。
     * 以前は PP を強制していたが、それでは U-mode へ戻れない */
    li    t1, XSTATUS_IE
    not   t1, t1
    and   t0, t0, t1              /* xret までは割込み禁止のまま */
    csrw  CSR_XSTATUS, t0
    /* 次のトラップ用のスタックを xscratch に入れる
     *   S-mode へ戻る: 共有のカーネルスタック
     *   U-mode へ戻る: このタスクのカーネルスタック上端（= フレームの真上） */
#if TK2_MMU
    li    t1, XSTATUS_PP
    and   t1, t0, t1
    bnez  t1, 1f
    addi  t1, sp, 288
    j     2f
1:
#endif
    la    t1, knl_kernel_stack_top
2:
    csrw  CSR_XSCRATCH, t1
    ld    x1, 0*8(sp)
    ld    x3, 2*8(sp)
    ld    x4, 3*8(sp)
    ld    x5, 4*8(sp)
    ld    x6, 5*8(sp)
    ld    x7, 6*8(sp)
    ld    x8, 7*8(sp)
    ld    x9, 8*8(sp)
    ld    x10, 9*8(sp)
    ld    x11, 10*8(sp)
    ld    x12, 11*8(sp)
    ld    x13, 12*8(sp)
    ld    x14, 13*8(sp)
    ld    x15, 14*8(sp)
    ld    x16, 15*8(sp)
    ld    x17, 16*8(sp)
    ld    x18, 17*8(sp)
    ld    x19, 18*8(sp)
    ld    x20, 19*8(sp)
    ld    x21, 20*8(sp)
    ld    x22, 21*8(sp)
    ld    x23, 22*8(sp)
    ld    x24, 23*8(sp)
    ld    x25, 24*8(sp)
    ld    x26, 25*8(sp)
    ld    x27, 26*8(sp)
    ld    x28, 27*8(sp)
    ld    x29, 28*8(sp)
    ld    x30, 29*8(sp)
    ld    x31, 30*8(sp)
    ld    sp,  1*8(sp)            /* 最後に元の sp（x2）を戻す */
    XRET

/*
 * タスク切替（プリエンプション）
 *   1. トラップフレーム（288B）を割り込まれたタスクのスタックへ複写
 *   2. その下にディスパッチ用フレーム（128B）を作り、PC = rv64_trap_resume
 *   3. ctxtsk->tskctxb.ssp に保存し、ディスパッチャのループへ
 */
.Lpreempt:
#if TK2_MMU
    ld    t0, 32*8(sp)
    li    t1, XSTATUS_PP
    and   t0, t0, t1
    bnez  t0, 3f
    mv    t0, sp                  /* U-mode から: フレームはすでにタスクのカーネルスタック上 */
    j     4f
3:
#endif
    ld    t0,  1*8(sp)            /* 割り込まれたタスクの sp */
    addi  t0, t0, -288
    li    t1, 0
    li    t4, 288
.Lcopy_frame:
    add   t2, sp, t1
    ld    t3, 0(t2)
    add   t2, t0, t1
    sd    t3, 0(t2)
    addi  t1, t1, 8
    blt   t1, t4, .Lcopy_frame
4:
    addi  t1, t0, -TK2_CTX_SIZE
    la    t2, rv64_trap_resume
    sd    t2, TK2_CTX_RA(t1)
    sd    t2, TK2_CTX_PC(t1)
    sd    t0, TK2_CTX_SP(t1)      /* 再開時の sp = 退避したトラップフレーム */
    li    t2, XSTATUS_PP
    sd    t2, TK2_CTX_MSTATUS(t1)

    la    t4, ctxtsk
    ld    t5, 0(t4)
    addi  t6, t5, TCB_tskctxb
    sd    t1, CTXB_ssp(t6)
    sd    zero, 0(t4)

    la    t1, knl_kernel_stack_top
    csrw  CSR_XSCRATCH, t1
    la    sp, tmp_dispatch_stack + 4096
    mv    s1, t5
    mv    a0, t5
    call  rv_fpu_switch_out       /* 浮動小数点・ベクトルのレジスタを退避 */
    mv    a0, s1
    call  dispatch_saved_ctxtsk   /* 実行可能キューに残っているか確認 */
    j     dispatch_loop_entry

/*
 * プリエンプトされたタスクの再開点（ディスパッチャが xret で飛んでくる）
 *   sp = タスクスタック上のトラップフレーム
 */
.globl rv64_trap_resume
rv64_trap_resume:
    csrci CSR_XSTATUS, XSTATUS_IE
    j     .Lrestore_frame

#if TK2_MMU
/* 外部割込み: 処理のあと、タイマと同じ出口（タスク切替の判定）へ */
.Lext_irq:
    call  rv_ext_irq_entry
    j     .Lirq_exit
#endif

.Lsync_trap:
#if TK2_MMU
    csrr  t0, CSR_XCAUSE
    li    t1, 8                   /* U-mode からの ecall = システムコール */
    beq   t0, t1, .Luser_syscall
#endif
    mv    a0, sp
    call  kernel_sync_exception_handler
#if TK2_MMU
    li    t0, 2
    beq   a0, t0, .Luser_exit     /* 2 ならユーザープログラム終了 → カーネルへ */
#endif
    bnez  a0, .Lrestore_frame     /* 1 なら（xepc を進めた）フレームから復帰 */
.Lhang:
    wfi
    j     .Lhang

#if TK2_MMU
/*
 * システムコール: 呼んだタスクの文脈で、割込みを許可して実行する
 *   （眠る・待つシステムコールは、T-Kernel の通常のディスパッチで切り替わる）
 *   sp = このタスクのカーネルスタック上のフレーム（呼び出しの間 s0 で保持）
 */
.Luser_syscall:
    mv    s0, sp
    la    t1, knl_kernel_stack_top
    csrw  CSR_XSCRATCH, t1        /* 実行中の S-mode のトラップは共有スタックへ */
    ld    t0, 31*8(sp)
    addi  t0, t0, 4               /* ecall の次から再開 */
    sd    t0, 31*8(sp)
    csrsi CSR_XSTATUS, XSTATUS_IE
    mv    a0, sp
    call  rv_user_syscall         /* 0: ユーザーへ戻る / 0 以外: プログラム終了 */
    csrci CSR_XSTATUS, XSTATUS_IE
    mv    sp, s0
    beqz  a0, .Lrestore_frame

/*
 * ユーザープログラム終了 → rv_run_user の呼び出し元へ
 *   終了コードはフレームの a0 の位置（9*8）に入っている
 *   フレームの真上（sp+288）に rv_run_user が退避したカーネルのレジスタがある
 */
.Luser_exit:
    la    t1, knl_kernel_stack_top
    csrw  CSR_XSCRATCH, t1
    ld    a0, 9*8(sp)
    addi  sp, sp, 288
    j     rv_user_return
#endif
