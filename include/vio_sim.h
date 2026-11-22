/* vio_sim.h — 仮想の機器（センサー・アクチュエータ・サーボ・安全装置）
 *
 *   カーネルの /dev/io と、ふつうの Linux で T88-BASIC を試すときの両方が、この同じ計算を使う。
 *   整数だけで計算する（カーネルでは浮動小数点を使わない）。値はミリ単位で持つ。
 *   時間は呼ぶ側が渡す（ミリ秒）。読み書きのたびに、前回からの時間だけ状態を進める。
 *
 *   ポート（BASIC の INP・OUT では &HE000 を足した番号。/dev/io では番号 × 4 の位置に 32 ビット）:
 *     00〜08  センサー（読む）。書くと、その値に置き換える（試験や授業の「もしも」に使う）
 *       00 温度（0.1℃）   01 湿度（%）   02 水位（mm）   03 距離（mm）   04 明るさ（lx）
 *       05 音（dB）       06 触覚（0/1） 07 におい・CO2（ppm）           08 水質 pH（0.1）
 *     10〜14  アクチュエータ（0〜100%。読むと今の値）
 *       10 ヒーター  11 ファン  12 ポンプ  13 照明  14 モーター（搬送）
 *     20      デジタル出力（16 ビット。信号機・LED など）
 *     3C      サーボの速さ（度/秒、初めは 90）
 *     3E      ウォッチドッグ（ミリ秒、0 で止める）。この時間、どのポートにも書かなければ非常停止
 *     3F      非常停止。1 を書くと止め、0 を書くと解く。読むと 0 ふつう、1 非常停止、2 ウォッチドッグ
 *     40〜7F  サーボ 0〜63。目標の角度（0.1 度、−1800〜1800）を書く。読むと今の角度
 *   非常停止のあいだは、アクチュエータとデジタル出力は 0、サーボはその場で止まり、書いても効かない。
 */
#ifndef VIO_SIM_H
#define VIO_SIM_H

#include <stdint.h>

#define VIO_NPORT       0x80
#define VIO_NSERVO      64

enum { VIO_TEMP, VIO_HUMID, VIO_WATER, VIO_DIST, VIO_LIGHT, VIO_SOUND, VIO_TOUCH, VIO_GAS, VIO_PH, VIO_NSENS };
enum { VIO_HEATER, VIO_FAN, VIO_PUMP, VIO_LAMP, VIO_MOTOR, VIO_NACT };

#define VIO_P_ACT       0x10
#define VIO_P_DOUT      0x20
#define VIO_P_SPEED     0x3C
#define VIO_P_WDT       0x3E
#define VIO_P_ESTOP     0x3F
#define VIO_P_SERVO     0x40

typedef struct {
    int64_t t;                      /* 最後に進めた時刻（ミリ秒） */
    int64_t s[VIO_NSENS];           /* センサーの状態（ミリ単位。温度は 0.001℃、pH は 0.001） */
    int32_t act[VIO_NACT];          /* アクチュエータ（%） */
    int32_t dout;
    int64_t servo[VIO_NSERVO];      /* 今の角度（0.0001 度） */
    int32_t target[VIO_NSERVO];     /* 目標（0.1 度） */
    int32_t speed;                  /* 度/秒 */
    int32_t touch_manual;
    int32_t wdt_ms;
    int64_t wdt_last;
    int32_t estop;                  /* 0 ふつう 1 非常停止 2 ウォッチドッグ */
} VioState;

static void vio_init(VioState *v, int64_t now)
{
    int i;
    for (i = 0; i < VIO_NSENS; i++) {
        v->s[i] = 0;
    }
    v->s[VIO_TEMP] = 20000;         /* 20.0℃ */
    v->s[VIO_HUMID] = 60000;
    v->s[VIO_WATER] = 500000;       /* 500mm */
    v->s[VIO_DIST] = 500000;
    v->s[VIO_LIGHT] = 200000;
    v->s[VIO_SOUND] = 35000;
    v->s[VIO_GAS] = 1400000;
    v->s[VIO_PH] = 7000;
    for (i = 0; i < VIO_NACT; i++) {
        v->act[i] = 0;
    }
    for (i = 0; i < VIO_NSERVO; i++) {
        v->servo[i] = 0;
        v->target[i] = 0;
    }
    v->dout = 0;
    v->speed = 90;
    v->touch_manual = 0;
    v->wdt_ms = 0;
    v->wdt_last = now;
    v->estop = 0;
    v->t = now;
}

/* x を eq へ、時定数 tau ミリ秒で dt ミリ秒だけ近づける */
static int64_t vio_toward(int64_t x, int64_t eq, int64_t dt, int64_t tau)
{
    if (dt >= tau) {
        return eq;
    }
    return x + (eq - x) * dt / tau;
}

static void vio_stop_outputs(VioState *v, int why)
{
    int i;
    for (i = 0; i < VIO_NACT; i++) {
        v->act[i] = 0;
    }
    v->dout = 0;
    for (i = 0; i < VIO_NSERVO; i++) {
        v->target[i] = (int32_t)(v->servo[i] / 1000);
    }
    v->estop = why;
}

/* dt ミリ秒（100 以下）だけ状態を進める */
static void vio_step(VioState *v, int64_t dt)
{
    int64_t *s = v->s;
    int32_t *a = v->act;
    int64_t eq, tau;
    int i;

    /* 温度: ヒーター 100% で 35℃に向かう。ファンは外気（20℃）へ早く戻す */
    eq = 20000 + 150 * a[VIO_HEATER] * (100 - a[VIO_FAN] / 2) / 100;
    tau = 60000 * 100 / (100 + 2 * a[VIO_FAN]);
    s[VIO_TEMP] = vio_toward(s[VIO_TEMP], eq, dt, tau);
    /* 湿度: ポンプ（水やり）で上がり、暖めると下がり、ファンで下がる */
    eq = 60000 + 300 * a[VIO_PUMP] - (s[VIO_TEMP] - 20000) - 150 * a[VIO_FAN];
    eq = eq < 10000 ? 10000 : eq > 100000 ? 100000 : eq;
    s[VIO_HUMID] = vio_toward(s[VIO_HUMID], eq, dt, 120000);
    /* 水位: ポンプ 100% で毎秒 5mm 入り、毎秒 1mm 出ていく（0〜1000mm） */
    s[VIO_WATER] += (50 * a[VIO_PUMP] - 1000) * dt / 1000;
    s[VIO_WATER] = s[VIO_WATER] < 0 ? 0 : s[VIO_WATER] > 1000000 ? 1000000 : s[VIO_WATER];
    /* 距離: 搬送のモーター 100% で品物が毎秒 100mm 近づく。届いたら次の品物（500mm） */
    s[VIO_DIST] -= 1000 * a[VIO_MOTOR] * dt / 1000;
    if (s[VIO_DIST] <= 0) {
        s[VIO_DIST] += 500000;
    }
    /* 明るさ: 外の光 200lx に、照明 100% で 800lx を足す（すぐ変わる） */
    s[VIO_LIGHT] = vio_toward(s[VIO_LIGHT], 200000 + 8000 * a[VIO_LAMP], dt, 500);
    /* 音: 静かな部屋 35dB に、ファン・ポンプ・モーターの音 */
    eq = 35000 + 250 * a[VIO_FAN] + 150 * a[VIO_PUMP] + 200 * a[VIO_MOTOR];
    s[VIO_SOUND] = vio_toward(s[VIO_SOUND], eq, dt, 500);
    /* CO2: 閉めきると 1400ppm、ファン 100% で外気の 400ppm へ */
    s[VIO_GAS] = vio_toward(s[VIO_GAS], 1400000 - 10000 * a[VIO_FAN], dt, 60000);
    /* pH: 放っておくと 6.0 へ下がり、ポンプ（新しい水）で 7.0 へ戻る */
    eq = 6000 + 10 * a[VIO_PUMP];
    s[VIO_PH] = vio_toward(s[VIO_PH], eq, dt, a[VIO_PUMP] ? 30000 : 600000);
    /* 触覚: 品物が 30mm より近いか、手で触れた（書いた）とき */
    s[VIO_TOUCH] = (s[VIO_DIST] < 30000 || v->touch_manual) ? 1000 : 0;
    /* サーボ: 目標へ、決まった速さで */
    for (i = 0; i < VIO_NSERVO; i++) {
        int64_t want = (int64_t)v->target[i] * 1000, d = (int64_t)v->speed * 10 * dt;
        if (v->servo[i] < want) {
            v->servo[i] = v->servo[i] + d > want ? want : v->servo[i] + d;
        } else if (v->servo[i] > want) {
            v->servo[i] = v->servo[i] - d < want ? want : v->servo[i] - d;
        }
    }
}

/* now まで進める。ウォッチドッグが切れたら、その時刻で出力を止める */
static void vio_advance(VioState *v, int64_t now)
{
    while (v->t < now) {
        int64_t dt = now - v->t;
        if (dt > 100) {
            dt = 100;
        }
        if (v->wdt_ms > 0 && v->estop == 0) {
            int64_t left = v->wdt_last + v->wdt_ms - v->t;
            if (left <= 0) {
                vio_stop_outputs(v, 2);
            } else if (dt > left) {
                dt = left;
            }
        }
        vio_step(v, dt);
        v->t += dt;
    }
    if (v->wdt_ms > 0 && v->estop == 0 && now >= v->wdt_last + v->wdt_ms) {
        vio_stop_outputs(v, 2);
    }
}

static int32_t vio_read(VioState *v, unsigned port, int64_t now)
{
    vio_advance(v, now);
    if (port < VIO_NSENS) {
        int64_t x = v->s[port];
        if (port == VIO_TEMP || port == VIO_PH) {
            x /= 100;                       /* 0.1 の単位 */
        } else {
            x /= 1000;
        }
        return (int32_t)x;
    }
    if (port >= VIO_P_ACT && port < VIO_P_ACT + VIO_NACT) {
        return v->act[port - VIO_P_ACT];
    }
    if (port >= VIO_P_SERVO && port < VIO_P_SERVO + VIO_NSERVO) {
        int64_t x = v->servo[port - VIO_P_SERVO];
        return (int32_t)((x + (x >= 0 ? 500 : -500)) / 1000);
    }
    switch (port) {
    case VIO_P_DOUT: return v->dout;
    case VIO_P_SPEED: return v->speed;
    case VIO_P_WDT: return v->wdt_ms;
    case VIO_P_ESTOP: return v->estop;
    }
    return 0;
}

static void vio_write(VioState *v, unsigned port, int32_t x, int64_t now)
{
    vio_advance(v, now);
    v->wdt_last = now;                      /* どこに書いても、ウォッチドッグを満たす */
    if (port == VIO_P_ESTOP) {
        if (x) {
            vio_stop_outputs(v, 1);
        } else {
            v->estop = 0;
        }
        return;
    }
    if (port == VIO_P_WDT) {
        v->wdt_ms = x < 0 ? 0 : x;
        return;
    }
    if (port < VIO_NSENS) {                 /* センサーの値を置き換える（試験用） */
        if (port == VIO_TOUCH) {
            v->touch_manual = x != 0;
            v->s[VIO_TOUCH] = (v->s[VIO_DIST] < 30000 || x) ? 1000 : 0;
        } else {
            v->s[port] = (int64_t)x * ((port == VIO_TEMP || port == VIO_PH) ? 100 : 1000);
        }
        return;
    }
    if (port == VIO_P_SPEED) {
        v->speed = x < 1 ? 1 : x > 3600 ? 3600 : x;
        return;
    }
    if (v->estop) {
        return;                             /* 非常停止のあいだ、出力は変えない */
    }
    if (port >= VIO_P_ACT && port < VIO_P_ACT + VIO_NACT) {
        v->act[port - VIO_P_ACT] = x < 0 ? 0 : x > 100 ? 100 : x;
    } else if (port == VIO_P_DOUT) {
        v->dout = x & 0xffff;
    } else if (port >= VIO_P_SERVO && port < VIO_P_SERVO + VIO_NSERVO) {
        v->target[port - VIO_P_SERVO] = x < -1800 ? -1800 : x > 1800 ? 1800 : x;
    }
}

#endif
