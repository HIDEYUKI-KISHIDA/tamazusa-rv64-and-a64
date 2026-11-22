/*
 * mmu_arch.h（RISC-V Sv39 版）— ユーザー空間のページ表の CPU に依る部分
 *
 *   uspace.c はこのヘッダの名前だけでページ表を扱う。AArch64 版は cpu/aarch64/mmu_arch.h。
 *   uspace.c は <mmu_arch.h> と書いて読む（-I の順で、ビルドする CPU のものが選ばれる）。
 */
#ifndef MMU_ARCH_H
#define MMU_ARCH_H

#include <stdint.h>

typedef uint64_t pte_t;

#define PTE_V   (1UL << 0)              /* 有効（AArch64 も同じ 0 ビット目） */
#define PTE_R   (1UL << 1)
#define PTE_W   (1UL << 2)
#define PTE_X   (1UL << 3)
#define PTE_U   (1UL << 4)
#define PTE_A   (1UL << 6)
#define PTE_D   (1UL << 7)
#define PTE_LEAF_MASK   (PTE_R | PTE_W | PTE_X)
#define PTE_SHARED_BIT  (1UL << 8)      /* ソフトウェアの印（RSW）: 共有のページ */

#define PA_TO_PTE(pa)   ((((uintptr_t)(pa)) >> 12) << 10)
#define PTE_TO_PA(pte)  ((((uint64_t)(pte)) >> 10) << 12)
#define VPN2(va)        ((((uintptr_t)(va)) >> 30) & 0x1ff)
#define VPN1(va)        ((((uintptr_t)(va)) >> 21) & 0x1ff)
#define VPN0(va)        ((((uintptr_t)(va)) >> 12) & 0x1ff)

/* ユーザーのページの権限（uspace.c が組み立てる） */
#define UP_R        PTE_R
#define UP_W        PTE_W
#define UP_X        PTE_X
#define UP_SHARED   PTE_SHARED_BIT

#define MK_TABLE(next)          (PA_TO_PTE(next) | PTE_V)
#define MK_UPTE(pa, perm)       (PA_TO_PTE(pa) | (perm) | PTE_U | PTE_A | PTE_D | PTE_V)
#define PTE_IS_LEAF(e, level)   (((e) & PTE_LEAF_MASK) != 0)
#define PTE_ADD_PERM(e, perm)   ((e) | (perm))
#define PTE_WITH_PA(e, pa)      (PA_TO_PTE(pa) | ((e) & 0x3ff))     /* 権限などの下位 10 ビットはそのまま */
#define PTE_UR(e)               (((e) & PTE_U) && ((e) & PTE_R))
#define PTE_UW(e)               (((e) & PTE_U) && ((e) & PTE_W))
#define PTE_UX(e)               (((e) & PTE_U) && ((e) & PTE_X))
#define PTE_IS_SHARED(e)        (((e) & PTE_SHARED_BIT) != 0)

/* 空間（satp の値）とページ表の最上段 */
#define SATP_MODE_SV39          (8UL << 60)
#define SATP_PPN_MASK           ((1UL << 44) - 1)
#define SPACE_FROM_ROOT(root)   (SATP_MODE_SV39 | (((uintptr_t)(root)) >> 12))
#define ROOT_FROM_SPACE(v)      ((pte_t *)(uintptr_t)((((uint64_t)(v)) & SATP_PPN_MASK) << 12))

static inline uint64_t arch_cur_space(void)
{
    uint64_t v;
    asm volatile("csrr %0, satp" : "=r"(v));
    return v;
}
static inline void arch_tlb_flush(void)    { asm volatile("sfence.vma zero, zero" ::: "memory"); }
static inline void arch_icache_sync(void)  { asm volatile("fence.i" ::: "memory"); }
static inline uint64_t arch_rdtime(void)
{
    uint64_t t;
    asm volatile("rdtime %0" : "=r"(t));
    return t;
}

#define EM_ARCH     243                 /* EM_RISCV */

/* ユーザー空間のアドレス（0〜2GiB。ページ表の最上段の 0 番と 1 番） */
#define UVA_SEG_LO      0x00001000UL
#define UVA_SEG_END     0x60000000UL
#define UVA_MMAP_LO     0x60000000UL
#define UVA_MMAP_END    0x7F000000UL
#define UVA_STACK_TOP   0x7FFFF000UL
#define UVA_SIGTRAMP    0x7FFFF000UL
#define UVA_VPN2_COUNT  2

/* シグナルの処理関数から戻る小さなコード: li a7, 139（rt_sigreturn）; ecall */
#define ARCH_SIGTRAMP0  0x08b00893u
#define ARCH_SIGTRAMP1  0x00000073u

#endif
