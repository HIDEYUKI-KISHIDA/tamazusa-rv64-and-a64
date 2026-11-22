/* kernel/tk/m24_bash_test.c — M24: bash（SMODE=1 かつ MMU=1）
 *
 *   ディスクの bash.elf（GNU bash を musl で静的リンクしたもの）に台本を流し込む。
 *   配列・中かっこの展開・関数・算術・[[ ]]・パイプ・busybox のコマンド（/bin/ls など）・
 *   #! のスクリプト・trap とシグナル・終了コードを確かめる。
 *   1. 台本の出力がすべて期待どおり
 *   2. 終わったあと、プロセスの表が空になり、ページとカーネルのメモリがすべて返っている
 *   bash が用意されていないとき（ディスクに bash.elf がない）は SKIP。
 *
 * 合格の目印: [M24] PASS
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
extern void rv_console_set_test_input(const char *s);
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);

static const char script[] =
    "echo bash=${BASH_VERSINFO[0]}\n"
    "a=(x y z); echo arr=${#a[@]}${a[2]}\n"
    "echo br {1..4}\n"
    "f() { local v=$1; echo fn=$((v*v)); }; f 12\n"
    "[[ abc == a* ]] && echo glob=ok\n"
    "s=hello; echo up=${s^^} sub=${s:1:3}\n"
    "echo one two three | tr ' ' '\\n' | sort -r | head -1\n"
    "/bin/ls / | grep -c elf > /dev/null && echo ls=ok\n"
    "/hello.sh p q\n"
    "/catself\n"
    "trap 'echo trapped=USR1' USR1; kill -USR1 $$\n"
    "( exit 6 ); echo sub=$?\n"
    "exit 4\n";

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M24] OK   " : "[M24] NG   ");
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

int m24_run_bash_test(void)
{
    static const char *const argv[] = { "bash", "--norc", "--noediting", "-i", 0 };
    static const char *const envp[] = { "HOME=/", "PATH=/bin:/", "TERM=vt100", "PS1=$ ", 0 };
    struct tarfs_file f;
    struct rv_uproc *p;
    const char *out;
    int free0 = rv_page_free_count();
    size_t heap0 = rv_heap_in_use;
    long r;
    int pass = 1;

    uart_puts("\r\n[M24] bash test\r\n");
    if (tarfs_find("bash.elf", &f) != 1) {
        uart_puts("[M24] SKIP (bash.elf is not on the disk: run tools/build_bash_rv64.sh)\r\n");
        return 1;
    }
    p = rv_linux_load_path("bash.elf", argv, envp, &r);
    if (p == NULL) {
        uart_puts("[M24] load failed\r\n[M24] FAIL\r\n");
        return 0;
    }
    rv_console_set_test_input(script);
    rv_console_capture(1);
    r = rv_linux_run_process(p);
    rv_console_capture(0);
    rv_console_set_test_input(0);
    out = rv_console_captured();

    pass &= check(contains(out, "bash=5") && contains(out, "arr=3z") && contains(out, "br 1 2 3 4"),
                  "version, arrays, brace expansion");
    pass &= check(contains(out, "fn=144") && contains(out, "glob=ok") && contains(out, "up=HELLO sub=ell"),
                  "functions, [[ ]], parameter expansion");
    pass &= check(contains(out, "two") && contains(out, "ls=ok"), "pipes and busybox commands (/bin/ls)");
    pass &= check(contains(out, "script: hello from /hello.sh, args: p q")
                  && contains(out, "#!/bin/cat\nprinted-by-cat"), "#! scripts (sh and cat as the interpreter)");
    pass &= check(contains(out, "trapped=USR1") && contains(out, "sub=6"), "trap and subshell exit status");
    pass &= check(r == 4, "exit 4");
    pass &= check(rv_linux_proc_count() == 0 && rv_page_free_count() == free0 && rv_heap_in_use == heap0,
                  "process table empty, pages and kernel memory returned");
    uart_puts(pass ? "[M24] PASS\r\n" : "[M24] FAIL\r\n");
    return pass;
}
