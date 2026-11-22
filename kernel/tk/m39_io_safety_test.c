/* kernel/tk/m39_io_safety_test.c — M39: 機器の安全の決まり 6（誰が動かせるか）・7（記録）（SMODE=1 かつ MMU=1）
 *
 *   user/linux/iotest.c を動かす:
 *   1. root は動かせる。uid 1000 は読めるが動かせない・解除できない・root に戻れない・止めることはできる
 *   2. io のグループ（20）に入った uid 1000 は動かせる
 *   3. ネットワークのソケットを開いたプロセスとその子は、読めるが動かせない。安全な状態へはできる
 *   4. /dev/iolog に、時刻・プロセス・プログラム・uid と、解除・値・断ったこと・停止が残る
 *   そのあと、記録が /data/log/io.log に書かれている（/data があるとき）。
 *
 * 合格の目印: [M39] PASS
 */
#include <stdint.h>
#include <stddef.h>
#include "../fs/tarfs.h"
#include "../fs/ext2.h"

extern void uart_puts(const char *s);
struct rv_uproc;
extern struct rv_uproc *rv_linux_load_path(const char *path, const char *const argv[],
                                           const char *const envp[], long *err);
extern long rv_linux_run_process(struct rv_uproc *p);
extern void rv_console_set_test_input(const char *s);
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);
extern int rv_linux_proc_count(void);
extern void rv_io_policy(int *gid, int *net_ok);
extern void rv_io_log_flush(void);
extern void rv_io_stop(int why);
extern volatile unsigned long rv_io_denied;

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M39] OK   " : "[M39] NG   ");
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

int m39_run_io_safety_test(void)
{
    static const char *const argv[] = { "iotest", 0 };
    static const char *const envp[] = { "HOME=/", "PATH=/bin:/", 0 };
    static char filebuf[8192];
    struct tarfs_file f;
    struct rv_uproc *p;
    const char *out;
    long r;
    int pass = 1, gid = -1, net = -1;
    unsigned long denied0 = rv_io_denied;

    uart_puts("\r\n[M39] device safety rules 6 (who may control) and 7 (log) test\r\n");
    if (tarfs_find("iotest.elf", &f) != 1) {
        uart_puts("[M39] SKIP (no iotest.elf)\r\n");
        return 1;
    }
    rv_io_policy(&gid, &net);
    pass &= check(gid == 20 && net == 0, "/etc/io.conf: access group 20, network deny");
    p = rv_linux_load_path("iotest.elf", argv, envp, &r);
    if (p == NULL) {
        uart_puts("[M39] load failed\r\n[M39] FAIL\r\n");
        return 0;
    }
    rv_console_set_test_input("");
    rv_console_capture(1);
    r = rv_linux_run_process(p);
    rv_console_capture(0);
    rv_console_set_test_input(0);
    out = rv_console_captured();

    pass &= check(contains(out, "IOTEST ok root:") && contains(out, "IOTEST ok uid 1000: can read")
                  && contains(out, "IOTEST ok uid 1000 in group io"),
                  "rule 6: root can control; uid 1000 can read and stop but not control; group io can control");
    pass &= check(contains(out, "IOTEST ok after opening an AF_INET socket") && rv_io_denied >= denied0 + 3,
                  "rule 6: a process that opened a network socket (and its child) cannot control");
    pass &= check(contains(out, "IOTEST ok /dev/iolog"), "rule 7: /dev/iolog records time, process, program, uid and actions");
    if (ext2_mounted() && !ext2_readonly()) {
        uint32_t ino;
        char redir[EXT2_PATH_MAX];
        struct ext2_stat st;
        long n = -1;
        rv_io_log_flush();
        if (ext2_lookup("log/io.log", 1, &ino, redir) == 0 && ext2_stat(ino, &st) == 0) {
            uint64_t off = st.size > sizeof(filebuf) - 1 ? st.size - (sizeof(filebuf) - 1) : 0;
            n = ext2_read(ino, off, filebuf, sizeof(filebuf) - 1);
        }
        filebuf[n > 0 ? n : 0] = 0;
        pass &= check(n > 0 && contains(filebuf, "iotest uid 0 SET HEATER 30.000")
                      && contains(filebuf, "iotest uid 0 net DENY-NET HEATER"),
                      "rule 7: the log is kept on the disk (/data/log/io.log)");
    } else {
        uart_puts("[M39] --   no /data: the log stays in memory only\r\n");
    }
    pass &= check(r == 0 && contains(out, "IOTEST done, 0 failed") && rv_linux_proc_count() == 0, "exit 0");
    rv_io_stop(3);                      /* 起動したときと同じ「まだ動かさない」に戻す（あとの試験のため） */
    uart_puts(pass ? "[M39] PASS\r\n" : "[M39] FAIL\r\n");
    return pass;
}
