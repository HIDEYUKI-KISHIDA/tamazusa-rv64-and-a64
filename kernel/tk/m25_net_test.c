/* kernel/tk/m25_net_test.c — M25: Linux 互換層のソケット（ネットワーク）（SMODE=1 かつ MMU=1）
 *
 *   ディスクの nettest.elf（user/linux/nettest.c）をプロセスとして動かす。
 *   TCP（listen・connect・accept・窓より大きい送受信・相手が閉じたら 0・ECONNREFUSED・
 *   待たない accept と poll）・UDP・getsockname を 127.0.0.1 だけで 10 項目確かめる
 *   （LAN の相手がいなくても動く）。
 *   1. nettest がすべて合格（exit 0）
 *   2. ソケットがすべて閉じられた
 *   3. 終わったあと、プロセスの表が空になり、ページがすべて返っている
 *   musl が用意されていないとき（ディスクに nettest.elf がない）は SKIP。
 *
 * 合格の目印: [M25] PASS
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
extern int rv_sock_count(void);
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M25] OK   " : "[M25] NG   ");
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

int m25_run_net_test(void)
{
    static const char *const argv[] = { "nettest", 0 };
    static const char *const envp[] = { "HOME=/", 0 };
    struct tarfs_file f;
    struct rv_uproc *p;
    int free0 = rv_page_free_count();
    long r;
    int pass = 1;

    uart_puts("\r\n[M25] Linux socket test (TCP and UDP over loopback)\r\n");
    if (tarfs_find("nettest.elf", &f) != 1) {
        uart_puts("[M25] SKIP (nettest.elf is not on the disk: run tools/build_musl_rv64.sh)\r\n");
        return 1;
    }
    p = rv_linux_load_path("nettest.elf", argv, envp, &r);
    if (p == NULL) {
        uart_puts("[M25] load failed\r\n[M25] FAIL\r\n");
        return 0;
    }
    rv_console_capture(1);
    r = rv_linux_run_process(p);
    rv_console_capture(0);

    pass &= check(r == 0 && contains(rv_console_captured(), "nettest: all checks passed"),
                  "nettest passed all 10 checks (TCP and UDP over 127.0.0.1)");
    pass &= check(rv_sock_count() == 0, "all sockets closed");
    pass &= check(rv_linux_proc_count() == 0 && rv_page_free_count() == free0,
                  "process table empty and all pages returned");
    uart_puts(pass ? "[M25] PASS\r\n" : "[M25] FAIL\r\n");
    return pass;
}
