/* exception.S — AArch64 の例外ベクタ（EL1）
 *
 *   割込み（IRQ）: 304 バイトのトラップフレームを、いま使っているスタック（割り込まれたタスクのスタック）に積み、
 *     irq_handler を呼ぶ。終わったら、より優先すべきタスクが起きていればここで切り替える（プリエンプション）。
 *     切り替えるときは、トラップフレームの下にディスパッチャのフレーム（PC = a64_trap_resume）を作って
 *     tskctxb.ssp に残す。再開するとき、ディスパッチャが eret で a64_trap_resume へ来て、トラップフレームから戻る。
 *   RISC-V 版は割込みに共有のスタックを使い、フレームをタスクのスタックへ写していた。AArch64 では
 *   EL1 から EL1 への例外で同じスタックのまま入るので、フレームは最初からタスクのスタックにあり、写す必要がない。
 *   トラップフレームから戻るまで割込みは禁止のまま（入れ子にしない。タスクのスタックが積み増されないように）。
 */

#define _in_asm_source_
#include "offset.h"
#include "cpu_attr.h"

.macro SAVE_FRAME
    sub     sp, sp, #A64_TF_SIZE
    stp     x0, x1,   [sp, #16 * 0]
    stp     x2, x3,   [sp, #16 * 1]
    stp     x4, x5,   [sp, #16 * 2]
    stp     x6, x7,   [sp, #16 * 3]
    stp     x8, x9,   [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    str     x30,      [sp, #16 * 15]
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0, x1,   [sp, #A64_TF_ELR]
    add     x0, sp, #A64_TF_SIZE
    mrs     x1, esr_el1
    stp     x0, x1,   [sp, #A64_TF_SP]
    mrs     x0, far_el1
    str     x0,       [sp, #A64_TF_FAR]
.endm

/* EL0 からの例外: 元の sp は SP_EL0 */
.macro SAVE_FRAME_EL0
    sub     sp, sp, #A64_TF_SIZE
    stp     x0, x1,   [sp, #16 * 0]
    stp     x2, x3,   [sp, #16 * 1]
    stp     x4, x5,   [sp, #16 * 2]
    stp     x6, x7,   [sp, #16 * 3]
    stp     x8, x9,   [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    str     x30,      [sp, #16 * 15]
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0, x1,   [sp, #A64_TF_ELR]
    mrs     x0, sp_el0
    mrs     x1, esr_el1
    stp     x0, x1,   [sp, #A64_TF_SP]
    mrs     x0, far_el1
    mrs     x1, tpidr_el0
    stp     x0, x1,   [sp, #A64_TF_FAR]
.endm

.section .text.vectors, "ax"
.balign 2048
.global a64_vectors
a64_vectors:
    /* 現在の EL、SP_EL0 を使用中（使わない） */
    .balign 0x80
    b       .Lbad
    .balign 0x80
    b       .Lbad
    .balign 0x80
    b       .Lbad
    .balign 0x80
    b       .Lbad
    /* 現在の EL、SP_EL1 を使用中（カーネルとタスク） */
    .balign 0x80
    b       .Lel1_sync
    .balign 0x80
    b       .Lel1_irq
    .balign 0x80
    b       .Lbad
    .balign 0x80
    b       .Lbad
    /* 下の EL（EL0、AArch64）… ユーザー空間 */
    .balign 0x80
    b       .Lel0_sync
    .balign 0x80
    b       .Lel0_irq
    .balign 0x80
    b       .Lbad
    .balign 0x80
    b       .Lbad
    /* 下の EL（AArch32）… 使わない */
    .balign 0x80
    b       .Lbad
    .balign 0x80
    b       .Lbad
    .balign 0x80
    b       .Lbad
    .balign 0x80
    b       .Lbad

.section .text
.balign 4

.Lel1_irq:
    SAVE_FRAME
    bl      irq_handler
    /* 割込みで優先度の高いタスクが起きたら、ここで切り替える */
    mov     x0, sp
    bl      rv64_irq_should_preempt
    cbnz    x0, .Lpreempt

/* sp が指すトラップフレームから戻る（共通の出口） */
.Lrestore_frame:
#if TK2_LINUX
    /* EL0 へ戻る直前: 保留中のシグナルを届ける（フレームを処理関数へ書き換える）か、
     * 既定の動作でプロセスを終わらせる（2 が返る） */
    ldr     x0, [sp, #A64_TF_SPSR]
    tst     x0, #0xf
    b.ne    3f
    mov     x0, sp
    bl      rv_signal_user_return
    cmp     x0, #2
    b.eq    .Luser_exit
3:
#endif
    ldp     x0, x1,   [sp, #A64_TF_ELR]
    msr     elr_el1, x0
    msr     spsr_el1, x1
    tst     x1, #0xf                        /* M[3:0] = 0 なら EL0 へ戻る: SP_EL0 を戻す */
    b.ne    1f
    ldr     x0, [sp, #A64_TF_SP]
    msr     sp_el0, x0
    ldr     x0, [sp, #A64_TF_TLS]
    msr     tpidr_el0, x0
1:
    ldp     x0, x1,   [sp, #16 * 0]
    ldp     x2, x3,   [sp, #16 * 1]
    ldp     x4, x5,   [sp, #16 * 2]
    ldp     x6, x7,   [sp, #16 * 3]
    ldp     x8, x9,   [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30,      [sp, #16 * 15]
    add     sp, sp, #A64_TF_SIZE
    eret

/*
 * タスクの切り替え（プリエンプション）
 *   トラップフレームは割り込まれたタスクのスタックにある。その下にディスパッチャのフレームを作り、
 *   PC = a64_trap_resume、SP = トラップフレーム、SPSR = 割込み禁止の EL1h にして ctxtsk の ssp に残す。
 */
.Lpreempt:
    mov     x0, sp                          /* トラップフレーム */
    sub     x1, x0, #TK2_CTX_SIZE           /* ディスパッチャのフレーム */
    adr     x2, a64_trap_resume
    str     x2, [x1, #TK2_CTX_PC]
    str     x2, [x1, #TK2_CTX_X30]
    str     x0, [x1, #TK2_CTX_SP]
    mov     x2, #(A64_SPSR_EL1H | A64_SPSR_D | A64_SPSR_A | A64_SPSR_I | A64_SPSR_F)
    str     x2, [x1, #TK2_CTX_SPSR]
    ldr     x4, =ctxtsk
    ldr     x5, [x4]
    add     x6, x5, #TCB_tskctxb
    str     x1, [x6, #CTXB_ssp]
    str     xzr, [x4]
    ldr     x0, =tmp_dispatch_stack + 4096
    mov     sp, x0
    mov     x0, x5
    bl      dispatch_saved_ctxtsk           /* 実行可能キューに残っているか確かめる */
    b       dispatch_loop_entry

/* プリエンプトされたタスクの再開点（ディスパッチャが eret で来る。sp = トラップフレーム） */
.global a64_trap_resume
a64_trap_resume:
    msr     daifset, #2
    b       .Lrestore_frame

/* 同期例外（カーネル内の誤り）: 内容を表示して止まる。1 が返れば（ELR を進めて）戻る */
.Lel1_sync:
    SAVE_FRAME
    mov     x0, sp
    bl      kernel_sync_exception_handler
    cbnz    x0, .Lrestore_frame
1:  wfi
    b       1b

/* EL0 からの割込み: EL1 のときと同じ出口（フレームはこのタスクのカーネルスタックにある） */
.Lel0_irq:
    SAVE_FRAME_EL0
    bl      irq_handler
    mov     x0, sp
    bl      rv64_irq_should_preempt
    cbnz    x0, .Lpreempt
    b       .Lrestore_frame

/*
 * EL0 からの同期例外
 *   svc（EC = 0x15）: システムコール。呼んだタスクの文脈で、割込みを許可して rv_user_syscall を呼ぶ
 *                     （眠る・待つシステムコールは T-Kernel の通常のディスパッチで切り替わる）。
 *                     ELR はすでに svc の次を指している
 *   それ以外: rv_user_trap がプログラムを終わらせる（2 が返る）
 *   0 ならフレームから EL0 へ戻る。0 以外ならプログラムの終了（終了コードはフレームの x0）
 */
.Lel0_sync:
    SAVE_FRAME_EL0
    mrs     x0, esr_el1
    lsr     x0, x0, #26
    cmp     x0, #0x15
    b.ne    .Lel0_fault
    mov     x19, sp
    msr     daifclr, #2
    mov     x0, sp
    bl      rv_user_syscall
    msr     daifset, #2
    mov     sp, x19
    cbz     x0, .Lrestore_frame
    b       .Luser_exit
.Lel0_fault:
    mov     x0, sp
    bl      rv_user_trap
.Luser_exit:
    /* ユーザープログラムの終了 → rv_run_user の呼び出し元へ。フレームの真上（kstop）に KCTX がある */
    ldr     x0, [sp, #16 * 0]
    add     sp, sp, #A64_TF_SIZE
    b       rv_user_return

.Lbad:
    SAVE_FRAME
    mov     x0, sp
    bl      kernel_bad_exception_handler
2:  wfi
    b       2b
