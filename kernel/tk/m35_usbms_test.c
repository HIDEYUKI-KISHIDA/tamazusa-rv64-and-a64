/* kernel/tk/m35_usbms_test.c — M35: USB メモリ（Bulk-Only Transport・SCSI）（SMODE=1 かつ MMU=1）
 *
 *   QEMU の usb-storage に、16MiB のイメージ（build/usb.img。頭に "TAMAZUSA USB MEMORY TEST IMAGE"）をつないでいる。
 *   1. USB メモリを見つけ、バルクの口を開き、INQUIRY・TEST UNIT READY・READ CAPACITY が通った: 16MiB・512 バイトのブロック
 *   2. 頭のブロックを読める（目印の文字）
 *   3. ブロックの途中から 70000 バイト（64KiB の区切りをまたぐ）を書き、読み戻して同じ。前後のバイトは変わらない
 *   4. busybox の sh から /dev/sda: stat（ブロック装置・大きさ）、head で読む、printf で書いて読み戻し、元に戻す
 * 書いた所は元の中身に戻す（イメージは次の起動でも使う）。
 *
 * 合格の目印: [M35] PASS
 */
#include <stdint.h>
#include <stddef.h>
#include "../fs/tarfs.h"

extern void uart_puts(const char *s);
extern int rv_usb_present(void);
extern int rv_usbms_present(void);
extern int rv_usbms_running(void);
extern uint64_t rv_usbms_size(void);
extern uint32_t rv_usbms_block(void);
extern const char *rv_usbms_name(void);
extern long rv_usbms_rw(uint64_t off, uintptr_t buf, size_t len, int write, int user);
struct rv_uproc;
extern struct rv_uproc *rv_linux_load_path(const char *path, const char *const argv[],
                                           const char *const envp[], long *err);
extern long rv_linux_run_process(struct rv_uproc *p);
extern void rv_console_set_test_input(const char *s);
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);

#define N 70000
#define OFF (2u * 1024 * 1024 + 300)
static uint8_t keep[N + 2], pat[N], back[N + 2];

static const char script[] =
    "stat -c 'type=%F size=%s' /dev/sda\n"
    "echo head=$(head -c 14 /dev/sda)\n"
    "printf 'SHELL WROTE IT' > /dev/sda; echo back=$(head -c 14 /dev/sda)\n"
    "printf 'TAMAZUSA USB M' > /dev/sda; echo again=$(head -c 14 /dev/sda)\n"
    "exit 0\n";

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M35] OK   " : "[M35] NG   ");
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

int m35_run_usbms_test(void)
{
    static const char *const argv[] = { "sh", 0 };
    static const char *const envp[] = { "HOME=/", "PATH=/bin:/", "TERM=vt100", 0 };
    static uint8_t blk0[512];
    struct tarfs_file f;
    struct rv_uproc *p;
    const char *out;
    int pass = 1, i, same = 1, edges;
    long r1, r2, r3, r;

    uart_puts("\r\n[M35] USB memory (Bulk-Only Transport, SCSI) test\r\n");
    if (!rv_usb_present() || !rv_usbms_present()) {
        uart_puts("[M35] SKIP (no USB memory: QEMU without usb-storage)\r\n");
        return 1;
    }
    pass &= check(rv_usbms_running() && rv_usbms_size() == 16u * 1024 * 1024 && rv_usbms_block() == 512,
                  "found, bulk endpoints running, INQUIRY / TEST UNIT READY / READ CAPACITY: 16 MiB, 512-byte blocks");
    uart_puts("[M35]   device: ");
    uart_puts(rv_usbms_name());
    uart_puts("\r\n");

    r = rv_usbms_rw(0, (uintptr_t)blk0, 512, 0, 0);
    pass &= check(r == 512 && blk0[0] == 'T' && blk0[1] == 'A' && blk0[8] == ' ' && blk0[9] == 'U',
                  "read block 0 (marker \"TAMAZUSA USB ...\")");

    for (i = 0; i < N; i++) {
        pat[i] = (uint8_t)(i * 7 + (i >> 8));
    }
    r1 = rv_usbms_rw(OFF - 1, (uintptr_t)keep, N + 2, 0, 0);            /* 元の中身（前後 1 バイトずつも） */
    r2 = rv_usbms_rw(OFF, (uintptr_t)pat, N, 1, 0);
    r3 = rv_usbms_rw(OFF - 1, (uintptr_t)back, N + 2, 0, 0);
    for (i = 0; i < N; i++) {
        if (back[i + 1] != pat[i]) {
            same = 0;
            break;
        }
    }
    edges = back[0] == keep[0] && back[N + 1] == keep[N + 1];
    rv_usbms_rw(OFF - 1, (uintptr_t)keep, N + 2, 1, 0);                 /* 元に戻す */
    pass &= check(r1 == N + 2 && r2 == N && r3 == N + 2 && same && edges,
                  "write 70000 bytes from the middle of a block (across 64 KiB), read back; bytes around unchanged");

    if (tarfs_find("busybox.elf", &f) != 1) {
        uart_puts("[M35] SKIP shell part (no busybox)\r\n");
    } else {
        p = rv_linux_load_path("busybox.elf", argv, envp, &r);
        if (p) {
            rv_console_set_test_input(script);
            rv_console_capture(1);
            r = rv_linux_run_process(p);
            rv_console_capture(0);
            rv_console_set_test_input(0);
            out = rv_console_captured();
            pass &= check(contains(out, "type=block special file size=16777216") && contains(out, "head=TAMAZUSA USB")
                          && contains(out, "back=SHELL WROTE IT") && contains(out, "again=TAMAZUSA USB M") && r == 0,
                          "busybox sh: stat /dev/sda (block device, size), head, printf > /dev/sda and read back");
        } else {
            pass &= check(0, "busybox sh");
        }
    }
    uart_puts(pass ? "[M35] PASS\r\n" : "[M35] FAIL\r\n");
    return pass;
}
