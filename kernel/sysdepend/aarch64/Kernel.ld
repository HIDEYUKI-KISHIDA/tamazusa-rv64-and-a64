/* kernel/sysdepend/aarch64/Kernel.ld — load 0x40200000（RAM の先頭 2MiB は QEMU がデバイスツリーを置くので空ける） */

OUTPUT_ARCH(aarch64)

ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x40200000, LENGTH = 128M
}

SECTIONS {
    . = ORIGIN(RAM);
    .text.boot : { *(.text.boot) } > RAM

    .text : {
        *(.text.vectors)
        *(.text*)
    } > RAM

    .rodata : { *(.rodata*) } > RAM

    .data : { *(.data*) } > RAM

    /* ユーザープログラム（M9〜M11）: コード 1 ページ + データ・スタック 1 ページを連続で */
    .user : ALIGN(4096) {
        __user_start = .;
        KEEP(*(.user.text))
        . = __user_start + 0x1000;
        KEEP(*(.user.data))
        . = __user_start + 0x2000;
        __user_end = .;
    } > RAM

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } > RAM

    .mmu_bss (NOLOAD) : ALIGN(4096) {
        __mmu_tables_start = .;
        *(.mmu_bss)
        . = ALIGN(4096);
        __mmu_tables_end = .;
    } > RAM

    .stack (NOLOAD) : ALIGN(16) {
        knl_kernel_stack_bottom = .;
        . += 0x10000;
        knl_kernel_stack_top = .;
    } > RAM

    /* Image の見出し（reset.S）が使う: 読み込み先から終わり（BSS・スタックまで）までの大きさ */
    __image_size = . - ORIGIN(RAM);
}
