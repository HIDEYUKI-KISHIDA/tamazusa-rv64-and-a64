/* kernel/tk/m15_shell_test.c — M15: シェル（SMODE=1 かつ MMU=1）
 *
 *   ディスクの sh.elf を、用意した入力で動かし、画面への出力を記録して確かめる。
 *   入力（1 行ずつ）:
 *     help / ls / cat readme.txt / hexx + Backspace 2 回 + llo（= hello）/ echo hi there /
 *     nosuch / cat nothing.txt / exit
 *   確かめること:
 *     1. シェルが exit で終わる（終了コード 0）
 *     2. ls にディスクのファイル（sh.elf、readme.txt）が出る
 *     3. cat readme.txt で中身が出る（文字列の引数がプログラムに渡る）
 *     4. Backspace で直した「hello」が動き、終了コード 90 が表示される
 *     5. echo の文字列がそのまま出る
 *     6. ないプログラムは「not found」、cat の開けないファイルは「cannot open」と終了コード 2
 *     7. すべて終わったあと、ページがすべて返っている
 *
 *   あわせて rv_run_shell（テストのあとキーボードから使うシェル）もここに置く。
 *
 * 合格の目印: [M15] PASS
 */
#include <stdint.h>
#include <stddef.h>

extern void uart_puts(const char *s);

struct rv_uproc;
extern struct rv_uproc *rv_load_program(const char *path, long *err);
extern void rv_uproc_free(struct rv_uproc *p);
extern uint64_t  rv_uproc_satp(const struct rv_uproc *p);
extern uintptr_t rv_uproc_entry(const struct rv_uproc *p);
extern uintptr_t rv_uproc_sp(const struct rv_uproc *p);
extern int rv_page_free_count(void);
extern long rv_run_user_in_space(uint64_t satp, uintptr_t entry, uintptr_t user_sp, long arg);
extern void rv_console_set_test_input(const char *s);
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);

static const char shell_input[] =
    "help\r"
    "ls\r"
    "cat readme.txt\r"
    "hexx\x7f\x7f" "llo\r"       /* hexx → he → hello */
    "echo hi there\r"
    "nosuch\r"
    "cat nothing.txt\r"
    "exit\r";

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M15] OK   " : "[M15] NG   ");
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

static long run_sh(void)
{
    long err = 0;
    struct rv_uproc *p = rv_load_program("sh.elf", &err);
    long r;

    if (p == NULL) {
        return -1000 + err;
    }
    r = rv_run_user_in_space(rv_uproc_satp(p), rv_uproc_entry(p), rv_uproc_sp(p), 0);
    rv_uproc_free(p);
    return r;
}

int m15_run_shell_test(void)
{
    int pass = 1;
    int free0 = rv_page_free_count();
    const char *out;
    long r;

    uart_puts("\r\n[M15] shell test (scripted input)\r\n");

    rv_console_set_test_input(shell_input);
    rv_console_capture(1);
    r = run_sh();
    rv_console_capture(0);
    rv_console_set_test_input(NULL);
    out = rv_console_captured();
    uart_puts("\r\n");

    pass &= check(r == 0, "shell ran and ended with 'exit' (exit 0)");
    pass &= check(contains(out, "sh.elf") && contains(out, "readme.txt"), "ls listed the disk files");
    pass &= check(contains(out, "B-Free disk test:"), "cat readme.txt printed the file (string argument passed)");
    pass &= check(contains(out, "[elf] hello") && contains(out, "[exit 90]"),
                  "line edited with Backspace ran hello, exit code shown");
    pass &= check(contains(out, "\r\nhi there\r\n"), "echo printed its text");
    pass &= check(contains(out, "nosuch: not found") && contains(out, "cannot open")
                  && contains(out, "[exit 2]"),
                  "unknown command / unopenable file reported");
    pass &= check(rv_page_free_count() == free0, "all pages returned");

    uart_puts(pass ? "[M15] PASS\r\n" : "[M15] FAIL\r\n");
    return pass;
}

/*
 * テストのあと、キーボードから使うシェル（exit または Ctrl-D で終わる）
 *   ディスクに busybox.elf があれば busybox の ash（Linux 互換層で動く）、
 *   なければ小さな sh.elf
 */
extern struct rv_uproc *rv_linux_load_path(const char *path, const char *const argv[],
                                           const char *const envp[], long *err);
extern long rv_linux_run_process(struct rv_uproc *p);

/* bash があれば bash、なければ busybox の ash（どちらも Linux 互換層で動く） */
static int run_busybox_sh(void)
{
    static const char *const sh_argv[] = { "sh", 0 };
    static const char *const bash_argv[] = { "bash", 0 };
    static const char *const envp[] = { "HOME=/", "PATH=/bin:/", "TERM=vt100", "PS1=tamazusa# ", 0 };
    struct rv_uproc *p;
    long err;

    p = rv_linux_load_path("bash.elf", bash_argv, envp, &err);
    if (p != NULL) {
        uart_puts("\r\n[shell] starting bash (Linux compatibility layer). Type 'exit' to quit.\r\n");
    } else {
        p = rv_linux_load_path("busybox.elf", sh_argv, envp, &err);
        if (p == NULL) {
            return 0;
        }
        uart_puts("\r\n[shell] starting busybox ash (Linux compatibility layer). Type 'exit' to quit.\r\n");
    }
    rv_linux_run_process(p);
    return 1;
}

void rv_run_shell(void)
{
    long r;

    if (run_busybox_sh()) {
        return;
    }
    uart_puts("\r\n[shell] starting sh.elf from the disk (type 'exit' to quit)\r\n");
    r = run_sh();
    if (r != 0) {
        uart_puts("[shell] could not start or ended with an error\r\n");
    }
}
