/* kernel/tk/m36_data_test.c — M36: 書き込めるディスク /data（ext2）（SMODE=1 かつ MMU=1）
 *
 *   2 台目の virtio-blk（build/data.img、mke2fs で作った ext2）を /data に見せている。busybox の sh に台本を流し込む。
 *   1. 書く・追記・読む、mkdir -p、cd と相対のパス・pwd・..、ls
 *   2. 何ブロックにもまたがる大きなファイル（seq 1 60000、約 350KiB。間接ブロックを使う）と cp・cmp・切り詰め。
 *      小さな書き込み 60000 回が 20 秒以内（書き込みをためている）、sync のあと書き残しがゼロ
 *   3. mv（名前の変更・ディレクトリをまたぐ・上書き）、rm、rmdir（空でなければ断る）
 *   4. シンボリックリンク（cat でたどる・readlink）、ハードリンク（リンクの数）、chmod・stat、touch
 *   5. /data に置いたプログラム（busybox の写し）とスクリプト（#!/bin/sh）を動かす。cd /data で ./ からも
 *   6. 起動のたびに数える（電源を切っても残ることを、次の起動で確かめられる）。df
 *   終わったら、数えるファイルのほかは消す。QEMU を閉じたあと、ふつうの Linux の e2fsck で壊れていないことを確かめられる。
 *
 * 合格の目印: [M36] PASS
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
extern int rv_linux_proc_count(void);
extern void rv_console_set_test_input(const char *s);
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);

static const char script[] =
    "cd /data && echo pwd1=$(pwd)\n"
    "n=$(cat boot.count 2>/dev/null || echo 0); echo $((n+1)) > boot.count; echo boots=$(cat boot.count)\n"
    "rm -rf t; mkdir -p t/a/b && cd t/a && echo pwd2=$(pwd) && cd .. && echo pwd3=$(pwd)\n"
    "echo hello > a/b/f.txt; echo world >> a/b/f.txt; echo cat=$(cat a/b/f.txt | tr '\\n' ' ')\n"
    "echo list=$(ls a/b) root=$(ls / | grep -c '^data$')\n"
    "t0=$(date +%s); seq 1 60000 > big; t1=$(date +%s); echo big=$(wc -c < big) secs=$((t1-t0))\n"
    "cp big big2 && cmp big big2 && echo copy=$(echo same)\n"
    "dd if=big of=big2 bs=1000 count=1 conv=notrunc 2>/dev/null; cmp big big2 && echo dd=$(echo same)\n"
    "mv big2 a/moved && mv a/moved a/b/moved2 && echo mv=$(ls a/b | tr '\\n' ' ')\n"
    "echo old > o1; echo new > o2; mv o2 o1; echo over=$(cat o1)\n"
    "rmdir a 2>/dev/null || echo notempty=$(echo ok)\n"
    "ln -s b/f.txt a/sl && echo sl=$(cat a/sl | head -1) rl=$(readlink a/sl)\n"
    "ln -s /bin/busybox bbl && echo abs=$(readlink bbl)\n"
    "ln a/b/f.txt hard && echo links=$(stat -c %h hard)\n"
    "chmod 600 hard && echo mode=$(stat -c %a hard)\n"
    "touch -d '2020-01-02 03:04:05' o1 2>/dev/null; echo tyear=$(stat -c %y o1 | cut -c1-4)\n"
    "cp /bin/busybox ./busybox && chmod 755 busybox && ./busybox echo exec=$((40+2))\n"
    "printf '#!/bin/sh\\necho script=$((1+1)) $1\\n' > s.sh && chmod +x s.sh && ./s.sh arg && /data/t/s.sh abs\n"
    "truncate -s 100 big 2>/dev/null || : > big; echo trunc=$(wc -c < big)\n"
    "cd /data && rm -rf t && echo left=$(ls /data | grep -c '^t$') count=$(ls /data | grep -c '^boot.count$')\n"
    "echo statfs=$(stat -f -c %b /data)\n"
    "echo year=$(date +%Y)\n"
    "sync && echo synced=$(echo ok)\n"
    "exit 0\n";

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M36] OK   " : "[M36] NG   ");
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

int m36_run_data_test(void)
{
    static const char *const argv[] = { "sh", 0 };
    static const char *const envp[] = { "HOME=/", "PATH=/bin:/", "TERM=vt100", 0 };
    struct tarfs_file f;
    struct rv_uproc *p;
    const char *out, *b;
    long r;
    int pass = 1;

    uart_puts("\r\n[M36] writable disk /data (ext2) test\r\n");
    if (!ext2_mounted()) {
        uart_puts("[M36] SKIP (no /data: QEMU without the second disk)\r\n");
        return 1;
    }
    if (tarfs_find("busybox.elf", &f) != 1) {
        uart_puts("[M36] SKIP (no busybox)\r\n");
        return 1;
    }
    p = rv_linux_load_path("busybox.elf", argv, envp, &r);
    if (p == NULL) {
        uart_puts("[M36] load failed\r\n[M36] FAIL\r\n");
        return 0;
    }
    rv_console_set_test_input(script);
    rv_console_capture(1);
    r = rv_linux_run_process(p);
    rv_console_capture(0);
    rv_console_set_test_input(0);
    out = rv_console_captured();

    pass &= check(contains(out, "pwd1=/data") && contains(out, "pwd2=/data/t/a") && contains(out, "pwd3=/data/t")
                  && contains(out, "cat=hello world") && contains(out, "list=f.txt root=1"),
                  "write, append, read, mkdir -p, cd and relative paths, pwd, .., ls (data in the root listing)");
    pass &= check(contains(out, "big=348894 secs=") && contains(out, "copy=same\n") && contains(out, "dd=same\n"),
                  "big file over many blocks (seq 1 60000), cp and cmp, dd conv=notrunc");
    pass &= check(contains(out, "mv=f.txt moved2") && contains(out, "over=new") && contains(out, "notempty=ok\n"),
                  "mv (rename, across folders, overwrite), rmdir refuses a non-empty folder");
    pass &= check(contains(out, "sl=hello rl=b/f.txt") && contains(out, "abs=/bin/busybox")
                  && contains(out, "links=2") && contains(out, "mode=600") && contains(out, "tyear=2020"),
                  "symbolic link (cat through it, readlink), hard link (link count), chmod and stat, touch -d");
    pass &= check(contains(out, "exec=42") && contains(out, "script=2 arg") && contains(out, "script=2 abs"),
                  "run a program and a #! script placed on /data (./ and absolute path)");
    pass &= check(contains(out, "trunc=100") && contains(out, "left=0 count=1") && contains(out, "statfs=16384"),
                  "truncate, rm -rf (the test folder is gone, the boot counter stays), statfs (stat -f)");
    b = out;
    while (*b && !(b[0] == 'b' && b[1] == 'o' && b[2] == 'o' && b[3] == 't' && b[4] == 's' && b[5] == '='
                   && b[6] >= '0' && b[6] <= '9')) {
        b++;
    }
    uart_puts("[M36]   boot count on /data: ");
    if (*b) {
        char n[12];
        int i;
        for (i = 0; i < 11 && b[6 + i] >= '0' && b[6 + i] <= '9'; i++) {
            n[i] = b[6 + i];
        }
        n[i] = 0;
        uart_puts(n);
    }
    uart_puts(" (grows on every boot: the disk keeps its contents)\r\n");
    pass &= check(*b != 0 && !contains(out, "year=1970"), "boot counter kept on the disk; date comes from the RTC");
    b = out;
    while (*b && !(b[0] == 's' && b[1] == 'e' && b[2] == 'c' && b[3] == 's' && b[4] == '=' && b[5] >= '0' && b[5] <= '9')) {
        b++;
    }
    {
        int secs = 0, i;
        for (i = 5; b[0] && b[i] >= '0' && b[i] <= '9'; i++) {
            secs = secs * 10 + (b[i] - '0');
        }
        uart_puts("[M36]   seq 1 60000 > big took about ");
        {
            char n[12];
            int k = 11, v = secs;
            n[k] = 0;
            do { n[--k] = (char)('0' + v % 10); v /= 10; } while (v);
            uart_puts(n + k);
        }
        uart_puts(" s (60000 small writes)\r\n");
        pass &= check(*b != 0 && secs <= 20, "small writes are buffered (seq 1 60000 within 20 s)");
    }
    pass &= check(contains(out, "synced=ok") && ext2_dirty_blocks() == 0, "sync writes everything out (no dirty blocks left)");
    pass &= check(r == 0 && rv_linux_proc_count() == 0, "exit 0, process table empty");
    uart_puts(pass ? "[M36] PASS\r\n" : "[M36] FAIL\r\n");
    return pass;
}
