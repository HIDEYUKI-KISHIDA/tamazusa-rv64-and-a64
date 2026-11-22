/*
 * user_mode.S — EL0（ユーザーモード）への出入り（AArch64 版。RISC-V 版 cpu/rv64/user_mode.S と同じ形）
 *
 *   long rv_run_user(uintptr_t entry, uintptr_t user_sp, long arg)
 *   long rv_run_user2(uintptr_t entry, uintptr_t user_sp, long arg, long arg2)
 *       呼んだタスクのスタックにカーネルのレジスタ（x19〜x30、DAIF）を積み、その位置を
 *       「このタスクのカーネルスタックの上端（kstop）」にして eret で EL0 の entry へ行く（x0 = arg、x1 = arg2）。
 *       EL0 からの例外では、CPU が SP_EL1（= kstop）に切り替えるので、フレームはその真下に積まれる。
 *       プログラムが SYS_EXIT を呼ぶか、例外で終わらされると、rv_user_return から終了コードが返る。
 *   long rv_run_user_frame(const uint64_t *frame)
 *       fork の子: トラップフレームのレジスタをすべてそのまま入れて EL0 へ戻る
 *
 *   タスクのスタック（上が高いアドレス）
 *     ... rv_run_user の呼び出し元 ...
 *     [KCTX 128B]  x19〜x30、DAIF   ← kstop（EL0 にいる間の SP_EL1）
 *     [トラップフレーム 304B]         ← EL0 からの例外のとき
 */
#include "cpu_attr.h"

#define KCTX_X19      0
#define KCTX_X29     80
#define KCTX_DAIF    96
#define KCTX_SIZE   128

.macro SAVE_KCTX
    sub   sp, sp, #KCTX_SIZE
    stp   x19, x20, [sp, #KCTX_X19 + 0]
    stp   x21, x22, [sp, #KCTX_X19 + 16]
    stp   x23, x24, [sp, #KCTX_X19 + 32]
    stp   x25, x26, [sp, #KCTX_X19 + 48]
    stp   x27, x28, [sp, #KCTX_X19 + 64]
    stp   x29, x30, [sp, #KCTX_X29]
    mrs   x9, daif
    str   x9, [sp, #KCTX_DAIF]
    msr   daifset, #2                /* eret まで割込み禁止 */
.endm

.section .text
.globl rv_run_user
.globl rv_run_user2
.globl rv_run_user_frame
.globl rv_user_return

rv_run_user:
    mov   x3, #0                     /* x1 には 0 を渡す */
rv_run_user2:
    SAVE_KCTX
    msr   elr_el1, x0                /* ユーザーの入口 */
    msr   spsr_el1, xzr              /* EL0t、割込み許可 */
    msr   sp_el0, x1
    msr   tpidr_el0, xzr
    mov   x0, x2
    mov   x1, x3
    /* カーネルの値をユーザーに見せない */
    mov   x2, #0
    mov   x3, #0
    mov   x4, #0
    mov   x5, #0
    mov   x6, #0
    mov   x7, #0
    mov   x8, #0
    mov   x9, #0
    mov   x10, #0
    mov   x11, #0
    mov   x12, #0
    mov   x13, #0
    mov   x14, #0
    mov   x15, #0
    mov   x16, #0
    mov   x17, #0
    mov   x18, #0
    mov   x19, #0
    mov   x20, #0
    mov   x21, #0
    mov   x22, #0
    mov   x23, #0
    mov   x24, #0
    mov   x25, #0
    mov   x26, #0
    mov   x27, #0
    mov   x28, #0
    mov   x29, #0
    mov   x30, #0
    eret

rv_run_user_frame:
    SAVE_KCTX
    ldr   x9, [x0, #A64_TF_ELR]
    msr   elr_el1, x9
    ldr   x9, [x0, #A64_TF_SPSR]
    and   x9, x9, #~0xf              /* EL0t に戻す */
    msr   spsr_el1, x9
    ldr   x9, [x0, #A64_TF_SP]
    msr   sp_el0, x9
    ldr   x9, [x0, #A64_TF_TLS]
    msr   tpidr_el0, x9
    mov   x30, x0                    /* x30 = フレーム（最後に入れる） */
    ldp   x0, x1,   [x30, #16 * 0]
    ldp   x2, x3,   [x30, #16 * 1]
    ldp   x4, x5,   [x30, #16 * 2]
    ldp   x6, x7,   [x30, #16 * 3]
    ldp   x8, x9,   [x30, #16 * 4]
    ldp   x10, x11, [x30, #16 * 5]
    ldp   x12, x13, [x30, #16 * 6]
    ldp   x14, x15, [x30, #16 * 7]
    ldp   x16, x17, [x30, #16 * 8]
    ldp   x18, x19, [x30, #16 * 9]
    ldp   x20, x21, [x30, #16 * 10]
    ldp   x22, x23, [x30, #16 * 11]
    ldp   x24, x25, [x30, #16 * 12]
    ldp   x26, x27, [x30, #16 * 13]
    ldp   x28, x29, [x30, #16 * 14]
    ldr   x30,      [x30, #16 * 15]
    eret

/* exception.S から（割込み禁止で）来る。sp = kstop、x0 = 終了コード */
rv_user_return:
    ldp   x19, x20, [sp, #KCTX_X19 + 0]
    ldp   x21, x22, [sp, #KCTX_X19 + 16]
    ldp   x23, x24, [sp, #KCTX_X19 + 32]
    ldp   x25, x26, [sp, #KCTX_X19 + 48]
    ldp   x27, x28, [sp, #KCTX_X19 + 64]
    ldp   x29, x30, [sp, #KCTX_X29]
    ldr   x9, [sp, #KCTX_DAIF]
    add   sp, sp, #KCTX_SIZE
    msr   daif, x9                   /* 呼ぶ前の割込みの禁止・許可に戻す */
    ret
