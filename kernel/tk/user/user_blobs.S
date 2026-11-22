/* user_blobs.S — ELF のユーザープログラムをカーネルに埋め込む（ファイルシステムができるまで） */
.section .rodata
.balign 8
.globl rv_blob_hello_elf
.globl rv_blob_hello_elf_end
rv_blob_hello_elf:
    .incbin "build/user/hello.elf"
rv_blob_hello_elf_end:
