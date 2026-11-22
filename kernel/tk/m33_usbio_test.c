/* kernel/tk/m33_usbio_test.c — M33: USB の I/O ボード（USB シリアル + Firmata）（SMODE=1 かつ MMU=1）
 *
 *   QEMU の usb-serial（FTDI）に、tools/firmata_sim.py（Arduino Uno の形の Firmata のボードの模擬）がつながっている。
 *   1. USB シリアルを見つけ、バルクの IN・OUT の口を開いた（Running）
 *   2. Firmata の問い合わせに答えが来て、ボード（20 ピン）が「usbio」として入出力の管理に加わった
 *   3. 名前の口（/etc/io.conf）で: BLED（ピン 13）を 1 → 線でつないだ BBTN（ピン 2）が 1、0 に戻すと 0
 *   4. BKNOB（A0 = 600、scale 0.09775）が 58.65%
 *   5. BDIM（ピン 3 の PWM）50% → 線でつないだ A1（BSENSE1）が 513 前後
 *   6. BSERVO（サーボ 9）を毎秒 180 度で 90 度へ: 0.1 秒後はまだ途中、0.8 秒後に 90 度（A2 も 511 前後）。上限 180 度を超えると断る
 *   7. 非常停止で、ピン 13 と PWM が 0 に戻る。終わったら「まだ動かさない」に戻す
 * python3 のない環境では usb-serial を付けないので SKIP する。
 *
 * 合格の目印: [M33] PASS
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>

extern void uart_puts(const char *s);
extern int rv_usb_present(void);
extern int rv_usbser_on(void);
extern int rv_usbser_running(void);
extern int rv_firmata_ready(void);
extern int rv_firmata_pins(void);
extern const char *rv_firmata_name(void);
extern int rv_io_has_backend(const char *name);
extern int rv_io_get_named(const char *name, int64_t *v);
extern int rv_io_set_named(const char *name, int64_t v, int32_t speed);
extern void rv_io_release(void);
extern void rv_io_stop(int why);

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M33] OK   " : "[M33] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

static int64_t get(const char *n)
{
    int64_t v = -999999;
    if (rv_io_get_named(n, &v) != 0) {
        return -999999;
    }
    return v;
}

static int near(int64_t v, int64_t want, int64_t tol)
{
    return v >= want - tol && v <= want + tol;
}

int m33_run_usbio_test(void)
{
    int pass = 1, i;
    int64_t a, b, s1, s2;

    uart_puts("\r\n[M33] USB I/O board (USB serial + Firmata) test\r\n");
    if (!rv_usb_present() || !rv_usbser_on()) {
        uart_puts("[M33] SKIP (no USB serial: QEMU without usb-serial)\r\n");
        return 1;
    }
    pass &= check(rv_usbser_running(), "USB serial (FTDI): bulk IN and OUT endpoints running");
    for (i = 0; i < 500 && !rv_firmata_ready(); i++) {
        tk_dly_tsk(10);
    }
    pass &= check(rv_firmata_ready() && rv_firmata_pins() == 20 && rv_io_has_backend("usbio"),
                  "Firmata board answered (version, capability, analog map): 20 pins, joined as usbio");
    uart_puts("[M33]   board: ");
    uart_puts(rv_firmata_name());
    uart_puts("\r\n");
    if (!rv_firmata_ready()) {
        uart_puts("[M33] FAIL\r\n");
        return 0;
    }
    tk_dly_tsk(100);
    rv_io_release();

    rv_io_set_named("BLED", 1000, 0);
    tk_dly_tsk(150);
    a = get("BBTN");
    rv_io_set_named("BLED", 0, 0);
    tk_dly_tsk(150);
    b = get("BBTN");
    pass &= check(a == 1000 && b == 0, "digital out (BLED, pin 13) and in (BBTN, pin 2, wired to pin 13)");

    pass &= check(near(get("BKNOB"), 58650, 60), "analog in with scale (BKNOB = A0 600 x 0.09775 = 58.65%)");

    rv_io_set_named("BDIM", 50000, 0);
    tk_dly_tsk(150);
    pass &= check(get("BDIM") == 50000 && near(get("BSENSE1"), 513000, 3000),
                  "PWM 50% on pin 3 (BDIM) read back on A1 (BSENSE1, about 513)");

    rv_io_set_named("BSERVO", 90000, 180);
    tk_dly_tsk(100);
    s1 = get("BSERVO");
    tk_dly_tsk(700);
    s2 = get("BSERVO");
    pass &= check(s1 > 0 && s1 < 60000 && s2 == 90000 && near(get("BSENSE2"), 511000, 4000)
                  && rv_io_set_named("BSERVO", 200000, 0) != 0,
                  "servo 9 moves at 180 deg/s to 90 (A2 about 511); 200 deg refused (io.conf max 180)");

    rv_io_set_named("BLED", 1000, 0);
    tk_dly_tsk(100);
    rv_io_stop(1);
    tk_dly_tsk(150);
    pass &= check(get("BLED") == 0 && get("BBTN") == 0 && get("BDIM") == 0 && near(get("BSENSE1"), 0, 1000)
                  && rv_io_set_named("BLED", 1000, 0) != 0,
                  "emergency stop: pin 13 and PWM back to 0, outputs refused");
    rv_io_stop(3);                  /* 起動のときと同じ「まだ動かさない」に戻す */

    uart_puts(pass ? "[M33] PASS\r\n" : "[M33] FAIL\r\n");
    return pass;
}
