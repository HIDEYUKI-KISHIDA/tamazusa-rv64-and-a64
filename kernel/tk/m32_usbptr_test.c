/* kernel/tk/m32_usbptr_test.c — M32: USB のマウス・タブレット（HID）（SMODE=1 かつ MMU=1）
 *
 *   1. usb-tablet の報告の記述子を読んで X・Y・ボタンを見つけ、割込みの IN の口を開いた（Running、絶対位置）
 *   2. 報告の記述子の読み取りと、位置への換算（0〜32767 → 画面全体 1024×768）が、作った記述子と報告で正しい
 *   本物の動きは、QEMU の窓でマウスを動かすか、QEMU のモニターの mouse_move・mouse_button で確かめる。
 *
 * 合格の目印: [M32] PASS
 */
#include <stdint.h>
#include <stddef.h>

extern void uart_puts(const char *s);
extern int rv_usb_present(void);
extern int rv_usb_ptr_on(void);
extern int rv_usb_ptr_running(void);
extern int rv_usb_ptr_absolute(void);
extern int rv_usb_ptr_selftest(void);

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M32] OK   " : "[M32] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

int m32_run_usbptr_test(void)
{
    int pass = 1;
    uart_puts("\r\n[M32] USB mouse / tablet (HID) test\r\n");
    if (!rv_usb_present()) {
        uart_puts("[M32] SKIP (no xHCI)\r\n");
        return 1;
    }
    pass &= check(rv_usb_ptr_on() && rv_usb_ptr_running() && rv_usb_ptr_absolute(),
                  "tablet found, report descriptor parsed, interrupt IN endpoint running (absolute)");
    pass &= check(rv_usb_ptr_selftest(),
                  "report descriptor parser and position (0..32767 -> 1024x768 desktop), buttons, wheel");
    uart_puts(pass ? "[M32] PASS\r\n" : "[M32] FAIL\r\n");
    return pass;
}
