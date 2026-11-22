/* crt0.S — ユーザープログラムの入口
 *   カーネルが sret で _start へ飛ぶ（a0 = 引数、sp = スタック上端）
 *   main(arg) の戻り値で SYS_EXIT する
 */
.section .text.start, "ax"
.globl _start
_start:
.option push
.option norelax
    la    gp, __global_pointer$
.option pop
    call  main
    li    a7, 2         /* SYS_EXIT（a0 = main の戻り値） */
    ecall
1:  j     1b
