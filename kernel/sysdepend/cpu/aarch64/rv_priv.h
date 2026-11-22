/*
 * rv_priv.h（AArch64 版）— CPU に依る定義
 *
 *   共通のソース（装置のドライバ・Linux 互換層など）が "rv_priv.h" を読むので、同じ名前で AArch64 の値を置く。
 *   RISC-V 版は cpu/rv64/rv_priv.h。
 */
#ifndef RV_PRIV_H
#define RV_PRIV_H

#define RV_STR_(x)  #x
#define RV_STR(x)   RV_STR_(x)

#define RV_MODE_NAME    "EL1"

/*
 * 装置（MMIO）のアドレス
 *   MMU を有効にしたあと、装置は高いアドレス（TTBR1 の 0xFFFF_FFC0_0000_0000 + 物理）から触る。
 *   低いアドレスはユーザープログラムに空ける。MMU を有効にする前は物理アドレスのまま（rv_io_offset = 0）。
 */
#ifndef __ASSEMBLER__
#include <stdint.h>
extern uintptr_t rv_io_offset;
#define RV_IO(pa)       ((uintptr_t)(pa) + rv_io_offset)
#endif
#define RV_IO_HIGH_BASE 0xFFFFFFC000000000UL

/*
 * トラップフレーム（exception.S、uint64_t の番号）
 *   [0]〜[30] = x0〜x30、[31] = ELR（戻り先）、[32] = SPSR、[33] = 元の sp（EL0 なら SP_EL0）、
 *   [34] = ESR（原因）、[35] = FAR（アドレス）
 */
#define TF_PC       31
#define TF_STATUS   32
#define TF_SP       33
#define TF_CAUSE    34
#define TF_TVAL     35
#define TF_A0       0           /* 引数・戻り値 x0〜x5 */
#define TF_NR       8           /* システムコールの番号 x8（Linux と同じ） */

/* ユーザー空間（ネイティブの試験のプログラム、M9〜M11） */
#define USER_VA_BASE        0x00400000UL

#define TF_TLS      36          /* TPIDR_EL0 */
#define TF_WORDS    37          /* fork で写す語数 */
#define TF_RA       30          /* x30（戻り先） */
/* 10MHz 単位の時刻（RISC-V 版の rdtime と同じ刻み。Linux 層が使う） */
#ifndef __ASSEMBLER__
/* 割込みの禁止（戻り値 = DAIF を RV_IRQ_RESTORE へ） */
static inline unsigned long RV_IRQ_SAVE(void)
{
    unsigned long st;
    __asm__ volatile("mrs %0, daif\n msr daifset, #2" : "=r"(st) :: "memory");
    return st;
}
static inline void RV_IRQ_RESTORE(unsigned long st)
{
    __asm__ volatile("msr daif, %0" :: "r"(st) : "memory");
}
#endif
#ifndef __ASSEMBLER__
static inline unsigned long RV_RDTIME(void)
{
    unsigned long c, f;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(c));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return (c / f) * 10000000u + ((c % f) * 10000000u) / f;   /* 128 ビットの割り算を使わない */
}
#endif

#endif /* RV_PRIV_H */
