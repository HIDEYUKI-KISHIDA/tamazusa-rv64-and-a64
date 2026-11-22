/*
 * mmu_a64.c — AArch64 のページング（4KiB の粒度、仮想アドレス 39 ビット、3 段）
 *
 *   RISC-V 版（mmu_sv39.c）と同じ形にした。表の段の数（3 段・512 個ずつ）も Sv39 と同じ。
 *
 *   TTBR0（低いアドレス）… カーネルとタスクのユーザー空間
 *     0x0000_0000〜0x3FFF_FFFF : ユーザー領域（MMIO はここに写さない）
 *       0x0040_0000 コード（EL0 実行可）、0x0040_1000 データ・スタック、0x0040_2000〜 追加のスタック 4 ページ
 *       物理はリンカの .user セクション
 *     0x4000_0000〜0x7FFF_FFFF : RAM（恒等写像。2MiB のブロック、EL1 だけ読み書き実行）
 *       rv_mmu_probe_page を含む 2MiB だけ 4KiB に分け、その 1 ページを読み取り専用にする（M8）
 *   TTBR1（高いアドレス）… 装置
 *     0xFFFF_FFC0_0000_0000 + 物理 0〜2GiB : MMIO（Device-nGnRnE、実行不可）。1〜2GiB は RAM（試験用）
 *
 *   タスクごとのユーザー空間（rv_uspace_create）: 最上段の表を複製し、0 番（0〜1GiB）だけ差し替える。
 *   ディスパッチャがタスクの tskctxb.uatb（= TTBR0 の値、0 ならカーネルの値）を TTBR0 に入れる。
 */
#include <stdint.h>
#include "rv_priv.h"

/* 記述子 */
#define D_VALID     (1UL << 0)
#define D_TABLE     (3UL << 0)          /* L1・L2 の表、L3 のページ */
#define D_BLOCK     (1UL << 0)          /* L1・L2 のブロック */
#define D_ATTR(n)   ((uint64_t)(n) << 2)
#define D_AP_EL0    (1UL << 6)          /* EL0 からも触れる */
#define D_AP_RO     (1UL << 7)          /* 読み取り専用 */
#define D_SH_INNER  (3UL << 8)
#define D_AF        (1UL << 10)
#define D_NG        (1UL << 11)
#define D_PXN       (1UL << 53)
#define D_UXN       (1UL << 54)
#define PA_MASK     0x0000FFFFFFFFF000UL

#define ATTR_NORMAL 0                   /* MAIR の 0 番: Normal、Write-Back */
#define ATTR_DEVICE 1                   /* MAIR の 1 番: Device-nGnRnE */
#define MAIR_VALUE  (0xFFUL | (0x00UL << 8))

/* T0SZ = T1SZ = 25（39 ビット）、4KiB、内側共有、Write-Back、物理アドレス 40 ビット */
#define TCR_VALUE   ((25UL << 0) | (1UL << 8) | (1UL << 10) | (3UL << 12) | (0UL << 14) | \
                     (25UL << 16) | (1UL << 24) | (1UL << 26) | (3UL << 28) | (2UL << 30) | \
                     (2UL << 32))

#define PAGE_SIZE   4096UL
#define MEGA_SIZE   (2UL * 1024 * 1024)
#define GIGA_SIZE   (1024UL * 1024 * 1024)
#define L1_IDX(va)  ((((uintptr_t)(va)) >> 30) & 0x1ff)
#define L2_IDX(va)  ((((uintptr_t)(va)) >> 21) & 0x1ff)
#define L3_IDX(va)  ((((uintptr_t)(va)) >> 12) & 0x1ff)

typedef uint64_t pte_t;

static pte_t pt_root[512]     __attribute__((aligned(4096)));   /* TTBR0 */
static pte_t pt_ram_l2[512]   __attribute__((aligned(4096)));   /* 0x4000_0000 の 1GiB */
static pte_t pt_probe_l3[512] __attribute__((aligned(4096)));   /* probe を含む 2MiB */
static pte_t pt_user_l2[512]  __attribute__((aligned(4096)));   /* 0〜1GiB（ユーザー） */
static pte_t pt_user_l3[512]  __attribute__((aligned(4096)));   /* 0x0040_0000 の 2MiB */
static pte_t pt_hi_root[512]  __attribute__((aligned(4096)));   /* TTBR1 */

#define RV_USER_EXTRA_STACKS 4
static uint8_t user_stack_pool[RV_USER_EXTRA_STACKS][PAGE_SIZE] __attribute__((aligned(4096)));

extern uint8_t __user_start[];

/* カーネルのページ表の TTBR0 の値（ディスパッチャが uatb = 0 のタスクに使う） */
uint64_t rv_kernel_satp;
/* 装置のアドレスの足し算（MMU の前は 0） */
uintptr_t rv_io_offset = 0;

#define RV_USPACE_MAX 4
struct rv_uspace {
    pte_t root[512];
    pte_t l2[512];
    pte_t l3[512];
    uint8_t data[PAGE_SIZE];
    uint8_t stack[PAGE_SIZE];
};
static struct rv_uspace uspace[RV_USPACE_MAX] __attribute__((aligned(4096)));

/* M8 の試験で書き込みが禁止されることを確かめるページ */
uint8_t rv_mmu_probe_page[PAGE_SIZE] __attribute__((aligned(4096)));

extern void uart_puts(const char *s);

static void put_hex(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[19];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        buf[2 + i] = hex[(v >> (60 - 4 * i)) & 0xf];
    }
    buf[18] = '\0';
    uart_puts(buf);
}

static pte_t table(const pte_t *next)
{
    return ((uintptr_t)next & PA_MASK) | D_TABLE;
}

/* カーネルの RAM: EL1 だけ。ro なら読み取り専用 */
static pte_t kram(uintptr_t pa, int block, int ro)
{
    return (pa & PA_MASK) | (block ? D_BLOCK : D_TABLE) | D_ATTR(ATTR_NORMAL) | D_SH_INNER | D_AF |
           D_UXN | (ro ? D_AP_RO : 0);
}

/* ユーザーのページ: EL0 から。x なら EL0 で実行でき、w なら書ける。EL1 では実行しない */
static pte_t upage(const void *pa, int w, int x)
{
    return ((uintptr_t)pa & PA_MASK) | D_TABLE | D_ATTR(ATTR_NORMAL) | D_SH_INNER | D_AF | D_NG |
           D_AP_EL0 | (w ? 0 : D_AP_RO) | D_PXN | (x ? 0 : D_UXN);
}

static void tlb_flush_all(void)
{
    asm volatile("dsb ishst\n\ttlbi vmalle1\n\tdsb ish\n\tisb" ::: "memory");
}

void rv_mmu_init(void)
{
    const uintptr_t ram_base = 0x40000000UL;
    const uintptr_t probe = (uintptr_t)rv_mmu_probe_page;
    const uintptr_t probe_mega = probe & ~(MEGA_SIZE - 1);
    uint64_t sctlr;
    unsigned i;

    /* RAM: 0x4000_0000 からの 1GiB を 2MiB のブロックで */
    pt_root[L1_IDX(ram_base)] = table(pt_ram_l2);
    for (i = 0; i < 512; i++) {
        pt_ram_l2[i] = kram(ram_base + (uintptr_t)i * MEGA_SIZE, 1, 0);
    }
    /* probe を含む 2MiB だけ 4KiB に分け、probe の 1 ページを読み取り専用に */
    for (i = 0; i < 512; i++) {
        uintptr_t pa = probe_mega + (uintptr_t)i * PAGE_SIZE;
        pt_probe_l3[i] = kram(pa, 0, pa == probe);
    }
    pt_ram_l2[L2_IDX(probe_mega)] = table(pt_probe_l3);

    /* ユーザー領域（0〜1GiB の表の中の 0x0040_0000） */
    pt_root[L1_IDX(USER_VA_BASE)] = table(pt_user_l2);
    pt_user_l2[L2_IDX(USER_VA_BASE)] = table(pt_user_l3);
    pt_user_l3[L3_IDX(USER_VA_BASE) + 0] = upage(__user_start, 0, 1);
    pt_user_l3[L3_IDX(USER_VA_BASE) + 1] = upage(__user_start + PAGE_SIZE, 1, 0);
    for (i = 0; i < RV_USER_EXTRA_STACKS; i++) {
        pt_user_l3[L3_IDX(USER_VA_BASE) + 2 + i] = upage(user_stack_pool[i], 1, 0);
    }

    /* 装置: 高いアドレスに物理 0〜1GiB。RAM（1〜2GiB）も同じ形で見せる
     * （RV_IO を通して RAM の上の「にせの装置」を触る試験（M34）のため。RISC-V 版も RAM まで写している） */
    pt_hi_root[L1_IDX(RV_IO_HIGH_BASE)] = (0UL & PA_MASK) | D_BLOCK | D_ATTR(ATTR_DEVICE) | D_AF |
                                          D_PXN | D_UXN;
    pt_hi_root[L1_IDX(RV_IO_HIGH_BASE) + 1] = (0x40000000UL & PA_MASK) | D_BLOCK | D_ATTR(ATTR_DEVICE) | D_AF |
                                              D_PXN | D_UXN;

    rv_kernel_satp = (uint64_t)(uintptr_t)pt_root;
    asm volatile("dsb sy" ::: "memory");
    asm volatile("msr mair_el1, %0" :: "r"(MAIR_VALUE));
    asm volatile("msr tcr_el1, %0" :: "r"(TCR_VALUE));
    asm volatile("msr ttbr0_el1, %0" :: "r"(rv_kernel_satp));
    asm volatile("msr ttbr1_el1, %0" :: "r"((uint64_t)(uintptr_t)pt_hi_root));
    asm volatile("isb");
    tlb_flush_all();
    asm volatile("ic iallu\n\tdsb ish\n\tisb" ::: "memory");

    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);     /* M・C・I */
    asm volatile("msr sctlr_el1, %0\n\tisb" :: "r"(sctlr) : "memory");

    rv_io_offset = RV_IO_HIGH_BASE;     /* これ以降、装置は高いアドレスから触る */
    uart_puts("[board_init] MMU OK (4KiB pages, 39-bit VA) TTBR0=");
    put_hex(rv_kernel_satp);
    uart_puts("\r\n");
}

/* スロット slot のユーザー空間を（作り直して）その TTBR0 の値を返す。失敗時 0 */
uint64_t rv_uspace_create(int slot)
{
    struct rv_uspace *u;
    unsigned i;

    if (slot < 0 || slot >= RV_USPACE_MAX) {
        return 0;
    }
    u = &uspace[slot];
    for (i = 0; i < 512; i++) {
        u->root[i] = pt_root[i];        /* カーネルの部分は共有 */
        u->l2[i] = 0;
        u->l3[i] = 0;
    }
    for (i = 0; i < PAGE_SIZE; i++) {
        u->data[i] = __user_start[PAGE_SIZE + i];
        u->stack[i] = 0;
    }
    u->root[L1_IDX(USER_VA_BASE)] = table(u->l2);
    u->l2[L2_IDX(USER_VA_BASE)] = table(u->l3);
    u->l3[L3_IDX(USER_VA_BASE) + 0] = upage(__user_start, 0, 1);
    u->l3[L3_IDX(USER_VA_BASE) + 1] = upage(u->data, 1, 0);
    u->l3[L3_IDX(USER_VA_BASE) + 2] = upage(u->stack, 1, 0);
    asm volatile("dsb ishst" ::: "memory");
    return (uint64_t)(uintptr_t)u->root;
}

uint8_t *rv_uspace_data_page(int slot)
{
    return (slot >= 0 && slot < RV_USPACE_MAX) ? uspace[slot].data : 0;
}

void rv_mmu_switch(uint64_t ttbr0)
{
    asm volatile("dsb ishst\n\tmsr ttbr0_el1, %0\n\tisb" :: "r"(ttbr0) : "memory");
    tlb_flush_all();
}

pte_t *rv_mmu_kernel_root(void)
{
    return pt_root;
}

uint64_t rv_mmu_read_satp(void)
{
    uint64_t v;
    asm volatile("mrs %0, ttbr0_el1" : "=r"(v));
    return v;
}

