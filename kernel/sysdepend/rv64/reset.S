/* reset.S — QEMU virt RV64 TK2 entry
 *
 *   構成 A: M-mode。QEMU -bios none で 0x80000000 から直接起動。
 *   構成 B: S-mode。OpenSBI が a0=hartid, a1=DTB 物理アドレスで
 *           0x80200000 へ sret してくる。
 */
#include "rv_priv.h"

.section .text.boot
.globl _start

_start:
    bnez a0, .Lhalt                 /* hart 0 以外は停止（SMP は未対応） */
    csrci CSR_XSTATUS, XSTATUS_IE

    la   t2, __global_pointer$
    mv   gp, t2
    la   sp, knl_kernel_stack_top

#ifdef TK2_SMODE
    mv   s1, a1                     /* DTB アドレスを BSS クリア前に退避 */
#endif

    la   t0, __bss_start
    la   t1, __bss_end
.Lbss:
    bgeu t0, t1, .Lbss_done
    sd   zero, 0(t0)
    addi t0, t0, 8
    j    .Lbss
.Lbss_done:

#ifdef TK2_SMODE
    la   t0, rv_boot_dtb
    sd   s1, 0(t0)
#endif

    la   t0, trap_entry
    andi t0, t0, ~3                 /* Direct モード（下位2ビット=0） */
    csrw CSR_XTVEC, t0
    la   t1, knl_kernel_stack_top
    csrw CSR_XSCRATCH, t1

    call board_init
    call knl_main

.Lhalt:
    wfi
    j    .Lhalt

#ifdef TK2_SMODE
.section .data
.align 3
.globl rv_boot_dtb
rv_boot_dtb:
    .dword 0                        /* 将来のデバイスツリー解析用 */
#endif
