/* kernel/tk/m31_usbkbd_test.c — M31: USB のキーボード（HID）（SMODE=1 かつ MMU=1）
 *
 *   1. usb-kbd の設定の記述子から HID のブート用キーボードを見つけ、割込みの IN の口を開いた
 *      （装置の表で、その口の状態が Running）
 *   2. 報告から文字への変換（JIS 配列・シフト・Ctrl・矢印・押したままの重なり）が正しい
 *   本物のキー入力は、QEMU の窓で打つか、QEMU のモニターの sendkey で確かめる。
 *
 * 合格の目印: [M31] PASS
 */
#include <stdint.h>
#include <stddef.h>

extern void uart_puts(const char *s);
extern int rv_usb_present(void);
extern int rv_usb_kbd_on(void);
extern int rv_usb_kbd_running(void);
extern const char *rv_usb_kbd_selftest(void);

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M31] OK   " : "[M31] NG   ");
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

int m31_run_usbkbd_test(void)
{
    int pass = 1;
    uart_puts("\r\n[M31] USB keyboard (HID) test\r\n");
    if (!rv_usb_present()) {
        uart_puts("[M31] SKIP (no xHCI)\r\n");
        return 1;
    }
    pass &= check(rv_usb_kbd_on() && rv_usb_kbd_running(),
                  "HID boot keyboard found, configured, interrupt IN endpoint running");
    pass &= check(same(rv_usb_kbd_selftest(), "aB\"@:_\003\033[A\r"),
                  "report to characters: JIS layout, Shift, Ctrl, arrow, keys held together");
    uart_puts(pass ? "[M31] PASS\r\n" : "[M31] FAIL\r\n");
    return pass;
}
