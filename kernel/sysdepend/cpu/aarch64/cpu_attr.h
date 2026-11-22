#ifndef CPU_ATTR_H
#define CPU_ATTR_H
/* AArch64: ディスパッチャのタスクのフレーム（128 バイト）
 *   呼び出し側が保存するレジスタ x19〜x28、x29（フレームポインタ）、x30（戻り先）、
 *   再開時の sp、再開する場所（ELR）、再開時の状態（SPSR）
 *   RISC-V 版（cpu/rv64/cpu_attr.h）と同じ考え方。共通のコード（m5b_trace.c など）が使う名前もそろえる */

#ifndef __ASSEMBLER__
#include <stdint.h>
#endif

#define TK2_CTX_SIZE          128

#define TK2_CTX_X19             0
#define TK2_CTX_X21            16
#define TK2_CTX_X23            32
#define TK2_CTX_X25            48
#define TK2_CTX_X27            64
#define TK2_CTX_X29            80
#define TK2_CTX_X30            88
#define TK2_CTX_SP             96
#define TK2_CTX_PC            104   /* ELR_EL1 */
#define TK2_CTX_SPSR          112   /* SPSR_EL1 */

/* 共通コードの名前 */
#define TK2_CTX_ELR           TK2_CTX_PC
#define TK2_CTX_SP_KSP        TK2_CTX_SP
#define TK2_CTX_RA            TK2_CTX_X30
#define TK2_CTX_MSTATUS       TK2_CTX_SPSR

/* SPSR: EL1h（SP_EL1 を使う）。D・A は止めたまま、I・F の扱いは下のとおり */
#define A64_SPSR_EL1H          0x5
#define A64_SPSR_D             (1 << 9)
#define A64_SPSR_A             (1 << 8)
#define A64_SPSR_I             (1 << 7)
#define A64_SPSR_F             (1 << 6)
/* タスクの最初: 割込みを許可して始める */
#define TK2_INITIAL_SPSR_IRQ   (A64_SPSR_EL1H | A64_SPSR_D | A64_SPSR_A | A64_SPSR_F)
#define TK2_INITIAL_MSTATUS_IRQ TK2_INITIAL_SPSR_IRQ

/* 割込みのトラップフレーム（exception.S）: x0〜x30、ELR、SPSR、元の sp、ESR、FAR、TPIDR_EL0、空き = 304 バイト */
#define A64_TF_SIZE           304
#define A64_TF_ELR            248
#define A64_TF_SPSR           256
#define A64_TF_SP             264
#define A64_TF_ESR            272
#define A64_TF_FAR            280
#define A64_TF_TLS            288     /* EL0 のスレッドポインタ（TPIDR_EL0。EL0 からの例外のときだけ意味がある） */

#ifndef __ASSEMBLER__
typedef struct {
    void *ssp;
} TSKCTXB;
#endif

#endif
