/* kernel/tk/m20_linux_fs_test.c — M20: Linux 互換層のファイル（SMODE=1 かつ MMU=1）
 *
 *   ディスクの fstest.elf（user/linux/fstest.c）を動かす。ふつうの C のやり方
 *   （fopen・fgets・stat・open・lseek・pread・dup・opendir・readdir・getcwd・access）で
 *   19 項目を確かめ、すべて合えば exit(0)。
 *   1. fstest がすべての項目に合格（exit 0）
 *   2. 開いたファイルの表が、プログラムの後始末で片付く
 *   musl が用意されていないとき（ディスクに fstest.elf がない）は SKIP。
 *
 * 合格の目印: [M20] PASS
 */
#include <stdint.h>
#include <stddef.h>
#include "../fs/tarfs.h"

extern void uart_puts(const char *s);
struct rv_uproc;
extern struct rv_uproc *rv_uproc_load_linux(const void *image, size_t len,
                                            const char *const argv[], const char *const envp[]);
extern void rv_uproc_free(struct rv_uproc *p);
extern uint64_t  rv_uproc_satp(const struct rv_uproc *p);
extern uintptr_t rv_uproc_entry(const struct rv_uproc *p);
extern uintptr_t rv_uproc_sp(const struct rv_uproc *p);
extern int rv_uproc_slot(const struct rv_uproc *p);
extern long rv_run_user_in_space(uint64_t satp, uintptr_t entry, uintptr_t user_sp, long arg);
extern int rv_linux_fs_open_count(int slot);
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);

#define ELF_MAX (512 * 1024)
static uint64_t elf_buf[ELF_MAX / 8];

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M20] OK   " : "[M20] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

static int contains(const char *hay, const char *needle)
{
    const char *h;
    for (h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (*b == 0) {
            return 1;
        }
    }
    return 0;
}

int m20_run_linux_fs_test(void)
{
    static const char *const argv[] = { "fstest", 0 };
    static const char *const envp[] = { "HOME=/root", 0 };
    struct tarfs_file f;
    struct rv_uproc *p;
    int slot, left;
    long r;
    int pass = 1;

    uart_puts("\r\n[M20] Linux file system calls test\r\n");
    if (tarfs_find("fstest.elf", &f) != 1) {
        uart_puts("[M20] SKIP (fstest.elf is not on the disk: run tools/build_musl_rv64.sh)\r\n");
        return 1;
    }
    if (f.size > ELF_MAX || tarfs_read_all(&f, elf_buf, ELF_MAX) != (int64_t)f.size) {
        uart_puts("[M20] could not read fstest.elf\r\n[M20] FAIL\r\n");
        return 0;
    }
    p = rv_uproc_load_linux(elf_buf, (size_t)f.size, argv, envp);
    if (p == NULL) {
        uart_puts("[M20] load failed\r\n[M20] FAIL\r\n");
        return 0;
    }
    slot = rv_uproc_slot(p);
    rv_console_capture(1);
    r = rv_run_user_in_space(rv_uproc_satp(p), rv_uproc_entry(p), rv_uproc_sp(p), 0);
    rv_console_capture(0);
    rv_uproc_free(p);
    left = rv_linux_fs_open_count(slot);

    pass &= check(r == 0 && contains(rv_console_captured(), "fstest: all 19 checks passed"),
                  "fstest passed all 19 checks (fopen, stat, lseek, pread, dup, readdir, ...)");
    pass &= check(left == 0, "file table cleared when the program ended");

    uart_puts(pass ? "[M20] PASS\r\n" : "[M20] FAIL\r\n");
    return pass;
}
