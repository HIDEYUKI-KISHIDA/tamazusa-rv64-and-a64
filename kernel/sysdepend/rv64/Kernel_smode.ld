/* Kernel_smode.ld — 構成 B: OpenSBI の後ろ（0x80200000）に配置 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x80200000, LENGTH = 128M
}

SECTIONS {
    . = 0x80200000;

    .text.boot : { *(.text.boot) } > RAM

    .text : {
        *(.text .text.*)
    } > RAM

    .rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) } > RAM

    .data : { *(.data .data.*) *(.sdata .sdata.*) } > RAM

    /* ユーザープログラム（MMU=1）: コード 1 ページ + データ・スタック 1 ページを連続で */
    .user : ALIGN(4096) {
        __user_start = .;
        KEEP(*(.user.text))
        . = __user_start + 0x1000;
        KEEP(*(.user.data))
        . = __user_start + 0x2000;
        __user_end = .;
    } > RAM

    .bss : {
        . = ALIGN(8);
        __bss_start = .;
        __global_pointer$ = . + 0x800;
        *(.sbss .sbss.*)
        *(.bss .bss.* COMMON)
        __bss_end = .;
    } > RAM

    .stack (NOLOAD) : ALIGN(16) {
        knl_kernel_stack_bottom = .;
        . += 0x10000;
        knl_kernel_stack_top = .;
    } > RAM
}
