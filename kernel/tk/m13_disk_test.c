/* kernel/tk/m13_disk_test.c — M13: 仮想ディスク（virtio-blk）と tar のファイル
 *
 *   QEMU に build/disk.img（tar）を virtio-blk として付けて起動する（make qemu が付ける）。
 *   1. virtio-blk が見つかり、容量が 0 でない
 *   2. 先頭セクタが tar の見出し（"ustar"）
 *   3. ファイル一覧を出し、readme.txt の中身が期待どおり
 *   4. ないファイルは「見つからない」
 *   5. ディスクの外のセクタを読むと失敗が返る（カーネルは止まらない）
 *   6.（MMU=1 のみ）ディスクの hello.elf を読み込んで実行 → exit(90)。
 *      カーネルに埋め込んだものと同じ中身であること
 *
 * 合格の目印: [M13] PASS
 */
#include <stdint.h>
#include <stddef.h>
#include "../fs/tarfs.h"

extern void uart_puts(const char *s);
extern void uart_putc(char c);

#define README_PREFIX   "B-Free disk test:"
#define FILE_BUF_SIZE   (64 * 1024)

static uint64_t file_buf[FILE_BUF_SIZE / 8];   /* 8 バイト境界（ELF ローダの条件） */

static void put_num(long v)
{
    char buf[24];
    int n = 0;
    unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;

    if (v < 0) {
        uart_putc('-');
    }
    do {
        buf[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u != 0);
    while (n > 0) {
        uart_putc(buf[--n]);
    }
}

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M13] OK   " : "[M13] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

static int starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

#if TK2_MMU
struct rv_uproc;
extern struct rv_uproc *rv_uproc_load(const void *image, size_t len);
extern void rv_uproc_free(struct rv_uproc *p);
extern uint64_t  rv_uproc_satp(const struct rv_uproc *p);
extern uintptr_t rv_uproc_entry(const struct rv_uproc *p);
extern uintptr_t rv_uproc_sp(const struct rv_uproc *p);
extern long rv_run_user_in_space(uint64_t satp, uintptr_t entry, uintptr_t user_sp, long arg);
extern const uint8_t rv_blob_hello_elf[];
extern const uint8_t rv_blob_hello_elf_end[];

static int run_elf_from_disk(void)
{
    struct tarfs_file f;
    int64_t n;
    struct rv_uproc *p;
    long r;
    size_t blen = (size_t)(rv_blob_hello_elf_end - rv_blob_hello_elf);
    size_t i;
    int same;

    if (tarfs_find("hello.elf", &f) != 1 || f.size > FILE_BUF_SIZE) {
        return check(0, "hello.elf on disk");
    }
    n = tarfs_read_all(&f, file_buf, FILE_BUF_SIZE);
    same = (n == (int64_t)blen);
    for (i = 0; same && i < blen; i++) {
        if (((const uint8_t *)file_buf)[i] != rv_blob_hello_elf[i]) {
            same = 0;
        }
    }
    p = (n > 0) ? rv_uproc_load(file_buf, (size_t)n) : NULL;
    if (p == NULL) {
        return check(0, "hello.elf from disk -> loaded");
    }
    r = rv_run_user_in_space(rv_uproc_satp(p), rv_uproc_entry(p), rv_uproc_sp(p), 0);
    rv_uproc_free(p);
    return check(same && r == 90,
                 "hello.elf read from disk (same as built), loaded and ran -> exit(90)");
}
#endif

int m13_run_disk_test(void)
{
    static uint8_t sec[512] __attribute__((aligned(8)));
    struct tarfs_file f;
    int pass = 1;
    int i, r;
    int64_t n;

    uart_puts("\r\n[M13] virtio-blk disk + tar file test\r\n");

    /* 1 */
    if (!virtio_blk_init()) {
        check(0, "virtio-blk disk found");
        uart_puts("[M13] FAIL\r\n");
        return 0;
    }
    uart_puts("[M13] disk: ");
    put_num((long)virtio_blk_capacity());
    uart_puts(" sectors\r\n");
    pass &= check(virtio_blk_capacity() > 0, "virtio-blk disk found, capacity > 0");

    /* 2 */
    r = virtio_blk_read(0, sec, 1);
    pass &= check(r == 0 && sec[257] == 'u' && sec[258] == 's' && sec[259] == 't'
                  && sec[260] == 'a' && sec[261] == 'r',
                  "sector 0 read, it is a tar header (ustar)");

    /* 3 */
    for (i = 0; ; i++) {
        r = tarfs_entry(i, &f);
        if (r <= 0) {
            break;
        }
        uart_puts("[M13]   ");
        uart_puts(f.name);
        uart_puts("  ");
        put_num((long)f.size);
        uart_puts(" bytes\r\n");
    }
    pass &= check(r == 0 && i >= 2, "listed files on the disk");

    r = tarfs_find("readme.txt", &f);
    n = (r == 1) ? tarfs_read_all(&f, file_buf, FILE_BUF_SIZE - 1) : -1;
    if (n >= 0) {
        ((char *)file_buf)[n] = 0;
        uart_puts("[M13] readme.txt: ");
        uart_puts((const char *)file_buf);
        if (n > 0 && ((char *)file_buf)[n - 1] != '\n') {
            uart_puts("\r\n");
        }
    }
    pass &= check(n > 0 && starts_with((const char *)file_buf, README_PREFIX),
                  "readme.txt read, contents as expected");

    /* 4 */
    pass &= check(tarfs_find("nothing.txt", &f) == 0, "missing file -> not found");

    /* 5 */
    r = virtio_blk_read(virtio_blk_capacity() + 10, sec, 1);
    pass &= check(r != 0, "read beyond the end of the disk -> error returned");

#if TK2_MMU
    /* 6 */
    pass &= run_elf_from_disk();
#endif

    uart_puts(pass ? "[M13] PASS\r\n" : "[M13] FAIL\r\n");
    return pass;
}
