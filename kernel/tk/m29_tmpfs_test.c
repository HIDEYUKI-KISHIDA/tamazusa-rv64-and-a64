/* kernel/tk/m29_tmpfs_test.c — M29: 書き込めるフォルダ /tmp（SMODE=1 かつ MMU=1）
 *
 *   busybox の sh に台本を流し込み、メモリの上の /tmp を確かめる。
 *   1. 作る・追記・読む・大きさ（> と >>、cat、wc）
 *   2. 一覧（ls /tmp、ルートの一覧に tmp が出る）、名前の変更（mv）
 *   3. 何ページにもまたがるファイル（seq 1 20000、約 106KiB）とその写し（cp・cmp）
 *   4. ディスクには書けないまま（EROFS）、/tmp とディスクの間の mv はできない（EXDEV）
 *   5. 消す（rm）。終わったあと /tmp は空、ページとカーネルのメモリがすべて返っている
 *
 * 合格の目印: [M29] PASS
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
extern int rv_tmp_count(void);
extern void rv_console_set_test_input(const char *s);
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);

static const char script[] =
    "echo hello > /tmp/a.txt\n"
    "echo world >> /tmp/a.txt\n"
    "cat /tmp/a.txt\n"
    "echo size=$(wc -c < /tmp/a.txt)\n"
    "echo list=$(ls /tmp)\n"
    "mv /tmp/a.txt /tmp/b.txt; echo moved=$(ls /tmp)\n"
    "echo root=$(ls / | grep -c '^tmp$')\n"
    "seq 1 20000 > /tmp/big; echo lines=$(wc -l < /tmp/big) bytes=$(wc -c < /tmp/big)\n"
    "cp /tmp/big /tmp/big2 && cmp /tmp/big /tmp/big2 && echo copy=same\n"
    "echo x > /readme.txt 2>/dev/null || echo rofs=ok\n"
    "mv /tmp/b.txt /b.txt 2>/dev/null || echo xdev=ok\n"
    "rm /tmp/b.txt /tmp/big /tmp/big2; echo left=$(ls /tmp | wc -l)\n"
    "exit 0\n";

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M29] OK   " : "[M29] NG   ");
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

int m29_run_tmpfs_test(void)
{
    static const char *const argv[] = { "sh", 0 };
    static const char *const envp[] = { "HOME=/", "PATH=/bin:/", "TERM=vt100", 0 };
    struct tarfs_file f;
    struct rv_uproc *p;
    const char *out;
    int free0 = rv_page_free_count();
    size_t heap0 = rv_heap_in_use;
    long r;
    int pass = 1;

    uart_puts("\r\n[M29] /tmp (writable folder in memory) test\r\n");
    if (tarfs_find("busybox.elf", &f) != 1) {
        uart_puts("[M29] SKIP (busybox.elf is not on the disk)\r\n");
        return 1;
    }
    p = rv_linux_load_path("busybox.elf", argv, envp, &r);
    if (p == NULL) {
        uart_puts("[M29] load failed\r\n[M29] FAIL\r\n");
        return 0;
    }
    rv_console_set_test_input(script);
    rv_console_capture(1);
    r = rv_linux_run_process(p);
    rv_console_capture(0);
    rv_console_set_test_input(0);
    out = rv_console_captured();

    pass &= check(contains(out, "hello\nworld") && contains(out, "size=12"), "create, append, read, size");
    pass &= check(contains(out, "list=a.txt") && contains(out, "moved=b.txt") && contains(out, "root=1"),
                  "ls /tmp, tmp in the root listing, rename");
    pass &= check(contains(out, "lines=20000 bytes=108894") && contains(out, "copy=same"),
                  "multi-page file (seq 1 20000), cp and cmp");
    pass &= check(contains(out, "rofs=ok") && contains(out, "xdev=ok"),
                  "disk stays read-only, no mv between /tmp and the disk");
    pass &= check(contains(out, "left=0") && rv_tmp_count() == 0, "rm, /tmp is empty");
    pass &= check(r == 0, "exit 0");
    pass &= check(rv_linux_proc_count() == 0 && rv_page_free_count() == free0 && rv_heap_in_use == heap0,
                  "process table empty, pages and kernel memory returned");
    uart_puts(pass ? "[M29] PASS\r\n" : "[M29] FAIL\r\n");
    return pass;
}
