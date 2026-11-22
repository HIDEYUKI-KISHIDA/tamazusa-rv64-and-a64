/*
 * mmu_arch.h（AArch64 版）— ユーザー空間のページ表の CPU に依る部分
 *
 *   4KiB の粒度・仮想アドレス 39 ビット・3 段で、表の形は Sv39 と同じ（512 個ずつ）。記述子の形だけが違う。
 *     L1・L2 の表: 下位 2 ビット = 11、ブロック = 01。L3 のページ = 11
 *     権限: AP[1]（EL0 から）、AP[2]（読み取り専用）、UXN（EL0 で実行しない）、PXN（EL1 で実行しない）
 */
#ifndef MMU_ARCH_H
#define MMU_ARCH_H

#include <stdint.h>

typedef uint64_t pte_t;

#define PTE_V           (1UL << 0)
#define D_TYPE_TABLE    (3UL << 0)
#define D_ATTR_NORMAL   (0UL << 2)      /* MAIR の 0 番 */
#define D_AP_EL0        (1UL << 6)
#define D_AP_RO         (1UL << 7)
#define D_SH_INNER      (3UL << 8)
#define D_AF            (1UL << 10)
#define D_NG            (1UL << 11)
#define D_PXN           (1UL << 53)
#define D_UXN           (1UL << 54)
#define D_SW_SHARED     (1UL << 55)     /* ソフトウェアの印: 共有のページ */
#define D_PA_MASK       0x0000FFFFFFFFF000UL

#define PA_TO_PTE(pa)   (((uintptr_t)(pa)) & D_PA_MASK)
#define PTE_TO_PA(pte)  (((uint64_t)(pte)) & D_PA_MASK)
#define VPN2(va)        ((((uintptr_t)(va)) >> 30) & 0x1ff)
#define VPN1(va)        ((((uintptr_t)(va)) >> 21) & 0x1ff)
#define VPN0(va)        ((((uintptr_t)(va)) >> 12) & 0x1ff)

/* ユーザーのページの権限（uspace.c が組み立てる。記述子のビットではない） */
#define UP_R        (1UL << 0)
#define UP_W        (1UL << 1)
#define UP_X        (1UL << 2)
#define UP_SHARED   (1UL << 3)

static inline pte_t mk_upte(uintptr_t pa, uint64_t perm)
{
    return PA_TO_PTE(pa) | D_TYPE_TABLE | D_ATTR_NORMAL | D_SH_INNER | D_AF | D_NG | D_AP_EL0 |
           ((perm & UP_W) ? 0 : D_AP_RO) | D_PXN | ((perm & UP_X) ? 0 : D_UXN) |
           ((perm & UP_SHARED) ? D_SW_SHARED : 0);
}
static inline uint64_t pte_perm(pte_t e)
{
    uint64_t p = 0;
    if (e & D_AP_EL0) p |= UP_R;
    if ((e & D_AP_EL0) && !(e & D_AP_RO)) p |= UP_W;
    if (!(e & D_UXN)) p |= UP_X;
    if (e & D_SW_SHARED) p |= UP_SHARED;
    return p;
}

#define MK_TABLE(next)          (PA_TO_PTE(next) | D_TYPE_TABLE)
#define MK_UPTE(pa, perm)       mk_upte((uintptr_t)(pa), (perm))
/* L1・L2（level 2・1）では下位 2 ビットが 01 ならブロック（葉）。L3（level 0）は常に葉 */
#define PTE_IS_LEAF(e, level)   ((level) == 0 || (((e) & 2) == 0))
#define PTE_ADD_PERM(e, perm)   mk_upte(PTE_TO_PA(e), pte_perm(e) | (perm))
#define PTE_WITH_PA(e, pa)      (((e) & ~D_PA_MASK) | PA_TO_PTE(pa))
#define PTE_UR(e)               (((e) & PTE_V) && ((e) & D_AP_EL0))
#define PTE_UW(e)               (((e) & PTE_V) && ((e) & D_AP_EL0) && !((e) & D_AP_RO))
#define PTE_UX(e)               (((e) & PTE_V) && ((e) & D_AP_EL0) && !((e) & D_UXN))
#define PTE_IS_SHARED(e)        (((e) & D_SW_SHARED) != 0)

/* 空間（TTBR0 の値）とページ表の最上段（ASID は使わない） */
#define SPACE_FROM_ROOT(root)   ((uint64_t)(uintptr_t)(root))
#define ROOT_FROM_SPACE(v)      ((pte_t *)(uintptr_t)(((uint64_t)(v)) & D_PA_MASK))

static inline uint64_t arch_cur_space(void)
{
    uint64_t v;
    asm volatile("mrs %0, ttbr0_el1" : "=r"(v));
    return v;
}
static inline void arch_tlb_flush(void)
{
    asm volatile("dsb ishst\n\ttlbi vmalle1\n\tdsb ish\n\tisb" ::: "memory");
}
static inline void arch_icache_sync(void)
{
    asm volatile("dsb ish\n\tic iallu\n\tdsb ish\n\tisb" ::: "memory");
}
static inline uint64_t arch_rdtime(void)
{
    uint64_t t;
    asm volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(t));
    return t;
}

#define EM_ARCH     183                 /* EM_AARCH64 */

/* ユーザー空間のアドレス（0〜1GiB。ページ表の最上段の 0 番。1 番からは RAM の恒等写像） */
#define UVA_SEG_LO      0x00001000UL
#define UVA_SEG_END     0x20000000UL
#define UVA_MMAP_LO     0x20000000UL
#define UVA_MMAP_END    0x3F000000UL
#define UVA_STACK_TOP   0x3FFFF000UL
#define UVA_SIGTRAMP    0x3FFFF000UL
#define UVA_VPN2_COUNT  1

/* シグナルの処理関数から戻る小さなコード: mov x8, #139（rt_sigreturn）; svc #0 */
#define ARCH_SIGTRAMP0  0xd2801168u
#define ARCH_SIGTRAMP1  0xd4000001u

#endif
