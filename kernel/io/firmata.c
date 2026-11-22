/* kernel/io/firmata.c — USB の I/O ボード（Firmata）の中身「usbio」
 *
 *   Firmata は、マイコンのボード（Arduino・Raspberry Pi Pico など）を、シリアル越しにピンの単位で動かすための、
 *   広く使われている決まり（MIDI の形の短い通信文）。ボードに StandardFirmata を書き込んでおけば、
 *   どのボードでも同じ通信文で動く。ここでは USB シリアル（kernel/usb/xhci.c）の上で、その通信文を話す。
 *
 *   つながると: 版・ファームウェア・できること（capability）・アナログの対応表を問い合わせる。
 *   答え（できること）が来たら、入出力の管理に「usbio」として加わり、/etc/io.conf の usbio.* のピンの向きを決める。
 *     usbio.pinN   デジタル（output は出す、input は読む。変わったら報告させる）
 *     usbio.adcN   アナログの入力 AN（数え値。scale で物の値にする）
 *     usbio.pwmN   ピン N の PWM（0〜100% を、ボードの分解能の値にする）
 *     usbio.servoN ピン N のサーボ（0〜180 度）。速さを言われたら、usb_task が 8 ミリ秒ごとに少しずつ動かす
 *   止めるとき: 出力のピンを 0、PWM を 0、サーボはその場（今の角度）で止める。
 */
#include <stdint.h>
#include <stddef.h>
#define IOMGR_TYPES_ONLY
#include "iomgr_core.h"
#include <rv_priv.h>

#define LOCK(st)   ((st) = RV_IRQ_SAVE())
#define UNLOCK(st) RV_IRQ_RESTORE(st)

extern void uart_puts(const char *s);
extern int rv_usbser_on(void);
extern int rv_usbser_write(const uint8_t *p, int n);
extern void (*rv_usbser_rx)(const uint8_t *p, int n);
extern void (*rv_usbser_tick)(void);
extern void iomgr_add_backend(IoBackend *b);

#define NPIN 128

/* Firmata の通信文 */
#define F_ANALOG_MSG    0xe0
#define F_DIGITAL_MSG   0x90
#define F_REPORT_ANALOG 0xc0
#define F_REPORT_DIGITAL 0xd0
#define F_SET_PIN_MODE  0xf4
#define F_VERSION       0xf9
#define F_SYSEX         0xf0
#define F_SYSEX_END     0xf7
#define S_EXT_ANALOG    0x6f
#define S_CAP_QUERY     0x6b
#define S_CAP_RESPONSE  0x6c
#define S_MAP_QUERY     0x69
#define S_MAP_RESPONSE  0x6a
#define S_SERVO_CONFIG  0x70
#define S_FIRMWARE      0x79
#define S_SAMPLING      0x7a
#define M_INPUT 0
#define M_OUTPUT 1
#define M_ANALOG 2
#define M_PWM 3
#define M_SERVO 4

static struct {
    int stage;                  /* 0 待ち 1 問い合わせた 2 使える */
    int wait;                   /* 問い合わせからの見回りの回数 */
    int major, minor;
    char fw[48];
    uint8_t caps[NPIN];         /* できることのビット（1 << モード） */
    uint8_t pwm_res[NPIN];
    uint8_t amap[NPIN];         /* ピン → アナログの番号（0x7f = なし） */
    int npins;
    uint8_t mode[NPIN];         /* 決めた向き（0xff = まだ） */
    uint16_t analog[16];
    uint8_t port_in[16];        /* デジタル入力のポート（8 ピンずつ） */
    uint8_t port_out[16];
    int64_t value_m[NPIN];      /* 出した値（PWM は %、サーボは度、ミリ単位） */
    int64_t servo_cur[NPIN];    /* サーボの今の角度（ミリ度）・目標・速さ */
    int64_t servo_tgt[NPIN];
    int32_t servo_spd[NPIN];
    /* 受けている途中の通信文 */
    uint8_t msg[256];
    int mlen, msys, mneed;
} fm;

static IoBackend fm_be;
volatile unsigned long rv_firmata_msgs;

static void send(const uint8_t *p, int n)
{
    rv_usbser_write(p, n);
}

static void send3(uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t m[3] = { a, b, c };
    send(m, 3);
}

static void set_mode(int pin, int mode)
{
    if (pin < 0 || pin >= NPIN || fm.mode[pin] == mode) {
        return;
    }
    if (mode == M_SERVO) {      /* パルス幅 544〜2400 マイクロ秒（Arduino の Servo と同じ） */
        uint8_t m[8] = { F_SYSEX, S_SERVO_CONFIG, (uint8_t)pin, 544 & 0x7f, 544 >> 7, 2400 & 0x7f, 2400 >> 7, F_SYSEX_END };
        send(m, 8);
    }
    send3(F_SET_PIN_MODE, (uint8_t)pin, (uint8_t)mode);
    fm.mode[pin] = (uint8_t)mode;
    if (mode == M_INPUT) {
        uint8_t m[2] = { (uint8_t)(F_REPORT_DIGITAL | ((pin >> 3) & 15)), 1 };
        send(m, 2);
    } else if (mode == M_ANALOG && fm.amap[pin] != 0x7f) {
        uint8_t m[2] = { (uint8_t)(F_REPORT_ANALOG | (fm.amap[pin] & 15)), 1 };
        send(m, 2);
    }
}

static void analog_write(int pin, unsigned v)
{
    if (pin < 16) {
        send3((uint8_t)(F_ANALOG_MSG | pin), v & 0x7f, (v >> 7) & 0x7f);
    } else {
        uint8_t m[7] = { F_SYSEX, S_EXT_ANALOG, (uint8_t)pin, v & 0x7f, (v >> 7) & 0x7f, (v >> 14) & 0x7f, F_SYSEX_END };
        send(m, 7);
    }
}

static void digital_write(int pin, int v)
{
    int port = (pin >> 3) & 15;
    if (v) {
        fm.port_out[port] |= (uint8_t)(1 << (pin & 7));
    } else {
        fm.port_out[port] &= (uint8_t)~(1 << (pin & 7));
    }
    send3((uint8_t)(F_DIGITAL_MSG | port), fm.port_out[port] & 0x7f, (fm.port_out[port] >> 7) & 1);
}

/* アナログの番号 → ピンの番号 */
static int analog_pin(int ch)
{
    int i;
    for (i = 0; i < fm.npins; i++) {
        if (fm.amap[i] == ch) {
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- 中身の口 */
static int fm_get(void *ctx, int kind, int chan, int64_t *raw_m)
{
    (void)ctx;
    if (fm.stage < 2) {
        return IO_ENODEV;
    }
    switch (kind) {
    case IO_K_ADC:
        if (chan < 0 || chan > 15 || analog_pin(chan) < 0) {
            return IO_ENODEV;
        }
        *raw_m = (int64_t)fm.analog[chan] * 1000;
        return IO_OK;
    case IO_K_PIN:
        if (chan < 0 || chan >= fm.npins) {
            return IO_ENODEV;
        }
        if (fm.mode[chan] == M_INPUT) {
            *raw_m = ((fm.port_in[(chan >> 3) & 15] >> (chan & 7)) & 1) * 1000;
        } else {
            *raw_m = ((fm.port_out[(chan >> 3) & 15] >> (chan & 7)) & 1) * 1000;
        }
        return IO_OK;
    case IO_K_PWM:
        if (chan < 0 || chan >= fm.npins) {
            return IO_ENODEV;
        }
        *raw_m = fm.value_m[chan];
        return IO_OK;
    case IO_K_SERVO:
        if (chan < 0 || chan >= fm.npins) {
            return IO_ENODEV;
        }
        *raw_m = fm.servo_cur[chan];
        return IO_OK;
    }
    return IO_EKIND;
}

static int fm_set(void *ctx, int kind, int chan, int64_t raw_m, int32_t speed)
{
    (void)ctx;
    if (fm.stage < 2) {
        return IO_ENODEV;
    }
    if (chan < 0 || chan >= fm.npins) {
        return IO_ENODEV;
    }
    switch (kind) {
    case IO_K_PIN:
        if (!(fm.caps[chan] & (1 << M_OUTPUT))) {
            return IO_ENODEV;
        }
        set_mode(chan, M_OUTPUT);
        digital_write(chan, raw_m != 0);
        return IO_OK;
    case IO_K_PWM: {
        unsigned maxv = (1u << (fm.pwm_res[chan] ? fm.pwm_res[chan] : 8)) - 1;
        if (!(fm.caps[chan] & (1 << M_PWM))) {
            return IO_ENODEV;
        }
        if (raw_m < 0 || raw_m > 100000) {
            return IO_ERANGE;
        }
        set_mode(chan, M_PWM);
        fm.value_m[chan] = raw_m;
        analog_write(chan, (unsigned)((raw_m * maxv + 50000) / 100000));
        return IO_OK;
    }
    case IO_K_SERVO:
        if (!(fm.caps[chan] & (1 << M_SERVO))) {
            return IO_ENODEV;
        }
        if (raw_m < 0 || raw_m > 180000) {
            return IO_ERANGE;
        }
        set_mode(chan, M_SERVO);
        fm.servo_tgt[chan] = raw_m;
        fm.servo_spd[chan] = speed;
        if (speed <= 0) {                           /* 速さの指定なし: すぐ */
            fm.servo_cur[chan] = raw_m;
            analog_write(chan, (unsigned)((raw_m + 500) / 1000));
        }
        return IO_OK;
    }
    return IO_EKIND;
}

static void fm_setup(void *ctx, int kind, int chan, int output)
{
    (void)ctx;
    if (chan < 0 || chan >= fm.npins) {
        return;
    }
    if (kind == IO_K_PIN) {
        set_mode(chan, output ? M_OUTPUT : M_INPUT);
    } else if (kind == IO_K_ADC) {
        int pin = analog_pin(chan);
        if (pin >= 0) {
            set_mode(pin, M_ANALOG);
        }
    }
    /* PWM・サーボは、初めて出すときに向きを決める（決めたとたんに動かないように） */
}

static void fm_safe(void *ctx)
{
    int i;
    (void)ctx;
    if (fm.stage < 2) {
        return;
    }
    for (i = 0; i < fm.npins; i++) {
        if (fm.mode[i] == M_OUTPUT && ((fm.port_out[(i >> 3) & 15] >> (i & 7)) & 1)) {
            digital_write(i, 0);
        } else if (fm.mode[i] == M_PWM && fm.value_m[i]) {
            fm.value_m[i] = 0;
            analog_write(i, 0);
        } else if (fm.mode[i] == M_SERVO) {
            fm.servo_tgt[i] = fm.servo_cur[i];      /* その場で止める */
        }
    }
}

/* ---------------------------------------------------------------- 受けた通信文 */
static void sysex(const uint8_t *m, int n)
{
    int i, pin;
    if (n < 1) {
        return;
    }
    switch (m[0]) {
    case S_FIRMWARE:
        if (n >= 3) {
            int k = 0;
            fm.major = m[1];
            fm.minor = m[2];
            for (i = 3; i + 1 < n && k < (int)sizeof(fm.fw) - 1; i += 2) {
                fm.fw[k++] = (char)(m[i] | (m[i + 1] << 7));
            }
            fm.fw[k] = 0;
        }
        break;
    case S_CAP_RESPONSE:
        pin = 0;
        for (i = 1; i < n && pin < NPIN; ) {
            if (m[i] == 0x7f) {
                pin++;
                i++;
                continue;
            }
            if (i + 1 < n) {
                if (m[i] < 8) {
                    fm.caps[pin] |= (uint8_t)(1 << m[i]);
                }
                if (m[i] == M_PWM) {
                    fm.pwm_res[pin] = m[i + 1];
                }
            }
            i += 2;
        }
        fm.npins = pin;
        break;
    case S_MAP_RESPONSE:
        for (i = 1; i < n && i - 1 < NPIN; i++) {
            fm.amap[i - 1] = m[i];
        }
        break;
    }
}

static void rx(const uint8_t *p, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (fm.msys) {
            if (c == F_SYSEX_END) {
                sysex(fm.msg, fm.mlen);
                rv_firmata_msgs++;
                fm.msys = 0;
                fm.mlen = 0;
            } else if (fm.mlen < (int)sizeof(fm.msg)) {
                fm.msg[fm.mlen++] = c;
            }
            continue;
        }
        if (c & 0x80) {
            fm.mlen = 0;
            if (c == F_SYSEX) {
                fm.msys = 1;
                continue;
            }
            fm.msg[fm.mlen++] = c;
            fm.mneed = ((c & 0xf0) == F_ANALOG_MSG || (c & 0xf0) == F_DIGITAL_MSG || c == F_VERSION) ? 3 : 0;
            continue;
        }
        if (fm.mneed && fm.mlen > 0 && fm.mlen < 3) {
            fm.msg[fm.mlen++] = c;
            if (fm.mlen == fm.mneed) {
                uint8_t cmd = fm.msg[0];
                unsigned v = fm.msg[1] | (fm.msg[2] << 7);
                if ((cmd & 0xf0) == F_ANALOG_MSG) {
                    fm.analog[cmd & 15] = (uint16_t)v;
                } else if ((cmd & 0xf0) == F_DIGITAL_MSG) {
                    fm.port_in[cmd & 15] = (uint8_t)v;
                } else if (cmd == F_VERSION) {
                    fm.major = fm.msg[1];
                    fm.minor = fm.msg[2];
                }
                rv_firmata_msgs++;
                fm.mlen = 0;
            }
        }
    }
}

/* usb_task から 8 ミリ秒ごと */
static void tick(void)
{
    int i;
    unsigned long st;
    if (!rv_usbser_on()) {
        return;
    }
    if (fm.stage == 0) {
        static const uint8_t q[] = {
            F_VERSION,
            F_SYSEX, S_FIRMWARE, F_SYSEX_END,
            F_SYSEX, S_CAP_QUERY, F_SYSEX_END,
            F_SYSEX, S_MAP_QUERY, F_SYSEX_END,
            F_SYSEX, S_SAMPLING, 20, 0, F_SYSEX_END     /* 20 ミリ秒ごとにアナログを報告 */
        };
        send(q, sizeof(q));
        fm.stage = 1;
        fm.wait = 0;
        return;
    }
    if (fm.stage == 1) {
        if (fm.npins > 0) {
            fm.stage = 2;
            uart_puts("[usbio] Firmata board ready: ");
            uart_puts(fm.fw[0] ? fm.fw : "(no name)");
            uart_puts("\r\n");
            iomgr_add_backend(&fm_be);
        } else if (++fm.wait == 375) {              /* 3 秒: Arduino は DTR で再起動するので、もう一度問い合わせる */
            fm.stage = 0;
        }
        return;
    }
    LOCK(st);
    for (i = 0; i < fm.npins; i++) {                /* 速さを決めたサーボを、少しずつ目標へ */
        if (fm.mode[i] == M_SERVO && fm.servo_cur[i] != fm.servo_tgt[i]) {
            int64_t step = fm.servo_spd[i] > 0 ? (int64_t)fm.servo_spd[i] * 8 : 180000;
            int64_t d = fm.servo_tgt[i] - fm.servo_cur[i];
            fm.servo_cur[i] += d > step ? step : d < -step ? -step : d;
            analog_write(i, (unsigned)((fm.servo_cur[i] + 500) / 1000));
        }
    }
    UNLOCK(st);
}

void firmata_init(void)
{
    int i;
    for (i = 0; i < NPIN; i++) {
        fm.mode[i] = 0xff;
        fm.amap[i] = 0x7f;
    }
    fm_be.name = "usbio";
    fm_be.ctx = NULL;
    fm_be.get = fm_get;
    fm_be.set = fm_set;
    fm_be.setup = fm_setup;
    fm_be.safe = fm_safe;
    fm_be.tick = NULL;                      /* サーボの速さは usb_task の見回りで */
    rv_usbser_rx = rx;
    rv_usbser_tick = tick;
}

/* テスト用 */
int rv_firmata_ready(void)
{
    return fm.stage == 2;
}

int rv_firmata_pins(void)
{
    return fm.npins;
}

const char *rv_firmata_name(void)
{
    return fm.fw;
}
