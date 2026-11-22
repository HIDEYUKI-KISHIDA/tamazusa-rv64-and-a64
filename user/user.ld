/* user.ld — ユーザープログラム（ELF）の配置
 *   0x4000_0000 から: コード + 読み取り専用データ（R/X）
 *   次のページ境界から: データ + bss（R/W）
 * スタックはカーネルのローダが用意する（0x7FFF_D000〜0x7FFF_EFFF、sp = 0x7FFF_F000）
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);  /* R + X */
    data PT_LOAD FLAGS(6);  /* R + W */
}

SECTIONS {
    . = 0x40000000;
    .text : { *(.text.start) *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) } :text

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
    } :data
    .bss : { *(.sbss .sbss.*) *(.bss .bss.* COMMON) } :data

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) }
}
