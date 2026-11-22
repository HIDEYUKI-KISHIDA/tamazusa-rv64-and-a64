/* kernel/tk/m23_signal_test.c — M23: Linux 互換層のシグナル（SMODE=1 かつ MMU=1）
 *
 *   ディスクの sigtest.elf（user/linux/sigtest.c）をプロセスとして動かす。
 *   処理関数・SA_SIGINFO・マスクと保留・無視・kill（走っている子・眠っている子）・
 *   SIGCHLD・EINTR・SA_RESTART・sigsuspend・SIGPIPE・プロセスグループ・/dev/null を
 *   14 項目で確かめ、すべて合えば exit(0)。
 *   1. sigtest がすべて合格（exit 0）
 *   2. 処理関数が実際に呼ばれた（カーネルの数）
 *   3. 終わったあと、プロセスの表が空になり、ページがすべて返っている
 *   musl が用意されていないとき（ディスクに sigtest.elf がない）は SKIP。
 *
 * 合格の目印: [M23] PASS
 */
#include <stdint.h>
#include <stddef.h>
#include "../fs/tarfs.h"

extern void uart_puts(const char *s);
struct rv_uproc;
extern struct rv_uproc *rv_linux_load_path(const char *path, const char *const argv[],
                                           const char *const envp[], long *err);
extern long rv_linux_run_process(struct rv_uproc *p);
extern int rv_linux_proc_count(void);
extern int rv_page_free_count(void);
extern volatile unsigned long rv_linux_signals_sent, rv_linux_signals_handled;
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M23] OK   " : "[M23] NG   ");
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

int m23_run_signal_test(void)
{
    static const char *const argv[] = { "sigtest", 0 };
    static const char *const envp[] = { "HOME=/", 0 };
    struct tarfs_file f;
    struct rv_uproc *p;
    unsigned long h0 = rv_linux_signals_handled;
    int free0 = rv_page_free_count();
    long r;
    int pass = 1;

    uart_puts("\r\n[M23] Linux signal test\r\n");
    if (tarfs_find("sigtest.elf", &f) != 1) {
        uart_puts("[M23] SKIP (sigtest.elf is not on the disk: run tools/build_musl_rv64.sh)\r\n");
        return 1;
    }
    p = rv_linux_load_path("sigtest.elf", argv, envp, &r);
    if (p == NULL) {
        uart_puts("[M23] load failed\r\n[M23] FAIL\r\n");
        return 0;
    }
    rv_console_capture(1);
    r = rv_linux_run_process(p);
    rv_console_capture(0);

    pass &= check(r == 0 && contains(rv_console_captured(), "sigtest: all checks passed"),
                  "sigtest passed all 14 checks (handler, mask, kill, EINTR, SA_RESTART, ...)");
    pass &= check(rv_linux_signals_handled - h0 >= 7, "signal handlers were run");
    pass &= check(rv_linux_proc_count() == 0 && rv_page_free_count() == free0,
                  "process table empty and all pages returned");
    uart_puts(pass ? "[M23] PASS\r\n" : "[M23] FAIL\r\n");
    return pass;
}
