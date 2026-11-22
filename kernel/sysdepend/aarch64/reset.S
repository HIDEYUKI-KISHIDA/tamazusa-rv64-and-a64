/* kernel/sysdepend/aarch64/reset.S
 * EL 降下・BSS クリア・スタック設定・M1 入口
 *
 * 修正点（仕様書 v0.3 比）:
 *   - BSS ゼロクリアを cpu_el1_init 前に追加
 *   - SCR_EL3 を 0x5b1 に修正（IRQ EL3 ルーティング除去）
 *   - x30 (LR) を EL 降下前に保護する必要はないが x29 をゼロ初期化
 */

.section .text.boot, "ax", %progbits
.global _start

/* SPSel=1: SP_EL1 を使う (EL1h) */
/* DAIF = 0xF << 6 = 0x3C0 */
/* EL1h = 0x5 */
#define PSR_EL1H_DAIF  0x3C5

/* SCR_EL3:
 *   bit10 RW  = 1  (EL1 は AArch64)
 *   bit8  HCE = 1  (HVC 命令有効)
 *   bit0  NS  = 1  (Non-secure)
 *   bit1  IRQ = 0  (IRQ は EL3 へルーティングしない)
 *   bit2  FIQ = 0
 *   bit3  EA  = 0
 * = 0x501
 * ※ 0x3b1 (旧値) は bit1=IRQ=1, bit4=ST=1, bit5=RES0 が立っており危険
 */
#define SCR_EL3_BOOT  0x501

/*
 * Linux の arm64 の Image の見出し（64 バイト）。U-Boot の booti はこれを見て読み込み先を決める。
 *   text_offset 0x20_0000、flags 0x2（リトルエンディアン・4KiB のページ・RAM の先頭から 2MiB 区切りの位置）
 *   → RAM の先頭が 0x4000_0000 のボード（QEMU virt・Allwinner A733）では 0x4020_0000 に置かれる（リンク先と同じ）
 *   QEMU に ELF で渡す場合も、先頭の命令（b）で本体へ飛ぶだけなので害はない。
 */
_start:
    b       .Lentry               /* code0 */
    .long   0                     /* code1 */
    .quad   0x200000              /* text_offset */
    .quad   __image_size          /* image_size（BSS とスタックを含む） */
    .quad   0x2                   /* flags */
    .quad   0, 0, 0               /* res2〜res4 */
    .ascii  "ARM\x64"            /* magic 0x644d5241 */
    .long   0                     /* res5 */

.Lentry:
    mov     x21, x0               /* x0 = デバイスツリーの物理アドレス（Linux の起動の決まり。U-Boot の booti が渡す） */
    /* --- 他コア停止（SMP 未実装・D6） --- */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, .Lprimary_core
.Lsecondary_wfi:
    wfi
    b       .Lsecondary_wfi

.Lprimary_core:
    /* --- CurrentEL 確認 --- */
    mrs     x0, CurrentEL
    and     x0, x0, #0xC          /* EL フィールドは [3:2] */
    /* 入ってきた EL を残す（psci.c が PSCI の呼び方を決めるのと、起動ログ用）。
     * .data に置く（BSS はこのあと消すので使えない） */
    lsr     x1, x0, #2
    ldr     x2, =boot_entry_el
    str     w1, [x2]
    ldr     x2, =rv_boot_dtb
    str     x21, [x2]
    cmp     x0, #0xC
    b.eq    .Lfrom_el3
    cmp     x0, #0x8
    b.eq    .Lfrom_el2
    b       .Lel1_setup

.Lfrom_el3:
    /* EL3 → EL1 降下 */
#ifdef BOARD_GIC_V3
    /* GICv3: 下の EL がシステムレジスタ（ICC_*）を使えるようにする
     * ICC_SRE_EL3 / ICC_SRE_EL2 = SRE | DFB | DIB | Enable */
    mov     x0, #0xF
    msr     S3_6_C12_C12_5, x0      /* ICC_SRE_EL3 */
    isb
    msr     S3_4_C12_C9_5, x0       /* ICC_SRE_EL2 */
    isb
#endif
    mov     x0, #SCR_EL3_BOOT
    msr     scr_el3, x0
    isb
    /* SPSR_EL3: EL1h + DAIF マスク */
    mov     x0, #PSR_EL1H_DAIF
    msr     spsr_el3, x0
    adr     x0, .Lel1_setup
    msr     elr_el3, x0
    eret

.Lfrom_el2:
    /* EL2 → EL1 降下 */
#ifdef BOARD_GIC_V3
    /* GICv3: EL1 が ICC_* を使えるようにし（ICC_SRE_EL2 = SRE|DFB|DIB|Enable）、
     * 仮想 CPU interface は使わない（ICH_HCR_EL2 = 0） */
    mov     x0, #0xF
    msr     S3_4_C12_C9_5, x0       /* ICC_SRE_EL2 */
    isb
    msr     S3_4_C12_C11_0, xzr     /* ICH_HCR_EL2 */
#endif
    /* HCR_EL2.RW=1: 下位ELはAArch64 */
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0
    /* cnthctl_el2: EL1 物理タイマアクセス許可 */
    mov     x0, #3
    msr     cnthctl_el2, x0
    /* cntvoff_el2: 仮想タイマオフセットをゼロに */
    msr     cntvoff_el2, xzr
    /* SPSR_EL2: EL1h + DAIF マスク */
    mov     x0, #PSR_EL1H_DAIF
    msr     spsr_el2, x0
    adr     x0, .Lel1_setup
    msr     elr_el2, x0
    eret

.Lel1_setup:
    /* --- SP_EL1 を選択 --- */
    msr     spsel, #1
    isb

    /* --- スタックポインタ設定 --- */
    ldr     x0, =knl_kernel_stack_top
    mov     sp, x0

    /* --- フレームポインタ初期化（バックトレース終端） --- */
    mov     x29, #0

    /* --- BSS ゼロクリア ---
     * cpu_el1_init より前に必須。グローバル変数が不定になる。
     */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    cmp     x0, x1
    b.hs    .Lbss_done
.Lbss_loop:
    str     xzr, [x0], #8
    cmp     x0, x1
    b.lo    .Lbss_loop
.Lbss_done:

    /* --- CPU EL1 初期化（CPACR, SCTLR, VBAR） --- */
    bl      cpu_el1_init

    /* --- ボード初期化（UART 等） --- */
    bl      board_init

    /* --- M5: T-Kernel エントリ --- */
    bl      knl_main

.Lhang:
    wfi
    b       .Lhang

.section .data
.balign 4
.global boot_entry_el
boot_entry_el:
    .word   0

/* デバイスツリーの物理アドレス（入口の x0。som_pins.c・fdt.c が読む。0 や不正なら board_init が探す） */
.balign 8
.global rv_boot_dtb
rv_boot_dtb:
    .quad   0
