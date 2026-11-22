/* kernel/tk/m18_linux_hello_test.c — M18: RISC-V の Linux 用プログラム（musl）を動かす
 *                                    （SMODE=1 かつ MMU=1）
 *
 *   ディスクの hello_musl.elf（user/linux/hello_musl.c を musl で静的リンクしたもの）を、
 *   引数 2 つ（"hello_musl", "x"）と環境変数（HOME=/root ほか）付きで起動する。
 *   1. printf の出力が出る（writev / ioctl / 起動時のスタック）
 *   2. argc・argv・環境変数が届いている
 *   3. malloc（brk / mmap）で取ったメモリが使える
 *   4. 浮動小数点の計算と表示ができる
 *   5. 終了コード 7（exit_group）
 *   6. 終わったあと、ページがすべて返っている
 *   musl が用意されていないとき（ディスクに hello_musl.elf がない）は SKIP。
 *
 * 合格の目印: [M18] PASS
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
extern int rv_uproc_last_error;
extern int rv_page_free_count(void);
extern long rv_run_user_in_space(uint64_t satp, uintptr_t entry, uintptr_t user_sp, long arg);
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);

#define ELF_MAX (512 * 1024)
static uint64_t elf_buf[ELF_MAX / 8];

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M18] OK   " : "[M18] NG   ");
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

int m18_run_linux_hello_test(void)
{
    static const char *const argv[] = { "hello_musl", "x", 0 };
    static const char *const envp[] = { "HOME=/root", "PATH=/bin", "TERM=vt100", 0 };
    struct tarfs_file f;
    struct rv_uproc *p;
    int free0 = rv_page_free_count();
    int64_t n;
    long r;
    const char *out;
    int pass = 1;

    uart_puts("\r\n[M18] Linux (musl) program test\r\n");

    if (tarfs_find("hello_musl.elf", &f) != 1) {
        uart_puts("[M18] SKIP (hello_musl.elf is not on the disk: run tools/build_musl_rv64.sh)\r\n");
        return 1;
    }
    if (f.size > ELF_MAX || (n = tarfs_read_all(&f, elf_buf, ELF_MAX)) != (int64_t)f.size) {
        uart_puts("[M18] could not read hello_musl.elf\r\n[M18] FAIL\r\n");
        return 0;
    }
    p = rv_uproc_load_linux(elf_buf, (size_t)n, argv, envp);
    if (p == NULL) {
        uart_puts("[M18] load failed\r\n[M18] FAIL\r\n");
        return 0;
    }
    rv_console_capture(1);
    r = rv_run_user_in_space(rv_uproc_satp(p), rv_uproc_entry(p), rv_uproc_sp(p), 0);
    rv_console_capture(0);
    rv_uproc_free(p);
    out = rv_console_captured();

    pass &= check(contains(out, "Hello from musl on Tamazusa-RV: 42"), "printf output (writev, startup stack)");
    pass &= check(contains(out, "argc=2 argv[0]=hello_musl HOME=/root"), "argc, argv and environment received");
    pass &= check(contains(out, "malloc 100000 OK (p[99999]=0x5a)"), "malloc'd memory usable (brk / mmap)");
    pass &= check(contains(out, "float: 0.333"), "floating point works");
    pass &= check(r == 7, "exit code 7 (exit_group)");
    pass &= check(rv_page_free_count() == free0, "all pages returned");

    uart_puts(pass ? "[M18] PASS\r\n" : "[M18] FAIL\r\n");
    return pass;
}
