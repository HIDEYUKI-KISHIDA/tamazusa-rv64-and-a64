/* user_blobs_a64.S — ELF のユーザープログラムをカーネルに埋め込む（AArch64 版。ファイルシステムができるまで） */
.section .rodata
.balign 8
.globl rv_blob_hello_elf
.globl rv_blob_hello_elf_end
rv_blob_hello_elf:
    .incbin "build-a64/user/hello.elf"
rv_blob_hello_elf_end:
