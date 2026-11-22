/* kernel/tk/m22_busybox_test.c — M22: busybox の ash（SMODE=1 かつ MMU=1）
 *
 *   ディスクの busybox.elf（RISC-V の Linux 用、musl で静的リンク）を「sh」として起動し、
 *   用意したコマンドを標準入力から与えて、出力を記録して確かめる。
 *   1. echo・変数と計算・for
 *   2. ls /・cat（ディスクのファイル）
 *   3. パイプ（echo | tr、seq | wc）
 *   4. 外部のプログラム（/hello_musl.elf）を起動し、終了コード $? を受け取る
 *   5. uname
 *   6. exit 3 で終わる
 *   busybox が用意されていないとき（ディスクに busybox.elf がない）は SKIP。
 *
 * 合格の目印: [M22] PASS
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
extern size_t rv_heap_in_use;
extern int rv_heap_check(void);
extern void rv_console_set_test_input(const char *s);
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);

static const char script[] =
    "echo hello from ash\n"
    "x=$((6*7)); echo answer=$x\n"
    "for i in 1 2 3; do echo loop $i; done\n"
    "ls /\n"
    "cat /readme.txt\n"
    "echo abc | tr a-z A-Z\n"
    "seq 1 5 | wc -l\n"
    "/hello_musl.elf a b\n"
    "echo status=$?\n"
    "uname -s -m\n"
    "exit 3\n";

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M22] OK   " : "[M22] NG   ");
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

int m22_run_busybox_test(void)
{
    static const char *const argv[] = { "sh", 0 };
    static const char *const envp[] = { "HOME=/", "PATH=/", "TERM=vt100", 0 };
    struct tarfs_file f;
    struct rv_uproc *p;
    const char *out;
    int free0 = rv_page_free_count();
    size_t heap0 = rv_heap_in_use;
    long r;
    int pass = 1;

    uart_puts("\r\n[M22] busybox ash test\r\n");
    if (tarfs_find("busybox.elf", &f) != 1) {
        uart_puts("[M22] SKIP (busybox.elf is not on the disk: run tools/build_busybox_rv64.sh or _a64.sh)\r\n");
        return 1;
    }
    p = rv_linux_load_path("busybox.elf", argv, envp, &r);
    if (p == NULL) {
        uart_puts("[M22] load failed\r\n[M22] FAIL\r\n");
        return 0;
    }
    rv_console_set_test_input(script);
    rv_console_capture(1);
    r = rv_linux_run_process(p);
    rv_console_capture(0);
    rv_console_set_test_input(0);
    out = rv_console_captured();

    pass &= check(contains(out, "hello from ash") && contains(out, "answer=42")
                  && contains(out, "loop 3"), "echo, arithmetic, for loop");
    pass &= check(contains(out, "busybox.elf") && contains(out, "B-Free disk test:"), "ls / and cat");
    pass &= check(contains(out, "ABC") && contains(out, "\n5\n"), "pipes (echo | tr, seq | wc -l)");
    pass &= check(contains(out, "argc=3 argv[0]=/hello_musl.elf") && contains(out, "status=7"),
                  "external program run, exit status 7 in $?");
#if defined(__aarch64__)
    pass &= check(contains(out, "Tamazusa aarch64"), "uname -s -m");
#else
    pass &= check(contains(out, "Tamazusa riscv64"), "uname -s -m");
#endif
    pass &= check(r == 3, "exit 3");
    pass &= check(rv_linux_proc_count() == 0 && rv_page_free_count() == free0,
                  "process table empty and all pages returned");
    pass &= check(rv_heap_in_use == heap0, "kernel memory of ended processes returned (task stacks)");
    pass &= check(rv_heap_check() == 0, "kernel memory guards intact (no stack overflow)");

    uart_puts(pass ? "[M22] PASS\r\n" : "[M22] FAIL\r\n");
    return pass;
}
