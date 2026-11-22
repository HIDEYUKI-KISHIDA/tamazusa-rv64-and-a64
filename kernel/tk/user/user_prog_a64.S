/*
 * user_prog_a64.S — EL0（ユーザーモード）で動かす試験のプログラム（M9〜M11、AArch64 版）
 *   RISC-V 版 user_prog.S と同じ動き。システムコールは svc #0、番号は x8、引数と戻り値は x0。
 *
 *   x0 = 0: SYS_PUTS で挨拶を出し、SYS_EXIT(42)
 *   x0 = 1: カーネルのメモリ（0x4020_0000）を読もうとする → データアボート
 *   x0 = 2: 特権命令（mrs x1, sctlr_el1）を実行しようとする → 未定義命令
 *   x0 = 3: 約 2 億回の空ループ（タイマ割込みが EL0 に入って戻ることの確認）→ SYS_EXIT(7)
 *   x0 = 4 | (id << 8): 3 回 { SYS_PUTS; SYS_SLEEP(30) } → SYS_EXIT(id)（M10）
 *   x0 = 5 | (v << 8): user_var に v を書き、2 回眠ってから読み戻す（M11）
 */
#define SYS_PUTS   1
#define SYS_EXIT   2
#define SYS_SLEEP  3

.section .user.text, "ax"
.globl rv_user_prog_entry
rv_user_prog_entry:
    cbz   x0, .Lhello
    cmp   x0, #1
    b.eq  .Lpeek_kernel
    cmp   x0, #2
    b.eq  .Lpriv_insn
    cmp   x0, #3
    b.eq  .Lspin
    and   x9, x0, #0xff
    cmp   x9, #4
    b.eq  .Lsleeper
    cmp   x9, #5
    b.eq  .Lprivate
    mov   x0, #99
    mov   x8, #SYS_EXIT
    svc   #0

.Lhello:
    adr   x0, user_msg
    mov   x8, #SYS_PUTS
    svc   #0
    mov   x0, #42
    mov   x8, #SYS_EXIT
    svc   #0
1:  b     1b

.Lpeek_kernel:
    mov   x9, #0x40200000
    ldr   x10, [x9]                 /* ここで例外になるはず */
    mov   x0, #0                    /* 読めてしまったら 0 で終了 */
    mov   x8, #SYS_EXIT
    svc   #0

.Lpriv_insn:
    mrs   x1, sctlr_el1             /* EL0 では未定義命令 */
    mov   x0, #0
    mov   x8, #SYS_EXIT
    svc   #0

.Lspin:
    movz  x9, #(200000000 & 0xffff)
    movk  x9, #(200000000 >> 16), lsl #16
    mov   x10, #0
2:  add   x10, x10, #1
    cmp   x10, x9
    b.lt  2b
    b.ne  3f                        /* レジスタが壊れていれば 0 で終了 */
    mov   x0, #7
    mov   x8, #SYS_EXIT
    svc   #0
3:  mov   x0, #0
    mov   x8, #SYS_EXIT
    svc   #0

.Lsleeper:
    lsr   x19, x0, #8               /* x19 = id（システムコールをまたいで保たれること） */
    mov   x20, #3
4:  adr   x0, user_tick_msg
    mov   x8, #SYS_PUTS
    svc   #0
    mov   x0, #30
    mov   x8, #SYS_SLEEP
    svc   #0
    subs  x20, x20, #1
    b.ne  4b
    mov   x0, x19
    mov   x8, #SYS_EXIT
    svc   #0

.Lprivate:
    lsr   x19, x0, #8
    adr   x9, user_var
    str   x19, [x9]
    adr   x0, user_priv_msg
    mov   x8, #SYS_PUTS
    svc   #0
    mov   x0, #30
    mov   x8, #SYS_SLEEP
    svc   #0
    mov   x0, #30
    mov   x8, #SYS_SLEEP
    svc   #0
    adr   x9, user_var
    ldr   x10, [x9]
    mov   x0, #0
    cmp   x10, x19
    b.ne  5f
    mov   x0, x19
5:  mov   x8, #SYS_EXIT
    svc   #0

.section .user.data, "aw"
.balign 8
.globl user_var
user_var:
    .dword 0
user_priv_msg:
    .asciz "[user] wrote my value to 0x0040_1000\r\n"
user_tick_msg:
    .asciz "[user] tick\r\n"
user_msg:
    .asciz "[user] hello from EL0 (via svc)\r\n"
