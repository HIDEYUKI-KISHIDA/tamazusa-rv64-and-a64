/*
 * user_prog.S — U-mode で動かすテスト用ユーザープログラム（M9）
 *
 * .user.text（コード 1 ページ）と .user.data（データ・スタック 1 ページ）は
 * リンカで物理的に連続して置かれ、MMU で VA 0x4000_0000 からの 2 ページに
 * U ビット付きで写される。PC 相対（la）でデータを参照するので、
 * 物理と仮想で 2 ページの相対位置が同じであることが前提。
 *
 *   a0 = 0: SYS_PUTS で挨拶を出し、SYS_EXIT(42)
 *   a0 = 1: カーネルのメモリ（0x8020_0000）を読もうとする → ロードページフォールト
 *   a0 = 2: 特権命令（csrr sstatus）を実行しようとする → 不正命令例外
 *   a0 = 3: 約 2 億回の空ループ（タイマ割込みが U-mode に入って戻ることの確認）→ SYS_EXIT(7)
 *   a0 = 4 | (id << 8): 3 回 { SYS_PUTS; SYS_SLEEP(30) } → SYS_EXIT(id)（M10）
 *   a0 = 5 | (v << 8): user_var に v を書き、2 回眠ってから読み戻す。
 *                      変わっていなければ SYS_EXIT(v)、書き換えられていれば SYS_EXIT(0)（M11）
 */
#define SYS_PUTS   1
#define SYS_EXIT   2
#define SYS_SLEEP  3

.section .user.text, "ax"
.globl rv_user_prog_entry
rv_user_prog_entry:
    beqz  a0, .Lhello
    li    t0, 1
    beq   a0, t0, .Lpeek_kernel
    li    t0, 2
    beq   a0, t0, .Lpriv_insn
    li    t0, 3
    beq   a0, t0, .Lspin
    andi  t1, a0, 0xff
    li    t0, 4
    beq   t1, t0, .Lsleeper
    li    t0, 5
    beq   t1, t0, .Lprivate
    li    a0, 99
    li    a7, SYS_EXIT
    ecall

.Lhello:
    la    a0, user_msg
    li    a7, SYS_PUTS
    ecall
    li    a0, 42
    li    a7, SYS_EXIT
    ecall
1:  j     1b

.Lpeek_kernel:
    li    t0, 0x80200000
    ld    t1, 0(t0)                 /* ここで例外になるはず */
    li    a0, 0                     /* 読めてしまったら 0 で終了 */
    li    a7, SYS_EXIT
    ecall

.Lpriv_insn:
    csrr  t1, sstatus               /* U-mode では不正命令 */
    li    a0, 0
    li    a7, SYS_EXIT
    ecall

.Lspin:
    li    t0, 200000000
    li    t1, 0
2:  addi  t1, t1, 1
    blt   t1, t0, 2b
    bne   t1, t0, 3f                /* レジスタが壊れていれば 0 で終了 */
    li    a0, 7
    li    a7, SYS_EXIT
    ecall
3:  li    a0, 0
    li    a7, SYS_EXIT
    ecall

.Lsleeper:
    srli  s1, a0, 8                 /* s1 = id（システムコールをまたいで保たれること） */
    li    s2, 3
4:  la    a0, user_tick_msg
    li    a7, SYS_PUTS
    ecall
    li    a0, 30
    li    a7, SYS_SLEEP
    ecall
    addi  s2, s2, -1
    bnez  s2, 4b
    mv    a0, s1
    li    a7, SYS_EXIT
    ecall

.Lprivate:
    srli  s1, a0, 8
    la    t0, user_var
    sd    s1, 0(t0)
    la    a0, user_priv_msg
    li    a7, SYS_PUTS
    ecall
    li    a0, 30
    li    a7, SYS_SLEEP
    ecall
    li    a0, 30
    li    a7, SYS_SLEEP
    ecall
    la    t0, user_var
    ld    t1, 0(t0)
    li    a0, 0
    bne   t1, s1, 5f
    mv    a0, s1
5:  li    a7, SYS_EXIT
    ecall

.section .user.data, "aw"
.balign 8
.globl user_var
user_var:
    .dword 0
user_priv_msg:
    .asciz "[user] wrote my value to 0x4000_1000\r\n"
user_tick_msg:
    .asciz "[user] tick\r\n"
user_msg:
    .asciz "[user] hello from U-mode (via ecall)\r\n"
