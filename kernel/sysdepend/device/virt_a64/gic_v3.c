/* GICv3（GIC-600 など）— addresses from board_hw.h
 *
 *   Distributor     BOARD_GICD_BASE  （SPI の設定。いまはタイマ（PPI）しか使わないので、有効にするだけ）
 *   Redistributor   BOARD_GICR_BASE  （コアごとに 128KB: RD_base と SGI_base。PPI・SGI はここで設定する）
 *   CPU interface   システムレジスタ ICC_*_EL1（GICv2 の GICC のメモリは使わない）
 *
 *   QEMU virt（gic-version=3）: GICD 0x08000000、GICR 0x080A0000
 *   Allwinner A733: GICD 0x03400000、GICR 0x03460000（Linux の A733 のデバイスツリー案より）
 *
 *   gic_init / gic_ack / gic_eoi の形は gic_v2.c と同じ。
 */

#include <stdint.h>
#include <board_hw.h>   /* ボードごと（-I の順で選ぶ） */
#include "rv_priv.h"

#define REG32(a)            (*(volatile uint32_t *)(a))
#define REG64(a)            (*(volatile uint64_t *)(a))

/* Distributor */
#define GICD_CTLR           REG32(RV_IO(BOARD_GICD_BASE) + 0x0000)
#define GICD_CTLR_RWP       (1u << 31)
#define GICD_CTLR_ARE       (1u << 4)       /* ARE_NS（2 つのセキュリティ状態のとき）または ARE（DS=1 のとき） */
#define GICD_CTLR_GRP1      (1u << 1)       /* EnableGrp1A（NS から見たとき）または EnableGrp1（DS=1 のとき） */

/* Redistributor: 1 つのコアに RD_base（64KB）と SGI_base（64KB） */
#define GICR_STRIDE         0x20000UL
#define GICR_WAKER_OFF      0x0014
#define GICR_TYPER_OFF      0x0008
#define GICR_SGI_OFF        0x10000UL
#define GICR_IGROUPR0_OFF   0x0080
#define GICR_ISENABLER0_OFF 0x0100
#define GICR_ICENABLER0_OFF 0x0180
#define GICR_IPRIORITYR_OFF 0x0400
#define GICR_IGRPMODR0_OFF  0x0D00

#define WAKER_PROC_SLEEP    (1u << 1)
#define WAKER_CHILD_ASLEEP  (1u << 2)
#define TYPER_LAST          (1ull << 4)
#define TYPER_VLPIS         (1ull << 1)

/* CPU interface（システムレジスタ。古いアセンブラでも通るよう S3_... の形で書く） */
#define ICC_PMR_EL1         "S3_0_C4_C6_0"
#define ICC_IAR1_EL1        "S3_0_C12_C12_0"
#define ICC_EOIR1_EL1       "S3_0_C12_C12_1"
#define ICC_BPR1_EL1        "S3_0_C12_C12_3"
#define ICC_CTLR_EL1        "S3_0_C12_C12_4"
#define ICC_SRE_EL1         "S3_0_C12_C12_5"
#define ICC_IGRPEN1_EL1     "S3_0_C12_C12_7"

#define TIMER_PRIO          0xA0u   /* PMR 0xFF より小さい（優先度が高い）値。NS から見ても 0 にならない */

static uintptr_t g_rd;              /* このコアの Redistributor（RD_base） */

/* MPIDR の Aff3.Aff2.Aff1.Aff0 と GICR_TYPER[63:32] を比べて、このコアの Redistributor を探す */
static uintptr_t find_redist(void)
{
    uint64_t mpidr;
    uint32_t aff;
    uintptr_t rd = RV_IO(BOARD_GICR_BASE);
    int i;

    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    aff = (uint32_t)(((mpidr >> 32) & 0xFFu) << 24) | (uint32_t)(mpidr & 0xFFFFFFu);
    for (i = 0; i < 64; i++) {
        uint64_t typer = REG64(rd + GICR_TYPER_OFF);
        if ((uint32_t)(typer >> 32) == aff) {
            return rd;
        }
        if (typer & TYPER_LAST) {
            break;
        }
        rd += GICR_STRIDE;
        if (typer & TYPER_VLPIS) {
            rd += GICR_STRIDE;          /* GICv4: 仮想 LPI の 2 枠（VLPI_base など）が続く */
        }
    }
    return RV_IO(BOARD_GICR_BASE);         /* 見つからないときは先頭（1 コアの QEMU と同じ） */
}

static void wait_rwp(void)
{
    int n;
    for (n = 0; n < 1000000 && (GICD_CTLR & GICD_CTLR_RWP); n++) {
    }
}

void gic_v3_init(void)
{
    uintptr_t sgi;
    int n;

    /* 1. Distributor: いったん止め、ARE（アフィニティでの割り振り）と Group 1 を有効にする */
    GICD_CTLR = 0;
    wait_rwp();
    GICD_CTLR = GICD_CTLR_ARE | GICD_CTLR_GRP1;
    wait_rwp();

    /* 2. Redistributor: このコアを起こす（ProcessorSleep を下ろし、ChildrenAsleep が下りるのを待つ） */
    g_rd = find_redist();
    REG32(g_rd + GICR_WAKER_OFF) &= ~WAKER_PROC_SLEEP;
    for (n = 0; n < 1000000 && (REG32(g_rd + GICR_WAKER_OFF) & WAKER_CHILD_ASLEEP); n++) {
    }

    /* 3. SGI・PPI: すべて Group 1（Non-secure）にし、いったん無効。タイマの PPI だけ優先度を付けて有効に */
    sgi = g_rd + GICR_SGI_OFF;
    REG32(sgi + GICR_ICENABLER0_OFF) = 0xFFFFFFFFu;
    REG32(sgi + GICR_IGROUPR0_OFF)   = 0xFFFFFFFFu;
    REG32(sgi + GICR_IGRPMODR0_OFF)  = 0;
    {
        volatile uint8_t *pri = (volatile uint8_t *)(sgi + GICR_IPRIORITYR_OFF);
        pri[BOARD_TIMER_IRQ] = (uint8_t)TIMER_PRIO;
    }
    REG32(sgi + GICR_ISENABLER0_OFF) = 1u << BOARD_TIMER_IRQ;

    /* 4. CPU interface: システムレジスタを使う。すべての優先度を通し、Group 1 を受け付ける */
    {
        uint64_t v;
        asm volatile("mrs %0, " ICC_SRE_EL1 : "=r"(v));
        v |= 1u;                                    /* SRE */
        asm volatile("msr " ICC_SRE_EL1 ", %0" :: "r"(v));
        asm volatile("isb");
    }
    asm volatile("msr " ICC_PMR_EL1 ", %0" :: "r"((uint64_t)0xFF));
    asm volatile("msr " ICC_BPR1_EL1 ", %0" :: "r"((uint64_t)0));
    asm volatile("msr " ICC_CTLR_EL1 ", %0" :: "r"((uint64_t)0));   /* EOImode=0: EOIR で優先度を下ろし、終わりにもする */
    asm volatile("msr " ICC_IGRPEN1_EL1 ", %0" :: "r"((uint64_t)1));
    asm volatile("isb");
}

void gic_init(void)
{
    gic_v3_init();
}

unsigned int gic_ack(void)
{
    uint64_t v;
    asm volatile("mrs %0, " ICC_IAR1_EL1 : "=r"(v));
    asm volatile("dsb sy" ::: "memory");
    return (unsigned int)(v & 0xFFFFFFu);
}

void gic_eoi(unsigned int irq)
{
    asm volatile("msr " ICC_EOIR1_EL1 ", %0" :: "r"((uint64_t)irq));
    asm volatile("isb");
}

/* 試験と起動ログ用: GICv3 として動いているか（SRE が立ち、IGRPEN1 が 1） */
int gic_v3_active(void)
{
    uint64_t sre, en;
    asm volatile("mrs %0, " ICC_SRE_EL1 : "=r"(sre));
    asm volatile("mrs %0, " ICC_IGRPEN1_EL1 : "=r"(en));
    return (sre & 1u) && (en & 1u);
}

/* SPI（装置の割込み、INTID 32 以上）を有効にする: Group 1、優先度はタイマと同じ、このコア（Aff 0.0.0.0）へ */
void gic_spi_enable(unsigned irq)
{
    uintptr_t d = RV_IO(BOARD_GICD_BASE);
    uint32_t bit = 1u << (irq % 32);

    if (irq < 32 || irq >= 1020) {
        return;
    }
    REG32(d + 0x0080 + (irq / 32) * 4) |= bit;          /* GICD_IGROUPR: Group 1 */
    REG32(d + 0x0D00 + (irq / 32) * 4) &= ~bit;         /* GICD_IGRPMODR: Non-secure Group 1 */
    ((volatile uint8_t *)(d + 0x0400))[irq] = (uint8_t)TIMER_PRIO;   /* GICD_IPRIORITYR */
    REG64(d + 0x6000 + irq * 8) = 0;                    /* GICD_IROUTER: Aff3.2.1.0 = 0（1 コア） */
    REG32(d + 0x0100 + (irq / 32) * 4) = bit;           /* GICD_ISENABLER */
    wait_rwp();
}
