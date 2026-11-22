/* iomgr_core.h — 入出力の名前の口（/etc/io.conf）と、安全の決まり
 *
 *   プログラムは口を名前で呼ぶ（HEATER・TEMP など）。名前と実物の結びつけ、上限、換算は /etc/io.conf に書く。
 *   実物を受け持つのは「中身」（backend）: vio（仮想の機器）・usbio（USB の I/O ボード）・som（SoM のピン）。
 *   カーネルの /dev/io と、ふつうの Linux で T88-BASIC を試すときの両方が、この同じコードを使う。
 *   整数だけで計算する。値はミリ単位（1/1000）で持つ（温度 25.5℃ は 25500、PWM 50% は 50000、角度 90 度は 90000）。
 *
 *   /etc/io.conf の 1 行:   名前 = 実物 [min 値] [max 値] [speed 度/秒] [scale 倍率] [offset 値] [input|output] [invert] [unit 単位]
 *     実物:  vio.adcN・vio.pwmN・vio.servoN・vio.pinN / usbio.pinN・usbio.adcN・usbio.pwmN・usbio.servoN /
 *            som.pinN・som.pwmN・som.servoN（gpioN は som.pinN、pwmN は som.pwmN と同じ）
 *     scale・offset: 物の値 = 生の値 × scale + offset（ADC の数え値を ℃ や % にするときなど）
 *     min・max: 出力の上限（物の値）。超える値を出すとエラー。speed: サーボの速さの上限
 *     invert: 0 で電気が入るつなぎ方（ピンは 1 と 0、PWM は 100% と 0% を入れ替える。止めるときも、切れる側にする）
 *   # から後ろは注釈。
 *
 *   安全の決まり:
 *     - 起動したときは「まだ動かさない」（状態 3）。出力するには、はっきり解除する（ESTOP 0）
 *     - 非常停止（1）・ウォッチドッグ（2）・まだ動かさない（3）のあいだは、出力に 0 以外を出せない（エラー）
 *     - 止めるときは、すべての中身の出力を 0 にし、サーボをその場で止める
 */
#ifndef IOMGR_CORE_H
#define IOMGR_CORE_H

#include <stdint.h>
#include <stddef.h>

#define IO_MAXENT       64
#define IO_MAXBE        8
#define IO_NAMELEN      24

enum { IO_K_PIN = 1, IO_K_ADC, IO_K_PWM, IO_K_SERVO };
enum { IO_OK = 0, IO_ENODEV = -1, IO_ERANGE = -2, IO_ESTOP = -3, IO_EKIND = -4, IO_EIO = -5, IO_EPERM = -6 };
enum { IO_RUN = 0, IO_STOPPED = 1, IO_WATCHDOG = 2, IO_NOTARMED = 3 };

typedef struct IoBackend {
    const char *name;                                   /* "vio" "usbio" "som" */
    void *ctx;
    /* 生の値（中身の自然な単位のミリ）を読む・出す。speed はサーボの速さ（度/秒、0 = 指定なし） */
    int (*get)(void *ctx, int kind, int chan, int64_t *raw_m);
    int (*set)(void *ctx, int kind, int chan, int64_t raw_m, int32_t speed);
    void (*setup)(void *ctx, int kind, int chan, int output);   /* ピンの向きなどの準備（NULL 可） */
    void (*safe)(void *ctx);                            /* 出力を 0、サーボをその場で止める */
    void (*tick)(void *ctx, int ms);                    /* 時間で動かすもの（サーボの速さなど）。NULL 可 */
} IoBackend;

typedef struct {
    char name[IO_NAMELEN];
    char be[8];                 /* 中身の名前 */
    int kind, chan, output, invert;
    int has_min, has_max;
    int64_t min_m, max_m;
    int32_t speed_max;          /* 0 = 上限なし */
    int64_t scale_u;            /* 倍率（100 万分の 1） */
    int64_t offset_m;
} IoEntry;

typedef struct {
    IoEntry ent[IO_MAXENT];
    int nent;
    IoBackend *be[IO_MAXBE];
    int nbe;
    int state;                  /* IO_RUN など */
    int32_t wdt_ms;
    int64_t wdt_last;
    int conf_errors;
    int conf_err_line;
    int io_gid;                 /* 動かしてよいグループ（root はいつも）。access group N */
    int net_ok;                 /* ネットワークのソケットを開いたプロセスも動かしてよいか。network allow|deny */
} IoMgr;

#ifndef IOMGR_TYPES_ONLY             /* ドライバは型だけを使う */

/* ---- 小さな道具（カーネルでも使えるよう、C ライブラリに頼らない） */
static int io_upper(int c)
{
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static int io_streq_ci(const char *a, const char *b)
{
    while (*a && *b && io_upper((unsigned char)*a) == io_upper((unsigned char)*b)) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int io_space(int c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* "−12.345" を mult 倍した整数に（mult = 1000 ならミリ） */
static int io_parse_dec(const char *s, int64_t mult, int64_t *out)
{
    int neg = 0, digits = 0;
    int64_t ip = 0, fp = 0, fdiv = 1;
    if (*s == '-' || *s == '+') {
        neg = *s == '-';
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        ip = ip * 10 + (*s++ - '0');
        digits++;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (fdiv < 1000000000LL) {
                fp = fp * 10 + (*s - '0');
                fdiv *= 10;
            }
            s++;
            digits++;
        }
    }
    if (!digits || *s) {
        return 0;
    }
    *out = (ip * mult + fp * mult / fdiv) * (neg ? -1 : 1);
    return 1;
}

static int io_parse_int(const char *s, int *out)
{
    int64_t v;
    if (!io_parse_dec(s, 1, &v) || v < 0 || v > 100000) {
        return 0;
    }
    *out = (int)v;
    return 1;
}

static void io_init(IoMgr *m)
{
    m->nent = 0;
    m->nbe = 0;
    m->state = IO_NOTARMED;
    m->wdt_ms = 0;
    m->wdt_last = 0;
    m->conf_errors = 0;
    m->conf_err_line = 0;
    m->io_gid = 20;             /* /etc/group の io */
    m->net_ok = 0;              /* 初めは許さない（安全の決まり 6） */
}

static IoBackend *io_find_be(IoMgr *m, const char *name)
{
    int i;
    for (i = 0; i < m->nbe; i++) {
        if (io_streq_ci(m->be[i]->name, name)) {
            return m->be[i];
        }
    }
    return NULL;
}

/* 「usbio.pwm3」「gpio17」などを読む */
static int io_parse_target(const char *s, IoEntry *e)
{
    static const struct { const char *w; int k; } kinds[] = {
        { "pin", IO_K_PIN }, { "adc", IO_K_ADC }, { "pwm", IO_K_PWM }, { "servo", IO_K_SERVO }
    };
    const char *dot = s, *p;
    int i, n;
    while (*dot && *dot != '.') {
        dot++;
    }
    if (*dot == '.') {
        n = (int)(dot - s);
        if (n < 1 || n > 7) {
            return 0;
        }
        for (i = 0; i < n; i++) {
            e->be[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
        }
        e->be[n] = 0;
        p = dot + 1;
    } else if ((s[0] == 'g' || s[0] == 'G') && (s[1] == 'p' || s[1] == 'P')
               && (s[2] == 'i' || s[2] == 'I') && (s[3] == 'o' || s[3] == 'O')) {
        e->be[0] = 's'; e->be[1] = 'o'; e->be[2] = 'm'; e->be[3] = 0;     /* gpioN = som.pinN */
        e->kind = IO_K_PIN;
        return io_parse_int(s + 4, &e->chan);
    } else {
        e->be[0] = 's'; e->be[1] = 'o'; e->be[2] = 'm'; e->be[3] = 0;     /* pwmN = som.pwmN */
        p = s;
    }
    for (i = 0; i < 4; i++) {
        const char *w = kinds[i].w, *q = p;
        while (*w && io_upper((unsigned char)*q) == io_upper((unsigned char)*w)) {
            w++;
            q++;
        }
        if (*w == 0 && *q >= '0' && *q <= '9') {
            e->kind = kinds[i].k;
            return io_parse_int(q, &e->chan);
        }
    }
    return 0;
}

/* 1 行を読む。空行・注釈だけなら 0、読めたら 1、まちがいなら −1 */
static int io_parse_line(IoMgr *m, char *line)
{
    char *tok[24];
    int nt = 0, i;
    IoEntry e;
    char *p = line, *eq = NULL;

    for (p = line; *p; p++) {                       /* # から後ろを捨てる */
        if (*p == '#') {
            *p = 0;
            break;
        }
        if (*p == '=' && !eq) {
            eq = p;
        }
    }
    if (eq) {
        *eq = 0;
    }
    for (p = line; ; ) {                            /* 空白で区切る（= の左で 1 つ、右でいくつか） */
        while (io_space((unsigned char)*p)) {
            p++;
        }
        if (!*p) {
            if (eq && p < eq + 1) {
                p = eq + 1;
                if (nt != 1) {
                    return -1;
                }
                tok[nt++] = (char *)"=";
                continue;
            }
            break;
        }
        if (nt >= 24) {
            return -1;
        }
        tok[nt++] = p;
        while (*p && !io_space((unsigned char)*p)) {
            p++;
        }
        if (*p) {
            *p++ = 0;
        }
    }
    if (nt == 0) {
        return 0;
    }
    if (!eq && nt == 3 && io_streq_ci(tok[0], "access") && io_streq_ci(tok[1], "group")) {
        int g = 0;
        for (i = 0; tok[2][i]; i++) {               /* access group N: 動かしてよいグループの番号 */
            if (tok[2][i] < '0' || tok[2][i] > '9' || g > 1000000) {
                return -1;
            }
            g = g * 10 + (tok[2][i] - '0');
        }
        m->io_gid = g;
        return 0;
    }
    if (!eq && nt == 2 && io_streq_ci(tok[0], "network")) {     /* network allow|deny */
        if (io_streq_ci(tok[1], "allow")) {
            m->net_ok = 1;
        } else if (io_streq_ci(tok[1], "deny")) {
            m->net_ok = 0;
        } else {
            return -1;
        }
        return 0;
    }
    if (!eq || nt < 3 || tok[1][0] != '=' || m->nent >= IO_MAXENT) {
        return -1;
    }
    for (i = 0; tok[0][i]; i++) {
        if (i >= IO_NAMELEN - 1) {
            return -1;
        }
        e.name[i] = (char)io_upper((unsigned char)tok[0][i]);
    }
    e.name[i] = 0;
    e.has_min = e.has_max = 0;
    e.min_m = e.max_m = 0;
    e.speed_max = 0;
    e.scale_u = 1000000;
    e.offset_m = 0;
    e.chan = 0;
    if (!io_parse_target(tok[2], &e)) {
        return -1;
    }
    e.output = e.kind != IO_K_ADC;
    e.invert = 0;
    for (i = 3; i < nt; i++) {
        const char *k = tok[i], *v = i + 1 < nt ? tok[i + 1] : "";
        if (io_streq_ci(k, "input")) {
            e.output = 0;
        } else if (io_streq_ci(k, "output")) {
            e.output = 1;
        } else if (io_streq_ci(k, "invert")) {
            e.invert = 1;
        } else if (io_streq_ci(k, "min") && io_parse_dec(v, 1000, &e.min_m)) {
            e.has_min = 1;
            i++;
        } else if (io_streq_ci(k, "max") && io_parse_dec(v, 1000, &e.max_m)) {
            e.has_max = 1;
            i++;
        } else if (io_streq_ci(k, "speed") && io_parse_int(v, &e.speed_max)) {
            i++;
        } else if (io_streq_ci(k, "scale") && io_parse_dec(v, 1000000, &e.scale_u) && e.scale_u != 0) {
            i++;
        } else if (io_streq_ci(k, "offset") && io_parse_dec(v, 1000, &e.offset_m)) {
            i++;
        } else if (io_streq_ci(k, "unit") && *v) {
            i++;                                    /* 単位は説明のため（計算には使わない） */
        } else {
            return -1;
        }
    }
    for (i = 0; i < m->nent; i++) {
        if (io_streq_ci(m->ent[i].name, e.name)) {
            return -1;                              /* 同じ名前は二度書けない */
        }
    }
    m->ent[m->nent++] = e;
    return 1;
}

/* 設定の文を読む（書き換えてよい作業用の場所 buf を渡す）。まちがった行は飛ばして数える */
static void io_parse_conf(IoMgr *m, char *buf, size_t len)
{
    size_t i = 0;
    int lineno = 0;
    while (i < len) {
        char *line = buf + i;
        while (i < len && buf[i] != '\n') {
            i++;
        }
        if (i < len) {
            buf[i++] = 0;
        } else {
            buf[len] = 0;
        }
        lineno++;
        if (io_parse_line(m, line) < 0) {
            m->conf_errors++;
            if (!m->conf_err_line) {
                m->conf_err_line = lineno;
            }
        }
    }
}

static void io_setup_entries(IoMgr *m, IoBackend *b)
{
    int i;
    if (!b->setup) {
        return;
    }
    for (i = 0; i < m->nent; i++) {
        if (io_streq_ci(m->ent[i].be, b->name)) {
            b->setup(b->ctx, m->ent[i].kind, m->ent[i].chan, m->ent[i].output);
        }
    }
}

static void io_safe_all(IoMgr *m);

static void io_add_backend(IoMgr *m, IoBackend *b)
{
    if (m->nbe < IO_MAXBE && !io_find_be(m, b->name)) {
        m->be[m->nbe++] = b;
        io_setup_entries(m, b);
        if (m->state != IO_RUN) {
            io_safe_all(m);                         /* 止めた状態に合わせる（invert は切れる側） */
        }
    }
}

/* invert のときの、生の値の入れ替え */
static int64_t io_flip(const IoEntry *e, int64_t raw_m)
{
    if (!e->invert) {
        return raw_m;
    }
    if (e->kind == IO_K_PIN) {
        return raw_m ? 0 : 1000;
    }
    if (e->kind == IO_K_PWM) {
        return 100000 - raw_m;
    }
    return raw_m;
}

static void io_safe_all(IoMgr *m)
{
    int i;
    for (i = 0; i < m->nbe; i++) {
        if (m->be[i]->safe) {
            m->be[i]->safe(m->be[i]->ctx);
        }
    }
    for (i = 0; i < m->nent; i++) {             /* invert の出力は、切れる側（生の値 1・100%）にする */
        IoEntry *e = &m->ent[i];
        IoBackend *b;
        if (e->invert && e->output && (e->kind == IO_K_PIN || e->kind == IO_K_PWM)
            && (b = io_find_be(m, e->be)) != NULL) {
            b->set(b->ctx, e->kind, e->chan, io_flip(e, 0), 0);
        }
    }
}

static void io_stop(IoMgr *m, int why)
{
    m->state = why;
    io_safe_all(m);
}

static void io_release(IoMgr *m, int64_t now)
{
    m->state = IO_RUN;
    m->wdt_last = now;
}

static void io_set_watchdog(IoMgr *m, int32_t ms, int64_t now)
{
    m->wdt_ms = ms < 0 ? 0 : ms;
    m->wdt_last = now;
}

/* ウォッチドッグを見る（カーネルは 10 ミリ秒ごと、ほかは読み書きのたびに） */
static inline void io_tick_backends(IoMgr *m, int ms)
{
    int i;
    for (i = 0; i < m->nbe; i++) {
        if (m->be[i]->tick) {
            m->be[i]->tick(m->be[i]->ctx, ms);
        }
    }
}

static void io_tick(IoMgr *m, int64_t now)
{
    if (m->wdt_ms > 0 && m->state == IO_RUN && now - m->wdt_last >= m->wdt_ms) {
        io_stop(m, IO_WATCHDOG);
    }
}

static int io_lookup(IoMgr *m, const char *name)
{
    int i;
    for (i = 0; i < m->nent; i++) {
        if (io_streq_ci(m->ent[i].name, name)) {
            return i;
        }
    }
    return IO_ENODEV;
}

/* 中身の生の値の、上限の確かめ（名前の口からでも、番号の書き方からでも同じ上限がかかる） */
static int io_check_limits(IoMgr *m, const char *be, int kind, int chan, int64_t phys_or_raw_m,
                           int32_t speed, int is_raw)
{
    int i;
    for (i = 0; i < m->nent; i++) {
        IoEntry *e = &m->ent[i];
        int64_t v = phys_or_raw_m;
        if (e->kind != kind || e->chan != chan || !io_streq_ci(e->be, be)) {
            continue;
        }
        if (is_raw) {
            v = phys_or_raw_m * e->scale_u / 1000000 + e->offset_m;
        }
        if ((e->has_min && v < e->min_m) || (e->has_max && v > e->max_m)) {
            return IO_ERANGE;
        }
        if (speed > 0 && e->speed_max > 0 && speed > e->speed_max) {
            return IO_ERANGE;
        }
    }
    return IO_OK;
}

static int io_get(IoMgr *m, int id, int64_t *phys_m, int64_t now)
{
    IoEntry *e;
    IoBackend *b;
    int64_t raw;
    int r;
    io_tick(m, now);
    if (id < 0 || id >= m->nent) {
        return IO_ENODEV;
    }
    e = &m->ent[id];
    b = io_find_be(m, e->be);
    if (!b) {
        return IO_ENODEV;
    }
    r = b->get(b->ctx, e->kind, e->chan, &raw);
    if (r < 0) {
        return r;
    }
    raw = io_flip(e, raw);
    *phys_m = raw * e->scale_u / 1000000 + e->offset_m;
    return IO_OK;
}

static int io_set(IoMgr *m, int id, int64_t phys_m, int32_t speed, int64_t now)
{
    IoEntry *e;
    IoBackend *b;
    int64_t raw;
    int r;
    io_tick(m, now);
    if (id < 0 || id >= m->nent) {
        return IO_ENODEV;
    }
    e = &m->ent[id];
    if (!e->output) {
        return IO_EKIND;
    }
    b = io_find_be(m, e->be);
    if (!b) {
        return IO_ENODEV;
    }
    if (m->state != IO_RUN && (phys_m != 0 || e->kind == IO_K_SERVO)) {
        return IO_ESTOP;
    }
    if ((e->has_min && phys_m < e->min_m) || (e->has_max && phys_m > e->max_m)
        || (speed > 0 && e->speed_max > 0 && speed > e->speed_max)) {
        return IO_ERANGE;
    }
    if (e->kind == IO_K_SERVO && speed <= 0 && e->speed_max > 0) {
        speed = e->speed_max;                       /* 速さを言わなければ、上限の速さ */
    }
    m->wdt_last = now;
    raw = io_flip(e, (phys_m - e->offset_m) * 1000000 / e->scale_u);
    r = b->set(b->ctx, e->kind, e->chan, raw, speed);
    return r;
}

/* ---------------------------------------------------------------- 中身: vio（仮想の機器） */
#include "vio_sim.h"

static int64_t io_vio_now;                          /* 呼ぶ側が読み書きの前に決める */

static int io_vio_get(void *ctx, int kind, int chan, int64_t *raw_m)
{
    VioState *v = (VioState *)ctx;
    vio_advance(v, io_vio_now);
    switch (kind) {
    case IO_K_ADC:
        if (chan < 0 || chan >= VIO_NSENS) {
            return IO_ENODEV;
        }
        *raw_m = v->s[chan];
        return IO_OK;
    case IO_K_PWM:
        if (chan < 0 || chan >= VIO_NACT) {
            return IO_ENODEV;
        }
        *raw_m = (int64_t)v->act[chan] * 1000;
        return IO_OK;
    case IO_K_SERVO:
        if (chan < 0 || chan >= VIO_NSERVO) {
            return IO_ENODEV;
        }
        *raw_m = v->servo[chan] / 10;
        return IO_OK;
    case IO_K_PIN:
        if (chan < 0 || chan > 15) {
            return IO_ENODEV;
        }
        *raw_m = ((v->dout >> chan) & 1) * 1000;
        return IO_OK;
    }
    return IO_EKIND;
}

static int io_vio_set(void *ctx, int kind, int chan, int64_t raw_m, int32_t speed)
{
    VioState *v = (VioState *)ctx;
    int64_t now = io_vio_now;
    switch (kind) {
    case IO_K_PWM:
        if (chan < 0 || chan >= VIO_NACT || raw_m < 0 || raw_m > 100000) {
            return chan < 0 || chan >= VIO_NACT ? IO_ENODEV : IO_ERANGE;
        }
        vio_write(v, VIO_P_ACT + (unsigned)chan, (int32_t)((raw_m + 500) / 1000), now);
        return IO_OK;
    case IO_K_SERVO:
        if (chan < 0 || chan >= VIO_NSERVO) {
            return IO_ENODEV;
        }
        if (raw_m < -180000 || raw_m > 180000) {
            return IO_ERANGE;
        }
        if (speed > 0) {
            vio_write(v, VIO_P_SPEED, speed, now);
        }
        vio_write(v, VIO_P_SERVO + (unsigned)chan, (int32_t)((raw_m + (raw_m >= 0 ? 50 : -50)) / 100), now);
        return IO_OK;
    case IO_K_PIN:
        if (chan < 0 || chan > 15) {
            return IO_ENODEV;
        }
        vio_write(v, VIO_P_DOUT, raw_m ? (v->dout | (1 << chan)) : (v->dout & ~(1 << chan)), now);
        return IO_OK;
    }
    return IO_EKIND;
}

static void io_vio_safe(void *ctx)
{
    VioState *v = (VioState *)ctx;
    int i;
    vio_advance(v, io_vio_now);
    for (i = 0; i < VIO_NACT; i++) {
        v->act[i] = 0;
    }
    v->dout = 0;
    for (i = 0; i < VIO_NSERVO; i++) {
        v->target[i] = (int32_t)(v->servo[i] / 1000);
    }
}

static void io_vio_backend(IoBackend *b, VioState *v)
{
    b->name = "vio";
    b->ctx = v;
    b->get = io_vio_get;
    b->set = io_vio_set;
    b->setup = NULL;
    b->safe = io_vio_safe;
    b->tick = NULL;
}

/* 番号の書き方（INP・OUT &HE000〜、PWM 0,50 など）: 仮想の機器のポートを直接読み書きする。
 * 非常停止・ウォッチドッグ（3F・3E）はこの管理の側が受け持ち、上限も名前の口と同じにかける */
static int io_raw_read(IoMgr *m, VioState *v, unsigned port, int32_t *out, int64_t now)
{
    io_tick(m, now);
    if (port == VIO_P_ESTOP) {
        *out = m->state;
    } else if (port == VIO_P_WDT) {
        *out = m->wdt_ms;
    } else {
        *out = vio_read(v, port, now);
    }
    return IO_OK;
}

static int io_raw_write(IoMgr *m, VioState *v, unsigned port, int32_t x, int64_t now)
{
    int r = IO_OK;
    io_tick(m, now);
    if (port == VIO_P_ESTOP) {
        if (x) {
            io_stop(m, IO_STOPPED);
        } else {
            io_release(m, now);
        }
        return IO_OK;
    }
    if (port == VIO_P_WDT) {
        io_set_watchdog(m, x, now);
        return IO_OK;
    }
    if (port >= VIO_P_ACT && port < VIO_P_ACT + VIO_NACT) {
        if (m->state != IO_RUN && x != 0) {
            return IO_ESTOP;
        }
        r = io_check_limits(m, "vio", IO_K_PWM, (int)(port - VIO_P_ACT), (int64_t)x * 1000, 0, 1);
    } else if (port >= VIO_P_SERVO && port < VIO_P_SERVO + VIO_NSERVO) {
        if (m->state != IO_RUN) {
            return IO_ESTOP;
        }
        r = io_check_limits(m, "vio", IO_K_SERVO, (int)(port - VIO_P_SERVO), (int64_t)x * 100, 0, 1);
    } else if (port == VIO_P_DOUT) {
        if (m->state != IO_RUN && x != 0) {
            return IO_ESTOP;
        }
    } else if (port == VIO_P_SPEED) {
        int i;                                      /* 速さは全部のサーボに効く: どれかの上限を超えたら */
        for (i = 0; i < m->nent; i++) {
            IoEntry *e = &m->ent[i];
            if (e->kind == IO_K_SERVO && io_streq_ci(e->be, "vio") && e->speed_max > 0 && x > e->speed_max) {
                r = IO_ERANGE;
            }
        }
    }
    if (r != IO_OK) {
        return r;
    }
    m->wdt_last = now;
    vio_write(v, port, x, now);
    return IO_OK;
}

/* /etc/io.conf がないときの結びつけ（ディスクの /etc/io.conf と同じ中身） */
static const char io_default_conf[] =
    "# 仮想の機器（センサー）\n"
    "TEMP   = vio.adc0  unit C\n"
    "HUMID  = vio.adc1  unit %\n"
    "LEVEL  = vio.adc2  unit mm\n"
    "DIST   = vio.adc3  unit mm\n"
    "LIGHT  = vio.adc4  unit lx\n"
    "SOUND  = vio.adc5  unit dB\n"
    "TOUCH  = vio.adc6\n"
    "CO2    = vio.adc7  unit ppm\n"
    "PH     = vio.adc8\n"
    "# 仮想の機器（アクチュエータ・出力）\n"
    "HEATER = vio.pwm0\n"
    "FAN    = vio.pwm1\n"
    "PUMP   = vio.pwm2\n"
    "LAMP   = vio.pwm3\n"
    "MOTOR  = vio.pwm4\n"
    "LED0   = vio.pin0\n"
    "LED1   = vio.pin1\n"
    "LED2   = vio.pin2\n"
    "# 仮想のロボットアーム（上限の例: 肩は -90〜90 度、毎秒 180 度まで）\n"
    "ARM0   = vio.servo0  min -90  max 90  speed 180\n"
    "ARM1   = vio.servo1  min -90  max 90  speed 180\n"
    "ARM2   = vio.servo2  min -90  max 90  speed 180\n"
    "# USB の I/O ボード（Firmata）をつないだとき\n"
    "BLED   = usbio.pin13\n"
    "BBTN   = usbio.pin2   input\n"
    "BKNOB  = usbio.adc0   scale 0.09775  unit %\n"
    "BDIM   = usbio.pwm3\n"
    "BSERVO = usbio.servo9  min 0  max 180\n"
    "BSENSE1 = usbio.adc1\n"
    "BSENSE2 = usbio.adc2\n";

#endif  /* IOMGR_TYPES_ONLY */
#endif
