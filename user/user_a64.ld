/* user_a64.ld — ユーザープログラム（ELF）の配置（AArch64 版。RISC-V 版は user.ld）
 *   0x0040_0000 から: コード + 読み取り専用データ（R/X）
 *   次のページ境界から: データ + bss（R/W）
 * スタックはカーネルのローダが用意する（sp = 0x3FFF_F000）
 */
OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);  /* R + X */
    data PT_LOAD FLAGS(6);  /* R + W */
}

SECTIONS {
    . = 0x00400000;
    .text : { *(.text.start) *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text

    . = ALIGN(4096);
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.* COMMON) } :data

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) }
}
