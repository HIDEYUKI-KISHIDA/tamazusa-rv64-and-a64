/*
 * user_mode.S — 構成 B-2: U-mode（ユーザーモード）への出入り（S-mode + MMU のみ）
 *
 *   long rv_run_user(uintptr_t entry, uintptr_t user_sp, long arg)
 *   long rv_run_user2(uintptr_t entry, uintptr_t user_sp, long arg, long arg2)
 *       rv_run_user2 はユーザーの a1 に arg2 を渡す（文字列の引数のアドレスなど）
 *       呼んだタスク自身のスタックにカーネルのレジスタ（ra, s0〜s11, sstatus）を積み、
 *       その位置を「このタスクのカーネルスタック上端」として sscratch に入れてから、
 *       sret で U-mode の entry へ飛ぶ（a0 = arg、sp = user_sp）。
 *       ユーザープログラムが SYS_EXIT を呼ぶか、例外で強制終了されると、
 *       rv_user_return から「rv_run_user の戻り値」として終了コードが返る。
 *
 * 第3段: タスクごとのカーネルスタック
 *   U-mode からのトラップは、そのタスクのカーネルスタック（積んだレジスタの真下）に
 *   フレームを作る。共有スタックを使わないので、複数のタスクが同時に U-mode の
 *   プログラムを持てる（割込み出口での切替・システムコール中の待ちができる）。
 *
 *   タスクのスタック（上が高位アドレス）
 *     ... rv_run_user の呼び出し元 ...
 *     [KCTX 128B]  ra, s0〜s11, sstatus   ← kstop（U-mode 中の sscratch）
 *     [トラップフレーム 288B]              ← U-mode からのトラップ時
 *     ...
 */
#include "rv_priv.h"

#define KCTX_RA       0
#define KCTX_S0       8
#define KCTX_STATUS 104
#define KCTX_SIZE   128

.section .text
.globl rv_run_user
.globl rv_run_user2
.globl rv_run_user_frame
.globl rv_user_return

rv_run_user:
    li    a3, 0                     /* a1 には 0 を渡す */
rv_run_user2:
    addi  sp, sp, -KCTX_SIZE
    sd    ra,   KCTX_RA(sp)
    sd    s0,   KCTX_S0+0*8(sp)
    sd    s1,   KCTX_S0+1*8(sp)
    sd    s2,   KCTX_S0+2*8(sp)
    sd    s3,   KCTX_S0+3*8(sp)
    sd    s4,   KCTX_S0+4*8(sp)
    sd    s5,   KCTX_S0+5*8(sp)
    sd    s6,   KCTX_S0+6*8(sp)
    sd    s7,   KCTX_S0+7*8(sp)
    sd    s8,   KCTX_S0+8*8(sp)
    sd    s9,   KCTX_S0+9*8(sp)
    sd    s10,  KCTX_S0+10*8(sp)
    sd    s11,  KCTX_S0+11*8(sp)
    csrr  t1, sstatus
    sd    t1,   KCTX_STATUS(sp)

    csrci sstatus, XSTATUS_IE       /* sret まで割込み禁止 */
    csrw  sscratch, sp              /* U-mode からのトラップはここから下へ */

    csrw  sepc, a0                  /* ユーザーの入口 */
    li    t1, XSTATUS_PP            /* SPP = 0 → sret で U-mode へ */
    csrc  sstatus, t1
    li    t1, XSTATUS_PIE           /* SPIE = 1 → U-mode では割込み有効 */
    csrs  sstatus, t1

    mv    sp, a1
    mv    a0, a2
    mv    a1, a3
    /* カーネルの値をユーザーに見せない */
    li    ra, 0
    li    gp, 0
    li    tp, 0
    li    t0, 0
    li    t1, 0
    li    t2, 0
    li    s0, 0
    li    s1, 0
    li    a2, 0
    li    a3, 0
    sret

/*
 * long rv_run_user_frame(const uint64_t *frame)
 *   fork の子: トラップフレーム（288B、f[r-1] = x_r、f[31] = 戻り先）の
 *   レジスタをすべてそのまま入れて U-mode へ戻る（a0 も f のまま。呼ぶ側が 0 にしておく）。
 *   カーネルのレジスタの退避と、終わったときの戻り方は rv_run_user と同じ。
 */
.globl rv_run_user_frame
rv_run_user_frame:
    addi  sp, sp, -KCTX_SIZE
    sd    ra,   KCTX_RA(sp)
    sd    s0,   KCTX_S0+0*8(sp)
    sd    s1,   KCTX_S0+1*8(sp)
    sd    s2,   KCTX_S0+2*8(sp)
    sd    s3,   KCTX_S0+3*8(sp)
    sd    s4,   KCTX_S0+4*8(sp)
    sd    s5,   KCTX_S0+5*8(sp)
    sd    s6,   KCTX_S0+6*8(sp)
    sd    s7,   KCTX_S0+7*8(sp)
    sd    s8,   KCTX_S0+8*8(sp)
    sd    s9,   KCTX_S0+9*8(sp)
    sd    s10,  KCTX_S0+10*8(sp)
    sd    s11,  KCTX_S0+11*8(sp)
    csrr  t1, sstatus
    sd    t1,   KCTX_STATUS(sp)

    csrci sstatus, XSTATUS_IE
    csrw  sscratch, sp

    ld    t1, 31*8(a0)
    csrw  sepc, t1
    li    t1, XSTATUS_PP
    csrc  sstatus, t1
    li    t1, XSTATUS_PIE
    csrs  sstatus, t1

    mv    t0, a0                    /* t0 = フレーム（x5 は最後に入れる） */
    ld    x1,  0*8(t0)
    ld    x2,  1*8(t0)
    ld    x3,  2*8(t0)
    ld    x4,  3*8(t0)
    ld    x6,  5*8(t0)
    ld    x7,  6*8(t0)
    ld    x8,  7*8(t0)
    ld    x9,  8*8(t0)
    ld    x10, 9*8(t0)
    ld    x11, 10*8(t0)
    ld    x12, 11*8(t0)
    ld    x13, 12*8(t0)
    ld    x14, 13*8(t0)
    ld    x15, 14*8(t0)
    ld    x16, 15*8(t0)
    ld    x17, 16*8(t0)
    ld    x18, 17*8(t0)
    ld    x19, 18*8(t0)
    ld    x20, 19*8(t0)
    ld    x21, 20*8(t0)
    ld    x22, 21*8(t0)
    ld    x23, 22*8(t0)
    ld    x24, 23*8(t0)
    ld    x25, 24*8(t0)
    ld    x26, 25*8(t0)
    ld    x27, 26*8(t0)
    ld    x28, 27*8(t0)
    ld    x29, 28*8(t0)
    ld    x30, 29*8(t0)
    ld    x31, 30*8(t0)
    ld    x5,  4*8(t0)
    sret

/*
 * exception.S から（割込み禁止で）飛んでくる。
 *   sp = kstop（積んだ KCTX の先頭）、a0 = 終了コード
 */
rv_user_return:
    ld    ra,   KCTX_RA(sp)
    ld    s0,   KCTX_S0+0*8(sp)
    ld    s1,   KCTX_S0+1*8(sp)
    ld    s2,   KCTX_S0+2*8(sp)
    ld    s3,   KCTX_S0+3*8(sp)
    ld    s4,   KCTX_S0+4*8(sp)
    ld    s5,   KCTX_S0+5*8(sp)
    ld    s6,   KCTX_S0+6*8(sp)
    ld    s7,   KCTX_S0+7*8(sp)
    ld    s8,   KCTX_S0+8*8(sp)
    ld    s9,   KCTX_S0+9*8(sp)
    ld    s10,  KCTX_S0+10*8(sp)
    ld    s11,  KCTX_S0+11*8(sp)
.option push
.option norelax
    la    gp, __global_pointer$
.option pop
    ld    t1,   KCTX_STATUS(sp)
    addi  sp, sp, KCTX_SIZE
    andi  t1, t1, XSTATUS_IE        /* 呼び出し前の割込み許可状態に戻す */
    csrs  sstatus, t1
    ret
