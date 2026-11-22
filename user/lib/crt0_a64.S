/* crt0_a64.S — ユーザープログラムの入口（AArch64 版。RISC-V 版は crt0.S）
 *   カーネルが eret で _start へ飛ぶ（x0 = 引数、x1 = 文字列の引数、sp = スタックの上端）
 *   main(arg) の戻り値で SYS_EXIT する
 */
.section .text.start, "ax"
.globl _start
_start:
    bl    main
    mov   x8, #2        /* SYS_EXIT（x0 = main の戻り値） */
    svc   #0
1:  b     1b
