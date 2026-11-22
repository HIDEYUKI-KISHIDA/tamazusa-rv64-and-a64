/* mmu_enable.S — SCTLR.M 有効化のみ（コンパイラ並べ替え防止） */

    .section .text, "ax", %progbits
    .global mmu_sctlr_enable_m
    .type   mmu_sctlr_enable_m, %function

mmu_sctlr_enable_m:
    mrs     x0, sctlr_el1
    orr     x0, x0, #1
    msr     sctlr_el1, x0
    isb
    ret
