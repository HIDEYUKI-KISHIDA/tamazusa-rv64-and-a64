/* kernel/tk/m26_ssh_test.c — M26: 疑似端末と ssh（SMODE=1 かつ MMU=1）
 *
 *   1. ptytest.elf（user/linux/ptytest.c）: /dev/ptmx・/dev/pts/N の 8 項目
 *      （開く・端末の大きさ・ttyname・Backspace とエコー・行ごとの読み・CR LF・Ctrl-C・生の入力・EIO）
 *   2. dropbear があれば: bash の台本で sshd を起動し、自分自身（127.0.0.1）へ ssh して
 *      コマンドを動かし、出力と終了コード 7 が返ることを確かめる。最後に sshd を止める。
 *      ssh で入った人の T88-BASIC は、機器を読めるが、ESTOP 0（解除）は断られる（安全の決まり 6）。
 *   3. 終わったあと、プロセスの表が空になり、ソケット・疑似端末が閉じ、ページが返っている
 *   musl がない（ptytest.elf がない）ときは SKIP。dropbear がないときは 2 を飛ばす。
 *
 * 合格の目印: [M26] PASS
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
extern int rv_pty_count(void);
extern void rv_console_set_test_input(const char *s);
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);

static const char ssh_script[] =
    /* ホスト鍵はイメージに入れない。試験のたびに /data に作る（dropbearkey はハードリンクを使うので、/tmp には作れない） */
    "mkdir -p /data/etc/dropbear; rm -f /data/etc/dropbear/m26_hostkey /data/etc/dropbear/m26_hostkey.pub\n"
    "dropbearkey -t ed25519 -f /data/etc/dropbear/m26_hostkey >/dev/null 2>&1\n"
    "dropbear -F -p 22 -r /data/etc/dropbear/m26_hostkey -P /dev/null 2>/dev/null & DP=$!\n"
    "sleep 1\n"
    "DROPBEAR_PASSWORD=tamazusa ssh -y -p 22 root@127.0.0.1 'echo ssh-ok $(uname -s); exit 7' </dev/null 2>/dev/null; echo rc=$?\n"
    /* 安全の決まり 6: ssh で入った人は、機器を読めるが動かせない（T88-BASIC で ESTOP 0 を試す） */
    "printf '10 PRINT \"SENSOR-OK\";ADC(\"TEMP\")>-100\\n20 ESTOP 0\\n30 PRINT \"AR\";\"MED\"\\n' > /tmp/net.bas\n"
    "DROPBEAR_PASSWORD=tamazusa ssh -y -p 22 root@127.0.0.1 'basic /tmp/net.bas' </dev/null 2>/dev/null\n"
    "rm -f /tmp/net.bas /data/etc/dropbear/m26_hostkey /data/etc/dropbear/m26_hostkey.pub\n"
    "kill $DP; wait\n"
    "exit 0\n";

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M26] OK   " : "[M26] NG   ");
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

int m26_run_ssh_test(void)
{
    static const char *const pty_argv[] = { "ptytest", 0 };
    static const char *const bash_argv[] = { "bash", "--norc", "--noediting", 0 };
    static const char *const envp[] = { "HOME=/", "PATH=/bin:/", "TERM=vt100", 0 };
    struct tarfs_file f;
    struct rv_uproc *p;
    int free0 = rv_page_free_count();
    long r;
    int pass = 1;

    uart_puts("\r\n[M26] pseudo terminal and ssh test\r\n");
    if (tarfs_find("ptytest.elf", &f) != 1) {
        uart_puts("[M26] SKIP (ptytest.elf is not on the disk: run tools/build_musl_rv64.sh)\r\n");
        return 1;
    }
    p = rv_linux_load_path("ptytest.elf", pty_argv, envp, &r);
    if (p == NULL) {
        uart_puts("[M26] load failed\r\n[M26] FAIL\r\n");
        return 0;
    }
    rv_console_capture(1);
    r = rv_linux_run_process(p);
    rv_console_capture(0);
    pass &= check(r == 0 && contains(rv_console_captured(), "ptytest: all checks passed"),
                  "ptytest passed all 8 checks (/dev/ptmx, echo, line editing, Ctrl-C, raw, EIO)");

    if (tarfs_find("dropbear.elf", &f) == 1 && tarfs_find("bash.elf", &f) == 1) {
        p = rv_linux_load_path("bash.elf", bash_argv, envp, &r);
        if (p == NULL) {
            uart_puts("[M26] bash load failed\r\n[M26] FAIL\r\n");
            return 0;
        }
        rv_console_set_test_input(ssh_script);
        rv_console_capture(1);
        r = rv_linux_run_process(p);
        rv_console_capture(0);
        rv_console_set_test_input(0);
        pass &= check(r == 0 && contains(rv_console_captured(), "ssh-ok Tamazusa")
                      && contains(rv_console_captured(), "rc=7")
                      && contains(rv_console_captured(), "SENSOR-OK-1")
                      && contains(rv_console_captured(), "Permission denied in 20")
                      && !contains(rv_console_captured(), "ARMED"),
                      "sshd (dropbear) and ssh to 127.0.0.1: password login, command output, exit code 7; over ssh, devices can be read but not armed");
    } else {
        uart_puts("[M26] (dropbear or bash is not on the disk: ssh part skipped)\r\n");
    }
    pass &= check(rv_linux_proc_count() == 0 && rv_sock_count() == 0 && rv_pty_count() == 0
                  && rv_page_free_count() == free0,
                  "process table empty, sockets and pseudo terminals closed, pages returned");
    uart_puts(pass ? "[M26] PASS\r\n" : "[M26] FAIL\r\n");
    return pass;
}
