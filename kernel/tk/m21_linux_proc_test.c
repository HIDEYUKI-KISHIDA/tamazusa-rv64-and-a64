/* kernel/tk/m21_linux_proc_test.c — M21: Linux 互換層のプロセス（SMODE=1 かつ MMU=1）
 *
 *   ディスクの proctest.elf（user/linux/proctest.c）をプロセスとして動かす。
 *   fork・wait・メモリの複製・getppid・パイプ（入れ物より大きいデータも）・execve・
 *   WNOHANG・ECHILD・繰り返しの fork を 16 項目で確かめ、すべて合えば exit(0)。
 *   1. proctest がすべて合格（exit 0）
 *   2. 終わったあと、プロセスの表が空になり、ページがすべて返っている
 *   musl が用意されていないとき（ディスクに proctest.elf がない）は SKIP。
 *
 * 合格の目印: [M21] PASS
 */
#include <stdint.h>
#include <stddef.h>
#include "../fs/tarfs.h"

extern void uart_puts(const char *s);
extern void uart_putc(char c);
struct rv_uproc;
extern struct rv_uproc *rv_uproc_load_linux(const void *image, size_t len,
                                            const char *const argv[], const char *const envp[]);
extern long rv_linux_run_process(struct rv_uproc *p);
extern int rv_linux_proc_count(void);
extern int rv_page_free_count(void);
extern volatile unsigned long rv_linux_forks, rv_linux_execs, rv_linux_reaped;
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);

#define ELF_MAX (512 * 1024)
static uint64_t elf_buf[ELF_MAX / 8];

static void put_num(unsigned long v)
{
    char b[24];
    int n = 0;
    do {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        uart_putc(b[--n]);
    }
}

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M21] OK   " : "[M21] NG   ");
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

int m21_run_linux_proc_test(void)
{
    static const char *const argv[] = { "proctest", 0 };
    static const char *const envp[] = { "HOME=/root", 0 };
    struct tarfs_file f;
    struct rv_uproc *p;
    unsigned long f0 = rv_linux_forks, e0 = rv_linux_execs, r0 = rv_linux_reaped;
    int free0 = rv_page_free_count();
    long r;
    int pass = 1;

    uart_puts("\r\n[M21] Linux process test (fork, wait, pipe, execve)\r\n");
    if (tarfs_find("proctest.elf", &f) != 1) {
        uart_puts("[M21] SKIP (proctest.elf is not on the disk: run tools/build_musl_rv64.sh)\r\n");
        return 1;
    }
    if (f.size > ELF_MAX || tarfs_read_all(&f, elf_buf, ELF_MAX) != (int64_t)f.size) {
        uart_puts("[M21] could not read proctest.elf\r\n[M21] FAIL\r\n");
        return 0;
    }
    p = rv_uproc_load_linux(elf_buf, (size_t)f.size, argv, envp);
    if (p == NULL) {
        uart_puts("[M21] load failed\r\n[M21] FAIL\r\n");
        return 0;
    }
    rv_console_capture(1);
    r = rv_linux_run_process(p);
    rv_console_capture(0);

    uart_puts("[M21] forks ");
    put_num(rv_linux_forks - f0);
    uart_puts(", execs ");
    put_num(rv_linux_execs - e0);
    uart_puts(", reaped ");
    put_num(rv_linux_reaped - r0);
    uart_puts("\r\n");
    pass &= check(r == 0 && contains(rv_console_captured(), "proctest: all 16 checks passed"),
                  "proctest passed all 16 checks (fork, wait, pipe, execve, WNOHANG, ...)");
    pass &= check(rv_linux_proc_count() == 0 && rv_page_free_count() == free0,
                  "process table empty and all pages returned");

    uart_puts(pass ? "[M21] PASS\r\n" : "[M21] FAIL\r\n");
    return pass;
}
