/*
 * rv_priv.h — 特権モード抽象化（構成 A: M-mode / 構成 B: S-mode）
 *
 *   構成 A（既定）  : M-mode、-bios none、0x80000000
 *   構成 B（SMODE=1）: S-mode、OpenSBI (-bios default)、0x80200000
 *
 * アセンブリ (.S) と C の両方から include できる。
 * C のインラインアセンブリでは RV_STR(CSR_XSTATUS) のように文字列化して使う。
 */
#ifndef RV_PRIV_H
#define RV_PRIV_H

#define RV_STR_(x)  #x
#define RV_STR(x)   RV_STR_(x)

#ifdef TK2_SMODE
/* ---- 構成 B: S-mode ---- */
#define CSR_XSTATUS     sstatus
#define CSR_XEPC        sepc
#define CSR_XTVEC       stvec
#define CSR_XSCRATCH    sscratch
#define CSR_XCAUSE      scause
#define CSR_XTVAL       stval
#define CSR_XIE         sie
#define XRET            sret

#define XSTATUS_IE      (1 << 1)        /* SIE  */
#define XSTATUS_PIE     (1 << 5)        /* SPIE */
#define XSTATUS_PP      (1 << 8)        /* SPP=S */
#define XIE_TIMER       (1 << 5)        /* STIE */
#define XCAUSE_TIMER    5               /* supervisor timer interrupt */

/* xret 前に書く値: PP|PIE のみ（IE は xret が PIE から立てる） */
#define XSTATUS_RUN     (XSTATUS_PP | XSTATUS_PIE)

#define RV_MODE_NAME    "S-mode"

#else
/* ---- 構成 A: M-mode（v0.1.0 と同一動作） ---- */
#define CSR_XSTATUS     mstatus
#define CSR_XEPC        mepc
#define CSR_XTVEC       mtvec
#define CSR_XSCRATCH    mscratch
#define CSR_XCAUSE      mcause
#define CSR_XTVAL       mtval
#define CSR_XIE         mie
#define XRET            mret

#define XSTATUS_IE      (1 << 3)        /* MIE  */
#define XSTATUS_PIE     (1 << 7)        /* MPIE */
#define XSTATUS_PP      (3 << 11)       /* MPP=M */
#define XIE_TIMER       (1 << 7)        /* MTIE */
#define XCAUSE_TIMER    7               /* machine timer interrupt */

/* v0.1.0 の値をそのまま維持（MPP|MPIE|MIE） */
#define XSTATUS_RUN     (XSTATUS_PP | XSTATUS_PIE | XSTATUS_IE)

#define RV_MODE_NAME    "M-mode"
#endif

/* 浮動小数点ユニットを使える状態にする（xSTATUS.FS = Initial）。M/S 共通の位置 */
#define XSTATUS_FS_INITIAL  (1 << 13)
/* ベクトルユニットを使える状態にする（xSTATUS.VS = Initial）。V 拡張のない CPU では 0 のまま */
#define XSTATUS_VS_INITIAL  (1 << 9)

/*
 * 装置（MMIO）のアドレス
 *   MMU=1 では、ページングを有効にしたあと、装置を高いアドレス（0xFFFF_FFC0_0000_0000 + 物理）
 *   から触る。低いアドレス（0〜2GiB）はユーザープログラム（Linux 用の ELF は 0x10000 から）に空ける。
 *   ページングを有効にする前と MMU=0 では、物理アドレスのまま（rv_io_offset = 0）。
 */
#ifndef __ASSEMBLER__
#include <stdint.h>
extern uintptr_t rv_io_offset;
#define RV_IO(pa)       ((uintptr_t)(pa) + rv_io_offset)
#endif
#define RV_IO_HIGH_BASE 0xFFFFFFC000000000UL

/* trap frame indices (shared Linux layer; aarch64 has its own values) */
#define TF_SP      1
#define TF_A0      9
#define TF_NR      16
#define TF_PC      31
#define TF_STATUS  32
#define TF_CAUSE   33
#define TF_TVAL    34

#define TF_WORDS   35         /* fork で写す語数 */
#define TF_RA      0
/* time counter in 10MHz units (QEMU virt's rdtime rate) */
#ifndef __ASSEMBLER__
/* 割込みの禁止（戻り値を RV_IRQ_RESTORE へ） */
static inline unsigned long RV_IRQ_SAVE(void)
{
    unsigned long st;
    __asm__ volatile("csrrc %0, " RV_STR(CSR_XSTATUS) ", %1" : "=r"(st) : "r"((unsigned long)XSTATUS_IE) : "memory");
    return st;
}
static inline void RV_IRQ_RESTORE(unsigned long st)
{
    __asm__ volatile("csrs " RV_STR(CSR_XSTATUS) ", %0" : : "r"(st & XSTATUS_IE) : "memory");
}
#endif
#ifndef __ASSEMBLER__
static inline unsigned long RV_RDTIME(void)
{
    unsigned long t;
    __asm__ volatile("rdtime %0" : "=r"(t));
    return t;
}
#endif

#endif /* RV_PRIV_H */
