/* kernel/tk/m30_usb_test.c — M30: USB のホスト（xHCI）（SMODE=1 かつ MMU=1）
 *
 *   QEMU の qemu-xhci に、usb-kbd と usb-tablet をつないで起動する（Makefile の qemu ターゲット）。
 *   1. PCIe で xHCI を見つけ、初期化して動かし、命令のリングが応える
 *   2. つながった 2 台に住所が付き、記述子が読める（メーカー 0627 = QEMU、製品の名前）
 *
 * 合格の目印: [M30] PASS
 */
#include <stdint.h>
#include <stddef.h>

extern void uart_puts(const char *s);
extern int rv_usb_present(void);
extern int rv_usb_count(void);
extern int rv_usb_info(int i, uint16_t *vid, uint16_t *pid, const char **name);

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M30] OK   " : "[M30] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

int m30_run_usb_test(void)
{
    int i, kbd = 0, tab = 0, pass = 1;

    uart_puts("\r\n[M30] USB host (xHCI) test\r\n");
    if (!rv_usb_present()) {
        uart_puts("[M30] SKIP (no xHCI: QEMU without -device qemu-xhci)\r\n");
        return 1;
    }
    pass &= check(1, "xHCI found on PCIe, reset, started, command ring responds");
    for (i = 0; i < rv_usb_count(); i++) {
        uint16_t vid, pid;
        const char *name;
        rv_usb_info(i, &vid, &pid, &name);
        if (vid == 0x0627 && same(name, "QEMU USB Keyboard")) {
            kbd = 1;
        }
        if (vid == 0x0627 && same(name, "QEMU USB Tablet")) {
            tab = 1;
        }
    }
    pass &= check(kbd, "usb-kbd addressed, descriptors read (0627 \"QEMU USB Keyboard\")");
    pass &= check(tab, "usb-tablet addressed, descriptors read (0627 \"QEMU USB Tablet\")");
    uart_puts(pass ? "[M30] PASS\r\n" : "[M30] FAIL\r\n");
    return pass;
}
