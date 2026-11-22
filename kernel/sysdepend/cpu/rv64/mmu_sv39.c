/*
 * mmu_sv39.c — 構成 B-2: Sv39 ページング（S-mode のみ、MMU=1）
 *
 * 第 1 段: カーネル用の恒等マップ（仮想アドレス = 物理アドレス）
 *
 *   VA 0x0000_0000 - 0x3FFF_FFFF : 1GiB ページ 1 枚、R/W（MMIO: UART, PLIC, sifive_test など）
 *   VA 0x8000_0000 - 0xBFFF_FFFF : 2MiB ページ ×512、R/W/X（RAM）
 *       ただし rv_mmu_probe_page を含む 2MiB だけは 4KiB ページに分割し、
 *       rv_mmu_probe_page 1 枚を「読み取り専用」にする（M8 テスト用）
 *
 *   VA 0x4000_0000 - 0x4000_5FFF : ユーザー領域 6 ページ（U ビット付き）
 *       0x4000_0000 コード（R/X/U）、0x4000_1000 データ・スタック（R/W/U）
 *       物理はリンカの .user セクション（__user_start から 2 ページ）
 *       0x4000_2000〜0x4000_5FFF 追加のユーザースタック 4 ページ（R/W/U、第3段）
 *
 * 第4段: タスクごとのユーザー空間（rv_uspace_create）
 *   スロットごとに専用のページ表（root / L1 / L0）を持つ。カーネルの部分（VA 0 と RAM）は
 *   カーネルのページ表と同じ表を共有し、VA 0x4000_0000 の 1GiB だけ差し替える。
 *     0x4000_0000 コード（R/X/U）  … 物理は全スロット共通（.user の 1 ページ目）
 *     0x4000_1000 データ（R/W/U）  … スロット専用の複製（初期値は .user の 2 ページ目）
 *     0x4000_2000 スタック（R/W/U）… スロット専用
 *   ディスパッチャがタスクの tskctxb.uatb（= satp の値、0 ならカーネルの satp）を satp に入れる。
 *
 * A/D ビットはあらかじめ立てておく（ハードウェアによる自動更新を前提にしない）。
 * カーネルのページは G（グローバル）で U なし。ユーザー領域だけ U 付き・G なし。
 */
#include <stdint.h>
#include "rv_priv.h"

#define PTE_V   (1UL << 0)
#define PTE_R   (1UL << 1)
#define PTE_W   (1UL << 2)
#define PTE_X   (1UL << 3)
#define PTE_U   (1UL << 4)
#define PTE_G   (1UL << 5)
#define PTE_A   (1UL << 6)
#define PTE_D   (1UL << 7)

#define PAGE_SIZE       4096UL
#define MEGA_SIZE       (2UL * 1024 * 1024)
#define GIGA_SIZE       (1024UL * 1024 * 1024)

#define SATP_MODE_SV39  (8UL << 60)

#define PA_TO_PTE(pa)   ((((uintptr_t)(pa)) >> 12) << 10)
#define VPN2(va)        ((((uintptr_t)(va)) >> 30) & 0x1ff)
#define VPN1(va)        ((((uintptr_t)(va)) >> 21) & 0x1ff)
#define VPN0(va)        ((((uintptr_t)(va)) >> 12) & 0x1ff)

typedef uint64_t pte_t;

static pte_t pt_root[512]  __attribute__((aligned(4096)));
static pte_t pt_ram_l1[512] __attribute__((aligned(4096)));  /* 0x8000_0000 の 1GiB */
static pte_t pt_probe_l0[512] __attribute__((aligned(4096))); /* probe を含む 2MiB */
static pte_t pt_user_l1[512] __attribute__((aligned(4096)));  /* 0x4000_0000 の 1GiB */
static pte_t pt_user_l0[512] __attribute__((aligned(4096)));  /* 先頭 2MiB */

/* 追加のユーザースタック（同時に動くユーザープログラムごとに 1 ページ）
 *   VA 0x4000_2000 + i*0x1000（R/W/U） */
#define RV_USER_EXTRA_STACKS 4
static uint8_t user_stack_pool[RV_USER_EXTRA_STACKS][PAGE_SIZE] __attribute__((aligned(4096)));

extern uint8_t __user_start[];
#define USER_VA_BASE    0x40000000UL

/* カーネルのページ表の satp（ディスパッチャが uatb = 0 のタスクに使う） */
uint64_t rv_kernel_satp;

/* タスクごとのユーザー空間 */
#define RV_USPACE_MAX 4
struct rv_uspace {
    pte_t root[512];
    pte_t l1[512];
    pte_t l0[512];
    uint8_t data[PAGE_SIZE];
    uint8_t stack[PAGE_SIZE];
};
static struct rv_uspace uspace[RV_USPACE_MAX] __attribute__((aligned(4096)));

/* M8 テストで書き込みが禁止されることを確かめるページ */
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

static pte_t leaf(uintptr_t pa, pte_t perm)
{
    return PA_TO_PTE(pa) | perm | PTE_A | PTE_D | PTE_G | PTE_V;
}

static pte_t table(const pte_t *next)
{
    return PA_TO_PTE(next) | PTE_V;     /* R/W/X = 0 は「次の段の表」 */
}

void rv_mmu_init(void)
{
    const uintptr_t ram_base = 0x80000000UL;
    const uintptr_t probe = (uintptr_t)rv_mmu_probe_page;
    const uintptr_t probe_mega = probe & ~(MEGA_SIZE - 1);
    unsigned i;
    uint64_t satp;

    /* MMIO: 0 - 1GiB を 1GiB ページで（実行不可） */
    pt_root[0] = leaf(0x0UL, PTE_R | PTE_W);
    /* 同じ MMIO を高いアドレスにも写す（ユーザープログラムの空間には含めない） */
    pt_root[VPN2(RV_IO_HIGH_BASE)] = leaf(0x0UL, PTE_R | PTE_W);
    /* PCIe の装置のメモリの窓（QEMU virt では 0x4000_0000 から 1GiB）も高いアドレスにだけ写す */
    pt_root[VPN2(RV_IO_HIGH_BASE) + 1] = leaf(0x40000000UL, PTE_R | PTE_W);

    /* RAM: 0x8000_0000 からの 1GiB を 2MiB ページで */
    pt_root[VPN2(ram_base)] = table(pt_ram_l1);
    for (i = 0; i < 512; i++) {
        pt_ram_l1[i] = leaf(ram_base + (uintptr_t)i * MEGA_SIZE, PTE_R | PTE_W | PTE_X);
    }

    /* probe を含む 2MiB だけ 4KiB ページに分割し、probe 1 枚を読み取り専用に */
    for (i = 0; i < 512; i++) {
        uintptr_t pa = probe_mega + (uintptr_t)i * PAGE_SIZE;
        pte_t perm = (pa == probe) ? PTE_R : (PTE_R | PTE_W | PTE_X);
        pt_probe_l0[i] = leaf(pa, perm);
    }
    pt_ram_l1[VPN1(probe_mega)] = table(pt_probe_l0);

    /* ユーザー領域: U ビット付き、G なし */
    pt_root[VPN2(USER_VA_BASE)] = table(pt_user_l1);
    pt_user_l1[VPN1(USER_VA_BASE)] = table(pt_user_l0);
    pt_user_l0[0] = PA_TO_PTE(__user_start) | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D | PTE_V;
    pt_user_l0[1] = PA_TO_PTE(__user_start + PAGE_SIZE) | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D | PTE_V;
    for (i = 0; i < RV_USER_EXTRA_STACKS; i++) {
        pt_user_l0[2 + i] = PA_TO_PTE(user_stack_pool[i]) | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D | PTE_V;
    }

    satp = SATP_MODE_SV39 | (((uintptr_t)pt_root) >> 12);
    rv_kernel_satp = satp;
    asm volatile("sfence.vma zero, zero" ::: "memory");
    asm volatile("csrw satp, %0" :: "r"(satp) : "memory");
    asm volatile("sfence.vma zero, zero" ::: "memory");
    rv_io_offset = RV_IO_HIGH_BASE;     /* これ以降、装置は高いアドレスから触る */

    uart_puts("[board_init] Sv39 MMU OK satp=");
    put_hex(satp);
    uart_puts("\r\n");
}

static pte_t user_pte(const void *pa, pte_t perm)
{
    return PA_TO_PTE(pa) | perm | PTE_U | PTE_A | PTE_D | PTE_V;
}

/*
 * スロット slot のユーザー空間を（作り直して）その satp の値を返す。失敗時 0。
 * データページは .user の初期値に戻し、スタックは 0 で埋める。
 */
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
        u->l1[i] = 0;
        u->l0[i] = 0;
    }
    for (i = 0; i < PAGE_SIZE; i++) {
        u->data[i] = __user_start[PAGE_SIZE + i];
        u->stack[i] = 0;
    }
    u->root[VPN2(USER_VA_BASE)] = table(u->l1);
    u->l1[VPN1(USER_VA_BASE)] = table(u->l0);
    u->l0[0] = user_pte(__user_start, PTE_R | PTE_X);
    u->l0[1] = user_pte(u->data, PTE_R | PTE_W);
    u->l0[2] = user_pte(u->stack, PTE_R | PTE_W);
    asm volatile("" ::: "memory");
    return SATP_MODE_SV39 | (((uintptr_t)u->root) >> 12);
}

/* スロットのデータページ（カーネルから恒等マップで読む、テスト用） */
uint8_t *rv_uspace_data_page(int slot)
{
    return (slot >= 0 && slot < RV_USPACE_MAX) ? uspace[slot].data : 0;
}

void rv_mmu_switch(uint64_t satp)
{
    asm volatile("csrw satp, %0\n\tsfence.vma zero, zero" :: "r"(satp) : "memory");
}

/* カーネルのページ表（uspace.c がカーネルの部分を共有するため） */
pte_t *rv_mmu_kernel_root(void)
{
    return pt_root;
}

uint64_t rv_mmu_read_satp(void)
{
    uint64_t v;
    asm volatile("csrr %0, satp" : "=r"(v));
    return v;
}
