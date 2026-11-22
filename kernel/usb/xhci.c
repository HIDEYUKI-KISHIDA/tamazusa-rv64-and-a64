/*
 * xhci.c — USB のホスト（xHCI）（S-mode + MMU のみ）
 *
 *   xHCI は、ほとんどの SoC と PC が使う USB のホストの規格。QEMU では -device qemu-xhci で付く。
 *   この段（作業の 1 番）でするのは、土台と、つながった装置の確認まで:
 *     1. PCIe で xHCI を見つけ（クラス 0C0330）、止めて初期化し、動かす
 *     2. 命令のリング・できごと（イベント）のリング・装置の表（DCBAA）を用意する
 *     3. つながっている口（ポート）をリセットし、装置ごとに番号（スロット）をもらって住所を決める
 *     4. 装置の記述子（メーカー・製品の番号、製品の名前）を読む
 *   作業の 2 番: HID のブート用キーボード、3 番: マウス・タブレット（このファイルの後半）。
 *   割込みは使わず、できごとのリングを見に行く。ハブの先の装置はまだ扱わない（ルートの口だけ）。
 *   メモリはカーネルの静的な領域を使う（カーネルは物理アドレス＝仮想アドレスなので、そのまま渡せる）。
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>
#include <libstr.h>
#include <rv_priv.h>

extern void uart_puts(const char *s);
extern int rv_pci_find_class(uint32_t class24, uintptr_t *bar0, unsigned *dev_out);

#define MAX_SLOTS   16
#define MAX_DEVS    16
#define RING_N      64

struct trb {
    uint64_t p;
    uint32_t s;
    uint32_t c;
};

/* TRB の種類 */
#define T_NORMAL        1
#define T_SETUP         2
#define T_DATA          3
#define T_STATUS        4
#define T_LINK          6
#define T_ENABLE_SLOT   9
#define T_ADDRESS_DEV   11
#define T_CONFIG_EP     12
#define T_EVAL_CTX      13
#define T_NOOP_CMD      23
#define T_EV_TRANSFER   32
#define T_EV_CMD_DONE   33
#define T_EV_PORT       34

#define CC_SUCCESS      1
#define CC_SHORT        13

static struct trb cmd_ring[RING_N] __attribute__((aligned(4096)));
static struct trb evt_ring[RING_N] __attribute__((aligned(4096)));
static struct {
    uint64_t base;
    uint32_t size, rsvd;
} erst[1] __attribute__((aligned(64)));
static uint64_t dcbaa[MAX_SLOTS + 1] __attribute__((aligned(64)));
static uint8_t devctx[MAX_SLOTS][2048] __attribute__((aligned(4096)));
static uint8_t inctx[4096] __attribute__((aligned(4096)));
static struct trb ep0ring[MAX_SLOTS][RING_N] __attribute__((aligned(4096)));
static uint64_t scratch_arr[8] __attribute__((aligned(64)));
static uint8_t scratch_pages[8][4096] __attribute__((aligned(4096)));
static uint8_t ctrl_buf[256] __attribute__((aligned(64)));

static unsigned cmd_enq, cmd_cycle = 1;
static unsigned evt_deq, evt_cycle = 1;
static unsigned ep0_enq[MAX_SLOTS], ep0_cycle[MAX_SLOTS];

static uintptr_t cap, op, rts, dbs;
static unsigned max_slots, max_ports, ctx_size;

struct usb_dev {
    unsigned port, speed, slot;
    uint16_t vid, pid;
    uint8_t dev_class, mps0;
    char name[40];
};
static struct usb_dev devs[MAX_DEVS];
static int ndevs;
static int xhci_ok;

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr64(uintptr_t a, uint64_t v)
{
    wr(a, (uint32_t)v);
    wr(a + 4, (uint32_t)(v >> 32));
}
static inline void fence_rw(void)
{
#if defined(__aarch64__)
    asm volatile("dmb sy" ::: "memory");
#else
    asm volatile("fence rw, rw" ::: "memory");
#endif
}
static inline uint64_t pa(const void *p) { return (uint64_t)(uintptr_t)p; }

static void put_hex(uint32_t v, int digits)
{
    char b[12];
    int i;
    for (i = digits - 1; i >= 0; i--) {
        b[i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    }
    b[digits] = 0;
    uart_puts(b);
}

static void put_dec(unsigned v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i > 0);
    uart_puts(&b[i]);
}

/* 条件が成り立つまで待つ（ミリ秒）。成り立てば 1 */
static int wait_bits(uintptr_t a, uint32_t mask, uint32_t want, int ms)
{
    int i;
    for (i = 0; i < ms; i++) {
        if ((rd(a) & mask) == want) {
            return 1;
        }
        tk_dly_tsk(1);
    }
    return (rd(a) & mask) == want;
}

/* リングに TRB を 1 つ積む。最後の 1 枠はリンク（先頭へ戻る） */
static void push(struct trb *ring, unsigned *enq, unsigned *cycle, uint64_t p, uint32_t s, uint32_t c)
{
    struct trb *t = &ring[*enq];
    t->p = p;
    t->s = s;
    fence_rw();
    t->c = c | (*cycle & 1);
    (*enq)++;
    if (*enq == RING_N - 1) {
        struct trb *l = &ring[RING_N - 1];
        l->p = pa(ring);
        l->s = 0;
        fence_rw();
        l->c = (T_LINK << 10) | 2 | (*cycle & 1);         /* Toggle Cycle */
        *cycle ^= 1;
        *enq = 0;
    }
}

static void dispatch(const struct trb *ev);

/* できごとを 1 つ待つ。type が一致したら out に写して 1。ほかのできごとは持ち主へ振り分ける */
static int wait_event(unsigned type, unsigned slot, struct trb *out, int ms)
{
    int waited = 0;
    for (;;) {
        struct trb *e = &evt_ring[evt_deq];
        if ((*(volatile uint32_t *)&e->c & 1) == evt_cycle) {
            struct trb ev;
            fence_rw();
            ev = *e;
            evt_deq++;
            if (evt_deq == RING_N) {
                evt_deq = 0;
                evt_cycle ^= 1;
            }
            wr64(rts + 0x20 + 0x18, pa(&evt_ring[evt_deq]) | 8);   /* ERDP（EHB を消す） */
            if (((ev.c >> 10) & 0x3f) == type && (slot == 0 || (ev.c >> 24) == slot)) {
                *out = ev;
                return 1;
            }
            dispatch(&ev);
            continue;
        }
        if (waited++ >= ms) {
            return 0;
        }
        tk_dly_tsk(1);
    }
}

/* 命令を出して完了を待つ。完了コードを返し、*slot_out に（あれば）スロットの番号 */
static int command(uint64_t p, uint32_t s, uint32_t c, unsigned *slot_out)
{
    struct trb ev;
    push(cmd_ring, &cmd_enq, &cmd_cycle, p, s, c);
    fence_rw();
    wr(dbs, 0);
    if (!wait_event(T_EV_CMD_DONE, 0, &ev, 1000)) {
        return -1;
    }
    if (slot_out) {
        *slot_out = ev.c >> 24;
    }
    return (int)(ev.s >> 24);
}

/* 制御の転送（読む向き）。成功で 0 */
static int control_in(unsigned slot, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index,
                      void *buf, uint16_t len)
{
    struct trb ev;
    unsigned i = slot - 1;
    uint64_t setup = (uint64_t)req_type | ((uint64_t)req << 8) | ((uint64_t)value << 16)
                   | ((uint64_t)index << 32) | ((uint64_t)len << 48);
    int cc;

    push(ep0ring[i], &ep0_enq[i], &ep0_cycle[i], setup, 8, (T_SETUP << 10) | (1 << 6) | (3u << 16));
    push(ep0ring[i], &ep0_enq[i], &ep0_cycle[i], pa(buf), len, (T_DATA << 10) | (1u << 16));
    push(ep0ring[i], &ep0_enq[i], &ep0_cycle[i], 0, 0, (T_STATUS << 10) | (1 << 5));
    fence_rw();
    wr(dbs + 4 * slot, 1);                                  /* EP0 */
    if (!wait_event(T_EV_TRANSFER, slot, &ev, 1000)) {
        return -1;
    }
    cc = (int)(ev.s >> 24);
    return (cc == CC_SUCCESS || cc == CC_SHORT) ? 0 : -cc;
}

/* 制御の転送（データなし、書く向き）。成功で 0 */
static int control_out0(unsigned slot, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index)
{
    struct trb ev;
    unsigned i = slot - 1;
    uint64_t setup = (uint64_t)req_type | ((uint64_t)req << 8) | ((uint64_t)value << 16)
                   | ((uint64_t)index << 32);
    push(ep0ring[i], &ep0_enq[i], &ep0_cycle[i], setup, 8, (T_SETUP << 10) | (1 << 6));
    push(ep0ring[i], &ep0_enq[i], &ep0_cycle[i], 0, 0, (T_STATUS << 10) | (1 << 5) | (1u << 16));
    fence_rw();
    wr(dbs + 4 * slot, 1);
    if (!wait_event(T_EV_TRANSFER, slot, &ev, 1000)) {
        return -1;
    }
    return (int)(ev.s >> 24) == CC_SUCCESS ? 0 : -1;
}

static uint32_t *ictx(unsigned n)        /* 入力のコンテキストの n 番（0 = 入力の制御） */
{
    return (uint32_t *)(inctx + n * ctx_size);
}

/* 口 port（1 から）につながった装置に住所を付け、記述子を読む */
static void enumerate(unsigned port, unsigned speed)
{
    unsigned slot = 0, i;
    int cc;
    uint32_t *sc, *ep;
    uint16_t mps = speed == 4 ? 512 : speed == 3 ? 64 : 8;
    struct usb_dev *d;

    if (ndevs >= MAX_DEVS) {
        return;
    }
    cc = command(0, 0, T_ENABLE_SLOT << 10, &slot);
    if (cc != CC_SUCCESS || slot == 0 || slot > MAX_SLOTS) {
        uart_puts("[usb] enable slot failed\r\n");
        return;
    }
    i = slot - 1;
    memset(devctx[i], 0, sizeof(devctx[i]));
    memset(ep0ring[i], 0, sizeof(ep0ring[i]));
    ep0_enq[i] = 0;
    ep0_cycle[i] = 1;
    dcbaa[slot] = pa(devctx[i]);

    memset(inctx, 0, sizeof(inctx));
    ictx(0)[1] = 0x3;                                       /* 足すもの: スロットと EP0 */
    sc = ictx(1);
    sc[0] = (speed << 20) | (1u << 27);                     /* 速さ、コンテキストの数 = 1 */
    sc[1] = port << 16;                                     /* ルートハブの口 */
    ep = ictx(2);
    ep[1] = (3 << 1) | (4 << 3) | ((uint32_t)mps << 16);    /* 誤りの回数 3、制御、最大パケット */
    ep[2] = (uint32_t)pa(ep0ring[i]) | 1;                   /* 取り出し位置と DCS */
    ep[3] = (uint32_t)(pa(ep0ring[i]) >> 32);
    ep[4] = 8;                                              /* 平均の TRB の長さ */
    fence_rw();
    cc = command(pa(inctx), 0, (T_ADDRESS_DEV << 10) | (slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        uart_puts("[usb] address device failed\r\n");
        return;
    }
    /* 最初の 8 バイトで EP0 の最大パケットを知り、違えば直す */
    if (control_in(slot, 0x80, 6, 0x0100, 0, ctrl_buf, 8) != 0) {
        uart_puts("[usb] get descriptor failed\r\n");
        return;
    }
    if (ctrl_buf[7] && ctrl_buf[7] != mps && speed < 4) {
        mps = ctrl_buf[7];
        memset(inctx, 0, sizeof(inctx));
        ictx(0)[1] = 0x2;
        ep = ictx(2);
        ep[1] = (3 << 1) | (4 << 3) | ((uint32_t)mps << 16);
        fence_rw();
        command(pa(inctx), 0, (T_EVAL_CTX << 10) | (slot << 24), NULL);
    }
    if (control_in(slot, 0x80, 6, 0x0100, 0, ctrl_buf, 18) != 0) {
        uart_puts("[usb] get descriptor failed\r\n");
        return;
    }
    d = &devs[ndevs++];
    d->port = port;
    d->speed = speed;
    d->slot = slot;
    d->vid = (uint16_t)(ctrl_buf[8] | (ctrl_buf[9] << 8));
    d->pid = (uint16_t)(ctrl_buf[10] | (ctrl_buf[11] << 8));
    d->dev_class = ctrl_buf[4];
    d->mps0 = (uint8_t)mps;
    d->name[0] = 0;
    {
        uint8_t iprod = ctrl_buf[15];
        if (iprod && control_in(slot, 0x80, 6, (uint16_t)(0x0300 | iprod), 0x0409, ctrl_buf, 80) == 0) {
            unsigned n = ctrl_buf[0], k, j = 0;
            for (k = 2; k + 1 < n && k < 80 && j < sizeof(d->name) - 1; k += 2) {
                d->name[j++] = (ctrl_buf[k + 1] == 0 && ctrl_buf[k] >= 0x20 && ctrl_buf[k] < 0x7f)
                             ? (char)ctrl_buf[k] : '?';
            }
            d->name[j] = 0;
        }
    }
    uart_puts("[usb] port ");
    put_dec(port);
    uart_puts(": ");
    put_hex(d->vid, 4);
    uart_puts(":");
    put_hex(d->pid, 4);
    uart_puts(" \"");
    uart_puts(d->name);
    uart_puts("\" speed ");
    uart_puts(speed == 1 ? "full" : speed == 2 ? "low" : speed == 3 ? "high" : "super");
    uart_puts(" slot ");
    put_dec(slot);
    uart_puts("\r\n");
    (void)control_out0;
}

/* 見つかって動けば、つながった装置の数（0 以上）。xHCI がなければ -1 */
int xhci_init(void)
{
    uintptr_t bar;
    unsigned pdev, caplen, nsp, i, port;
    uint32_t hcs2, hcc1;

    if (!rv_pci_find_class(0x0c0330, &bar, &pdev)) {
        return -1;
    }
    cap = bar;
    caplen = rd(cap) & 0xff;
    max_slots = rd(cap + 4) & 0xff;
    max_ports = rd(cap + 4) >> 24;
    hcs2 = rd(cap + 8);
    hcc1 = rd(cap + 0x10);
    ctx_size = (hcc1 & 4) ? 64 : 32;
    dbs = cap + (rd(cap + 0x14) & ~3u);
    rts = cap + (rd(cap + 0x18) & ~0x1fu);
    op = cap + caplen;
    nsp = ((hcs2 >> 27) & 0x1f) | (((hcs2 >> 21) & 0x1f) << 5);
    if (max_slots > MAX_SLOTS) {
        max_slots = MAX_SLOTS;
    }
    if (nsp > 8) {
        uart_puts("[usb] too many scratchpad buffers\r\n");
        return -1;
    }

    /* 止めてリセット */
    wr(op + 0x00, rd(op + 0x00) & ~1u);
    wait_bits(op + 0x04, 1, 1, 100);                        /* HCHalted */
    wr(op + 0x00, 2);                                       /* HCRST */
    if (!wait_bits(op + 0x00, 2, 0, 1000) || !wait_bits(op + 0x04, 1u << 11, 0, 1000)) {
        uart_puts("[usb] reset timeout\r\n");
        return -1;
    }
    wr(op + 0x38, max_slots);                               /* CONFIG: 使うスロットの数 */

    memset(dcbaa, 0, sizeof(dcbaa));
    if (nsp) {
        for (i = 0; i < nsp; i++) {
            memset(scratch_pages[i], 0, 4096);
            scratch_arr[i] = pa(scratch_pages[i]);
        }
        dcbaa[0] = pa(scratch_arr);
    }
    wr64(op + 0x30, pa(dcbaa));                             /* DCBAAP */

    memset(cmd_ring, 0, sizeof(cmd_ring));
    cmd_enq = 0;
    cmd_cycle = 1;
    wr64(op + 0x18, pa(cmd_ring) | 1);                      /* CRCR（RCS = 1） */

    memset(evt_ring, 0, sizeof(evt_ring));
    evt_deq = 0;
    evt_cycle = 1;
    erst[0].base = pa(evt_ring);
    erst[0].size = RING_N;
    erst[0].rsvd = 0;
    wr(rts + 0x20 + 0x08, 1);                               /* ERSTSZ */
    wr64(rts + 0x20 + 0x18, pa(evt_ring));                  /* ERDP */
    wr64(rts + 0x20 + 0x10, pa(erst));                      /* ERSTBA（最後に書く） */
    fence_rw();

    wr(op + 0x00, 1);                                       /* Run */
    if (!wait_bits(op + 0x04, 1, 0, 100)) {
        uart_puts("[usb] start timeout\r\n");
        return -1;
    }
    /* 命令のリングが動くかを、何もしない命令で確かめる */
    if (command(0, 0, T_NOOP_CMD << 10, NULL) != CC_SUCCESS) {
        uart_puts("[usb] command ring does not respond\r\n");
        return -1;
    }
    xhci_ok = 1;
    uart_puts("[usb] xHCI: ");
    put_dec(max_ports);
    uart_puts(" ports, ");
    put_dec(max_slots);
    uart_puts(" slots, context ");
    put_dec(ctx_size);
    uart_puts(" bytes\r\n");

    /* つながった口を見る */
    tk_dly_tsk(20);
    for (port = 1; port <= max_ports; port++) {
        uintptr_t ps = op + 0x400 + 0x10 * (port - 1);
        uint32_t v = rd(ps), speed;
        if (!(v & 1)) {                                     /* CCS: つながっていない */
            continue;
        }
        speed = (v >> 10) & 0xf;
        if (speed != 4) {                                   /* USB 2 の口はリセットして使えるようにする */
            wr(ps, (v & ~0x00fe0002u) | (1u << 4));         /* 変化のビットと PED を書かずに PR */
            if (!wait_bits(ps, 1u << 21, 1u << 21, 200)) {
                continue;
            }
            v = rd(ps);
            wr(ps, (v & ~0x00fe0002u) | (1u << 21));        /* PRC を消す */
            tk_dly_tsk(10);
            v = rd(ps);
            if (!(v & 2)) {                                 /* PED */
                continue;
            }
            speed = (v >> 10) & 0xf;
        }
        enumerate(port, speed);
    }
    return ndevs;
}

/* テスト用: 見つけた装置 */
int rv_usb_count(void)
{
    return ndevs;
}

int rv_usb_info(int i, uint16_t *vid, uint16_t *pid, const char **name)
{
    if (i < 0 || i >= ndevs) {
        return 0;
    }
    *vid = devs[i].vid;
    *pid = devs[i].pid;
    *name = devs[i].name;
    return 1;
}

int rv_usb_present(void)
{
    return xhci_ok;
}

/* ---------------------------------------------------------------- キーボード（HID、作業の 2 番）
 *   設定の記述子から HID のブート用キーボード（クラス 3・サブクラス 1・プロトコル 1）を探し、
 *   SET_CONFIGURATION・SET_PROTOCOL（ブート）のあと、割込みの IN の口を Configure Endpoint で開く。
 *   キーボードの報告（8 バイト: 修飾キー・予備・押されているキー 6 つ）は、usb_task が 8 ミリ秒ごとに
 *   できごとのリングを見て受け取り、新しく押されたキーを文字にして、シリアルと同じ入力の入れ物へ入れる
 *   （シェルにも T88-BASIC にも届く）。配列は JIS。押し続けると 0.5 秒後から 1 秒に 30 回くり返す。 */
extern void rv_uart_inject(int c);

struct hid_kbd {
    int on;
    unsigned slot, epid, mps;
    struct trb ring[RING_N] __attribute__((aligned(64)));
    unsigned enq, cycle;
    uint8_t buf[64] __attribute__((aligned(64)));
    uint8_t prev[8];
    uint8_t rep_key, rep_mod;
    unsigned rep_wait;                  /* くり返しまでの残り（ミリ秒） */
};
static struct hid_kbd kbd __attribute__((aligned(4096)));
static int usb_tid;
volatile unsigned long rv_usb_kbd_reports;

/* JIS 配列: 使用番号（usage）→ 文字（ふつう・シフト） */
static const char jis_n[0x39] = {
    0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '1', '2',
    '3', '4', '5', '6', '7', '8', '9', '0', '\r', 0x1b, 0x7f, '\t', ' ', '-', '^', '@',
    '[', ']', ']', ';', ':', 0, ',', '.', '/'
};
static const char jis_s[0x39] = {
    0, 0, 0, 0, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '!', '"',
    '#', '$', '%', '&', '\'', '(', ')', 0, '\r', 0x1b, 0x7f, '\t', ' ', '=', '~', '`',
    '{', '}', '}', '+', '*', 0, '<', '>', '?'
};

/* 1 つのキーを文字の並びにして out に出す（out(c) を呼ぶ） */
static void key_to_chars(uint8_t mod, uint8_t key, void (*out)(int))
{
    int shift = (mod & 0x22) != 0, ctrl = (mod & 0x11) != 0;
    const char *seq = NULL;
    int c = 0;

    if (key < 0x39) {
        c = shift ? jis_s[key] : jis_n[key];
    } else if (key == 0x87) {           /* ろ（＼ と ＿） */
        c = shift ? '_' : '\\';
    } else if (key == 0x89) {           /* ￥（＼ と ｜） */
        c = shift ? '|' : '\\';
    } else {
        switch (key) {
        case 0x4f: seq = "\033[C"; break;   /* → */
        case 0x50: seq = "\033[D"; break;   /* ← */
        case 0x51: seq = "\033[B"; break;   /* ↓ */
        case 0x52: seq = "\033[A"; break;   /* ↑ */
        case 0x4a: seq = "\033[H"; break;   /* Home */
        case 0x4d: seq = "\033[F"; break;   /* End */
        case 0x4c: seq = "\033[3~"; break;  /* Delete */
        }
    }
    if (seq) {
        while (*seq) {
            out(*seq++);
        }
        return;
    }
    if (!c) {
        return;
    }
    if (ctrl) {
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 1;
        } else if (c >= 'A' && c <= 'Z') {
            c -= 'A' - 1;
        } else if (c == '@') {
            c = 0;
        } else if (c == '[') {
            c = 0x1b;
        } else {
            return;
        }
    }
    out(c);
}

/* 報告 1 つを処理する（新しく押されたキーだけ文字にする） */
extern void rv_evdev_kbd(const uint8_t *r, const uint8_t *prev);
extern void rv_evdev_kbd_repeat(uint8_t hid);
extern void rv_evdev_ptr(int x, int y, int buttons, int wheel);

static void kbd_report(const uint8_t *r, uint8_t *prev, void (*out)(int))
{
    int i, j;
    uint8_t last = 0;
    rv_evdev_kbd(r, prev);              /* /dev/input/event0 にも（押した・離した、Linux のキーの番号） */
    for (i = 2; i < 8; i++) {
        int was = 0;
        if (r[i] < 4) {
            continue;                   /* 0 = なし、1〜3 = 誤り */
        }
        for (j = 2; j < 8; j++) {
            if (prev[j] == r[i]) {
                was = 1;
            }
        }
        if (!was) {
            key_to_chars(r[0], r[i], out);
            last = r[i];
        }
    }
    if (last) {
        kbd.rep_key = last;
        kbd.rep_mod = r[0];
        kbd.rep_wait = 500;
    } else {
        int held = 0;
        for (i = 2; i < 8; i++) {
            if (r[i] == kbd.rep_key) {
                held = 1;
            }
        }
        if (!held) {
            kbd.rep_key = 0;
        }
        kbd.rep_mod = r[0];
    }
    for (i = 0; i < 8; i++) {
        prev[i] = r[i];
    }
}

static void kbd_queue(void)
{
    push(kbd.ring, &kbd.enq, &kbd.cycle, pa(kbd.buf), kbd.mps > 8 ? 8 : kbd.mps,
         (T_NORMAL << 10) | (1 << 5) | (1 << 2));       /* IOC・ISP */
    fence_rw();
    wr(dbs + 4 * kbd.slot, kbd.epid);
}

/* 設定の記述子を読み、HID のキーボードなら口を開く。開けたら 1 */
static int setup_keyboard(struct usb_dev *d)
{
    uint8_t *p, cfgval;
    unsigned total, off, iface = 0xff, epaddr = 0, mps = 0, ival = 0;
    int in_kbd = 0;
    uint32_t *sc, *ep;
    unsigned epid, interval;

    if (control_in(d->slot, 0x80, 6, 0x0200, 0, ctrl_buf, 9) != 0) {
        return 0;
    }
    total = ctrl_buf[2] | (ctrl_buf[3] << 8);
    if (total > sizeof(ctrl_buf)) {
        total = sizeof(ctrl_buf);
    }
    if (control_in(d->slot, 0x80, 6, 0x0200, 0, ctrl_buf, (uint16_t)total) != 0) {
        return 0;
    }
    cfgval = ctrl_buf[5];
    for (off = 0; off + 2 <= total && ctrl_buf[off] >= 2; off += ctrl_buf[off]) {
        p = &ctrl_buf[off];
        if (p[1] == 4) {                                    /* インターフェース */
            in_kbd = p[5] == 3 && p[6] == 1 && p[7] == 1;
            if (in_kbd) {
                iface = p[2];
            }
        } else if (p[1] == 5 && in_kbd && epaddr == 0) {    /* エンドポイント */
            if ((p[2] & 0x80) && (p[3] & 3) == 3) {
                epaddr = p[2];
                mps = (p[4] | (p[5] << 8)) & 0x7ff;
                ival = p[6];
            }
        }
    }
    if (iface == 0xff || epaddr == 0) {
        return 0;
    }
    if (control_out0(d->slot, 0x00, 9, cfgval, 0) != 0) {           /* SET_CONFIGURATION */
        return 0;
    }
    control_out0(d->slot, 0x21, 0x0b, 0, (uint16_t)iface);          /* SET_PROTOCOL（ブート） */
    control_out0(d->slot, 0x21, 0x0a, 0, (uint16_t)iface);          /* SET_IDLE（変わったときだけ報告） */

    epid = 2 * (epaddr & 0x0f) + 1;
    if (d->speed >= 3) {
        interval = ival >= 1 ? ival - 1 : 0;                        /* 2^(n-1) × 125us */
    } else {
        unsigned f = ival ? ival * 8 : 8;
        interval = 0;
        while ((1u << (interval + 1)) <= f) {
            interval++;
        }
    }
    memset(&kbd, 0, sizeof(kbd));
    kbd.slot = d->slot;
    kbd.epid = epid;
    kbd.mps = mps;
    kbd.cycle = 1;
    memset(inctx, 0, sizeof(inctx));
    ictx(0)[1] = 1u | (1u << epid);
    sc = ictx(1);
    memcpy(sc, devctx[d->slot - 1], ctx_size);                      /* 今のスロットの中身を写す */
    sc[0] = (sc[0] & ~(0x1fu << 27)) | (epid << 27);                /* コンテキストの数 */
    ep = ictx(1 + epid);
    ep[0] = interval << 16;
    ep[1] = (3 << 1) | (7 << 3) | (mps << 16);                      /* 割込みの IN */
    ep[2] = (uint32_t)pa(kbd.ring) | 1;
    ep[3] = (uint32_t)(pa(kbd.ring) >> 32);
    ep[4] = mps | (mps << 16);
    fence_rw();
    if (command(pa(inctx), 0, (T_CONFIG_EP << 10) | (d->slot << 24), NULL) != CC_SUCCESS) {
        uart_puts("[usb] configure endpoint failed\r\n");
        return 0;
    }
    kbd.on = 1;
    kbd_queue();
    uart_puts("[usb] keyboard ready (HID boot, JIS layout)\r\n");
    return 1;
}

/* ---------------------------------------------------------------- マウス・タブレット（HID、作業の 3 番）
 *   ブート用マウス（プロトコル 2）なら決まった形（ボタン・X の移動・Y の移動）で読む。
 *   そうでない HID（QEMU の usb-tablet、タッチパッドなど）は、報告の記述子を読んで、
 *   ボタン（ページ 9）・X（0x30）・Y（0x31）・ホイール（0x38）の位置と大きさ、絶対か相対かを知る。
 *   位置は画面全体（1024×768）の座標にしてデスクトップ（kernel/gui/desktop.c）へ渡す。
 *   デスクトップは矢印を動かし、「画面」の窓の中の位置（640×400）を /dev/mouse（linux_fs.c）に渡す。 */
#include "../gui/desktop.h"
#define PTR_W   DESK_W
#define PTR_H   DESK_H

struct hid_field {
    int on;
    unsigned bit, size;                 /* 報告の中の位置（ビット）と大きさ */
    int lmin, lmax;
    int rel;                            /* 相対（移動量）なら 1 */
};
struct hid_ptr {
    int on;
    unsigned slot, epid, mps;
    struct trb ring[RING_N] __attribute__((aligned(64)));
    unsigned enq, cycle;
    uint8_t buf[64] __attribute__((aligned(64)));
    int report_id;                      /* 0 = 報告の番号なし */
    struct hid_field x, y, wheel;
    unsigned btn_bit, btn_count;
};
static struct hid_ptr ptr __attribute__((aligned(4096)));
volatile int rv_mouse_x, rv_mouse_y, rv_mouse_buttons;     /* /dev/mouse に見せるもの（640×400） */
static int scr_x = PTR_W / 2, scr_y = PTR_H / 2, scr_btn;  /* 画面全体の位置とボタン */
volatile unsigned rv_mouse_seq;

static uint32_t bits_at(const uint8_t *b, unsigned len, unsigned bit, unsigned size)
{
    uint32_t v = 0;
    unsigned i;
    for (i = 0; i < size && i < 32; i++) {
        unsigned p = bit + i;
        if (p / 8 < len && (b[p / 8] >> (p % 8)) & 1) {
            v |= 1u << i;
        }
    }
    return v;
}

static int sign_ext(uint32_t v, unsigned size, int lmin)
{
    if (lmin < 0 && size > 0 && size < 32 && (v & (1u << (size - 1)))) {
        return (int)(v | ~((1u << size) - 1));
    }
    return (int)v;
}

/* 報告の記述子を読み、X・Y・ボタン・ホイールを探す（最初に見つかった報告の番号のものだけ）。見つかれば 1 */
static int parse_report_desc(const uint8_t *d, unsigned n, struct hid_ptr *p)
{
    unsigned i = 0, page = 0, rsize = 0, rcount = 0, bit = 0;
    int lmin = 0, lmax = 0, rid = 0, want_rid = -1;
    unsigned usages[16], nusage = 0, umin = 0, umax = 0;
    int have_range = 0;

    memset(&p->x, 0, sizeof(p->x));
    memset(&p->y, 0, sizeof(p->y));
    memset(&p->wheel, 0, sizeof(p->wheel));
    p->btn_count = 0;
    while (i < n) {
        uint8_t pre = d[i];
        unsigned sz = pre & 3, tag = pre & 0xfc, k;
        uint32_t v = 0;
        int sv;
        if (sz == 3) {
            sz = 4;
        }
        if (pre == 0xfe) {                      /* 長い項目は飛ばす */
            if (i + 1 >= n) {
                break;
            }
            i += 3 + d[i + 1];
            continue;
        }
        for (k = 0; k < sz && i + 1 + k < n; k++) {
            v |= (uint32_t)d[i + 1 + k] << (8 * k);
        }
        sv = sz == 1 ? (int8_t)v : sz == 2 ? (int16_t)v : (int)v;
        i += 1 + sz;
        switch (tag) {
        case 0x04: page = v; break;                         /* Usage Page */
        case 0x14: lmin = sv; break;                        /* Logical Minimum */
        case 0x24: lmax = sz == 1 && lmin >= 0 ? (int)(uint8_t)v : sz == 2 && lmin >= 0 ? (int)(uint16_t)v : sv; break;
        case 0x74: rsize = v; break;                        /* Report Size */
        case 0x94: rcount = v; break;                       /* Report Count */
        case 0x84:                                          /* Report ID */
            rid = (int)v;
            if (want_rid >= 0 && rid != want_rid) {
                /* 別の報告: 位置は数えない */
            }
            bit = 0;
            break;
        case 0x08:                                          /* Usage */
            if (nusage < 16) {
                usages[nusage++] = (sz == 4) ? (v & 0xffff) : v;
            }
            break;
        case 0x18: umin = v; have_range = 1; break;         /* Usage Minimum */
        case 0x28: umax = v; have_range = 1; break;         /* Usage Maximum */
        case 0x80: {                                        /* Input */
            int is_const = v & 1, rel = (v >> 2) & 1;
            unsigned f;
            if (want_rid < 0 && !is_const) {
                want_rid = rid;
            }
            if (rid == want_rid && !is_const) {
                for (f = 0; f < rcount; f++) {
                    unsigned u = have_range ? umin + f : (f < nusage ? usages[f] : (nusage ? usages[nusage - 1] : 0));
                    unsigned at = bit + f * rsize;
                    if (page == 9) {
                        if (p->btn_count == 0) {
                            p->btn_bit = at;
                        }
                        if (u >= 1 && u <= 8) {
                            p->btn_count++;
                        }
                    } else if (page == 1) {
                        struct hid_field *hf = u == 0x30 ? &p->x : u == 0x31 ? &p->y : u == 0x38 ? &p->wheel : NULL;
                        if (hf && !hf->on) {
                            hf->on = 1;
                            hf->bit = at;
                            hf->size = rsize;
                            hf->lmin = lmin;
                            hf->lmax = lmax;
                            hf->rel = rel;
                        }
                    }
                    (void)umax;
                }
            }
            if (rid == want_rid) {
                bit += rsize * rcount;
            }
            nusage = 0;
            have_range = 0;
            break;
        }
        case 0xa0: case 0xc0: case 0x90: case 0xb0:         /* Collection・End・Output・Feature */
            if (tag != 0xa0 && tag != 0xc0) {
                nusage = 0;
                have_range = 0;
            } else if (tag == 0xa0) {
                nusage = 0;
                have_range = 0;
            }
            break;
        }
    }
    p->report_id = want_rid > 0 ? want_rid : 0;
    return p->x.on && p->y.on;
}

/* 報告 1 つから位置とボタンを更新する */
static void ptr_report(struct hid_ptr *p, const uint8_t *r, unsigned len)
{
    int x = scr_x, y = scr_y, btn;
    uint32_t vx, vy;

    if (p->report_id) {
        if (len < 1 || r[0] != p->report_id) {
            return;
        }
        r++;
        len--;
    }
    vx = bits_at(r, len, p->x.bit, p->x.size);
    vy = bits_at(r, len, p->y.bit, p->y.size);
    if (p->x.rel) {
        x += sign_ext(vx, p->x.size, p->x.lmin);
    } else if (p->x.lmax > p->x.lmin) {
        x = (int)(((long)(int)vx - p->x.lmin) * PTR_W / ((long)p->x.lmax - p->x.lmin + 1));
    }
    if (p->y.rel) {
        y += sign_ext(vy, p->y.size, p->y.lmin);
    } else if (p->y.lmax > p->y.lmin) {
        y = (int)(((long)(int)vy - p->y.lmin) * PTR_H / ((long)p->y.lmax - p->y.lmin + 1));
    }
    x = x < 0 ? 0 : x >= PTR_W ? PTR_W - 1 : x;
    y = y < 0 ? 0 : y >= PTR_H ? PTR_H - 1 : y;
    scr_x = x;
    scr_y = y;
    btn = p->btn_count ? (int)bits_at(r, len, p->btn_bit, p->btn_count > 8 ? 8 : p->btn_count) : 0;
    scr_btn = btn;
    if (p == &ptr) {
        int wheel = p->wheel.on ? sign_ext(bits_at(r, len, p->wheel.bit, p->wheel.size), p->wheel.size, p->wheel.lmin) : 0;
        desktop_pointer(x, y, btn);     /* 矢印・窓の移動。/dev/mouse の位置とボタンもここで決まる */
        rv_evdev_ptr(x, y, btn, wheel); /* /dev/input/event1 にも（画面全体の位置） */
    }
    rv_mouse_seq++;
}

static void ptr_queue(void)
{
    push(ptr.ring, &ptr.enq, &ptr.cycle, pa(ptr.buf), ptr.mps > 64 ? 64 : ptr.mps,
         (T_NORMAL << 10) | (1 << 5) | (1 << 2));
    fence_rw();
    wr(dbs + 4 * ptr.slot, ptr.epid);
}

static uint8_t rdesc[512] __attribute__((aligned(64)));

/* 設定の記述子を読み、マウス・タブレットなら口を開く。開けたら 1 */
static int setup_pointer(struct usb_dev *d)
{
    uint8_t *p, cfgval;
    unsigned total, off, iface = 0xff, epaddr = 0, mps = 0, ival = 0, rlen = 0, proto = 0;
    int cand = 0;
    uint32_t *sc, *ep;
    unsigned epid, interval;

    if (control_in(d->slot, 0x80, 6, 0x0200, 0, ctrl_buf, 9) != 0) {
        return 0;
    }
    total = ctrl_buf[2] | (ctrl_buf[3] << 8);
    if (total > sizeof(ctrl_buf)) {
        total = sizeof(ctrl_buf);
    }
    if (control_in(d->slot, 0x80, 6, 0x0200, 0, ctrl_buf, (uint16_t)total) != 0) {
        return 0;
    }
    cfgval = ctrl_buf[5];
    for (off = 0; off + 2 <= total && ctrl_buf[off] >= 2; off += ctrl_buf[off]) {
        p = &ctrl_buf[off];
        if (p[1] == 4) {                                    /* インターフェース: HID でキーボードでないもの */
            cand = iface == 0xff && p[5] == 3 && !(p[6] == 1 && p[7] == 1);
            if (cand) {
                iface = p[2];
                proto = p[7];
            }
        } else if (p[1] == 0x21 && cand) {                  /* HID の記述子: 報告の記述子の長さ */
            rlen = p[7] | (p[8] << 8);
        } else if (p[1] == 5 && cand && epaddr == 0) {
            if ((p[2] & 0x80) && (p[3] & 3) == 3) {
                epaddr = p[2];
                mps = (p[4] | (p[5] << 8)) & 0x7ff;
                ival = p[6];
            }
        }
    }
    if (iface == 0xff || epaddr == 0) {
        return 0;
    }
    if (control_out0(d->slot, 0x00, 9, cfgval, 0) != 0) {
        return 0;
    }
    memset(&ptr, 0, sizeof(ptr));
    if (proto == 2) {                                       /* ブート用マウス: 決まった形 */
        control_out0(d->slot, 0x21, 0x0b, 0, (uint16_t)iface);
        ptr.btn_bit = 0; ptr.btn_count = 3;
        ptr.x.on = 1; ptr.x.bit = 8;  ptr.x.size = 8; ptr.x.lmin = -127; ptr.x.lmax = 127; ptr.x.rel = 1;
        ptr.y.on = 1; ptr.y.bit = 16; ptr.y.size = 8; ptr.y.lmin = -127; ptr.y.lmax = 127; ptr.y.rel = 1;
    } else {
        if (rlen == 0 || rlen > sizeof(rdesc)
            || control_in(d->slot, 0x81, 6, 0x2200, (uint16_t)iface, rdesc, (uint16_t)rlen) != 0
            || !parse_report_desc(rdesc, rlen, &ptr)) {
            uart_puts("[usb] pointer: report descriptor not understood\r\n");
            return 0;
        }
    }
    control_out0(d->slot, 0x21, 0x0a, 0, (uint16_t)iface);  /* SET_IDLE */

    epid = 2 * (epaddr & 0x0f) + 1;
    if (d->speed >= 3) {
        interval = ival >= 1 ? ival - 1 : 0;
    } else {
        unsigned f = ival ? ival * 8 : 8;
        interval = 0;
        while ((1u << (interval + 1)) <= f) {
            interval++;
        }
    }
    ptr.slot = d->slot;
    ptr.epid = epid;
    ptr.mps = mps;
    ptr.cycle = 1;
    memset(inctx, 0, sizeof(inctx));
    ictx(0)[1] = 1u | (1u << epid);
    sc = ictx(1);
    memcpy(sc, devctx[d->slot - 1], ctx_size);
    sc[0] = (sc[0] & ~(0x1fu << 27)) | (epid << 27);
    ep = ictx(1 + epid);
    ep[0] = interval << 16;
    ep[1] = (3 << 1) | (7 << 3) | (mps << 16);
    ep[2] = (uint32_t)pa(ptr.ring) | 1;
    ep[3] = (uint32_t)(pa(ptr.ring) >> 32);
    ep[4] = mps | (mps << 16);
    fence_rw();
    if (command(pa(inctx), 0, (T_CONFIG_EP << 10) | (d->slot << 24), NULL) != CC_SUCCESS) {
        uart_puts("[usb] configure endpoint failed\r\n");
        return 0;
    }
    scr_x = PTR_W / 2;
    scr_y = PTR_H / 2;
    rv_mouse_x = APP_W / 2;
    rv_mouse_y = APP_H / 2;
    ptr.on = 1;
    ptr_queue();
    uart_puts(ptr.x.rel ? "[usb] mouse ready (relative)\r\n" : "[usb] tablet ready (absolute)\r\n");
    return 1;
}

int rv_usb_ptr_on(void)
{
    return ptr.on;
}

int rv_usb_ptr_running(void)
{
    uint32_t *ep;
    if (!ptr.on) {
        return 0;
    }
    ep = (uint32_t *)(devctx[ptr.slot - 1] + ptr.epid * ctx_size);
    return (ep[0] & 7) == 1;
}

int rv_usb_ptr_absolute(void)
{
    return ptr.on && !ptr.x.rel;
}

/* 作った報告で、位置への換算を確かめる（QEMU のタブレットの形: ボタン 1 バイト、X・Y 各 2 バイト 0〜32767）。
 * 合っていれば 1。状態はもとに戻す */
int rv_usb_ptr_selftest(void)
{
    static const uint8_t desc[] = {     /* ボタン 3 つ、X・Y は絶対値 0〜32767（16 ビット）、ホイール */
        0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
        0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x35, 0x00, 0x46, 0xff, 0x7f,
        0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
        0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
        0xc0, 0xc0
    };
    static const uint8_t rep[] = { 0x01, 0x00, 0x40, 0x00, 0x20, 0x00 };     /* 左ボタン、X=16384、Y=8192 */
    struct hid_ptr t;
    int sx = rv_mouse_x, sy = rv_mouse_y, sb = rv_mouse_buttons, ok;
    int qx = scr_x, qy = scr_y, qb = scr_btn;
    unsigned ss = rv_mouse_seq;

    memset(&t, 0, sizeof(t));
    ok = parse_report_desc(desc, sizeof(desc), &t) && !t.x.rel && t.x.size == 16 && t.btn_count == 3
         && t.wheel.on && t.wheel.rel;
    if (ok) {
        ptr_report(&t, rep, sizeof(rep));
        ok = scr_x == PTR_W / 2 && scr_y == PTR_H / 4 && scr_btn == 1;    /* X=16384/32768、Y=8192/32768 */
    }
    scr_x = qx;
    scr_y = qy;
    scr_btn = qb;
    rv_mouse_x = sx;
    rv_mouse_y = sy;
    rv_mouse_buttons = sb;
    rv_mouse_seq = ss;
    return ok;
}


/* ================================================================ USB シリアル（USB の I/O ボード用）
 *
 *   FTDI の USB シリアル（VID 0403。QEMU の usb-serial もこれ。Arduino の互換機に多い）と、
 *   CDC-ACM（USB の決まりのシリアル。Raspberry Pi Pico・Arduino Uno など）を 1 つだけ受け持つ。
 *   バルクの IN・OUT の口を開き、57600bps・8N1 にする（Firmata の決まりの速さ）。
 *   受けた文字は rv_usbser_rx（Firmata のドライバが決める）へ渡す。送る文字はリングにためて、usb_task が送る。
 *   CH340 など、ほかの会社の決まりのシリアルの石は、まだ受け持たない。
 */
#define SER_BUF 512
static struct {
    int on, ftdi;
    unsigned slot, ep_in, ep_out, mps_in, mps_out;
    struct trb ring_in[RING_N] __attribute__((aligned(64)));
    struct trb ring_out[RING_N] __attribute__((aligned(64)));
    unsigned enq_in, cyc_in, enq_out, cyc_out;
    int out_busy;
    uint8_t buf_in[SER_BUF] __attribute__((aligned(64)));
    uint8_t buf_out[64] __attribute__((aligned(64)));
} ser;
static uint8_t ser_tx[1024];
static volatile unsigned ser_tx_head, ser_tx_tail;
void (*rv_usbser_rx)(const uint8_t *p, int n);
void (*rv_usbser_tick)(void);
volatile unsigned long rv_usbser_rx_bytes, rv_usbser_tx_bytes;

#define SER_LOCK(st)   ((st) = RV_IRQ_SAVE())
#define SER_UNLOCK(st) RV_IRQ_RESTORE(st)

int rv_usbser_on(void)
{
    return ser.on;
}

/* 送る文字をためる（待たない）。ためられた数を返す */
int rv_usbser_write(const uint8_t *p, int n)
{
    unsigned long st;
    int i;
    SER_LOCK(st);
    for (i = 0; i < n; i++) {
        unsigned next = (ser_tx_head + 1) % sizeof(ser_tx);
        if (next == ser_tx_tail) {
            break;
        }
        ser_tx[ser_tx_head] = p[i];
        ser_tx_head = next;
    }
    SER_UNLOCK(st);
    return i;
}

/* 制御の転送（書く向き、データあり）。成功で 0 */
static int control_out_data(unsigned slot, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index,
                            const void *buf, uint16_t len)
{
    struct trb ev;
    unsigned i = slot - 1;
    uint64_t setup = (uint64_t)req_type | ((uint64_t)req << 8) | ((uint64_t)value << 16)
                   | ((uint64_t)index << 32) | ((uint64_t)len << 48);
    memcpy(ctrl_buf, buf, len);
    push(ep0ring[i], &ep0_enq[i], &ep0_cycle[i], setup, 8, (T_SETUP << 10) | (1 << 6) | (2u << 16));
    push(ep0ring[i], &ep0_enq[i], &ep0_cycle[i], pa(ctrl_buf), len, T_DATA << 10);
    push(ep0ring[i], &ep0_enq[i], &ep0_cycle[i], 0, 0, (T_STATUS << 10) | (1 << 5) | (1u << 16));
    fence_rw();
    wr(dbs + 4 * slot, 1);
    if (!wait_event(T_EV_TRANSFER, slot, &ev, 1000)) {
        return -1;
    }
    return (int)(ev.s >> 24) == CC_SUCCESS ? 0 : -1;
}

static void ser_queue_in(void)
{
    push(ser.ring_in, &ser.enq_in, &ser.cyc_in, pa(ser.buf_in), ser.mps_in,
         (T_NORMAL << 10) | (1 << 5) | (1 << 2));
    fence_rw();
    wr(dbs + 4 * ser.slot, ser.ep_in);
}

/* たまった文字を 1 回分送る（usb_task から） */
static void ser_send(void)
{
    unsigned long st;
    unsigned n = 0, lim = ser.mps_out > 64 ? 64 : ser.mps_out;
    if (!ser.on || ser.out_busy) {
        return;
    }
    SER_LOCK(st);
    while (n < lim && ser_tx_tail != ser_tx_head) {
        ser.buf_out[n++] = ser_tx[ser_tx_tail];
        ser_tx_tail = (ser_tx_tail + 1) % sizeof(ser_tx);
    }
    SER_UNLOCK(st);
    if (n == 0) {
        return;
    }
    ser.out_busy = 1;
    rv_usbser_tx_bytes += n;
    push(ser.ring_out, &ser.enq_out, &ser.cyc_out, pa(ser.buf_out), n, (T_NORMAL << 10) | (1 << 5));
    fence_rw();
    wr(dbs + 4 * ser.slot, ser.ep_out);
}

/* 受けた転送の中身を渡す。FTDI は、最大パケットごとに頭の 2 バイトが状態 */
static void ser_received(unsigned len)
{
    unsigned off;
    if (!ser.ftdi) {
        rv_usbser_rx_bytes += len;
        if (rv_usbser_rx && len) {
            rv_usbser_rx(ser.buf_in, (int)len);
        }
        return;
    }
    for (off = 0; off < len; off += ser.mps_in) {
        unsigned n = len - off > ser.mps_in ? ser.mps_in : len - off;
        if (n > 2) {
            rv_usbser_rx_bytes += n - 2;
            if (rv_usbser_rx) {
                rv_usbser_rx(ser.buf_in + off + 2, (int)(n - 2));
            }
        }
    }
}

/* 設定の記述子を読み、USB シリアルなら口を開く。開けたら 1 */
static int setup_serial(struct usb_dev *d)
{
    uint8_t *p, cfgval;
    unsigned total, off, cur_class = 0xff, comm_if = 0xff;
    unsigned in = 0, out = 0, mps_in = 0, mps_out = 0;
    int ftdi = d->vid == 0x0403, cdc = 0;
    uint32_t *sc, *ep;
    unsigned epi, epo, maxep;

    if (control_in(d->slot, 0x80, 6, 0x0200, 0, ctrl_buf, 9) != 0) {
        return 0;
    }
    total = ctrl_buf[2] | (ctrl_buf[3] << 8);
    if (total > sizeof(ctrl_buf)) {
        total = sizeof(ctrl_buf);
    }
    if (control_in(d->slot, 0x80, 6, 0x0200, 0, ctrl_buf, (uint16_t)total) != 0) {
        return 0;
    }
    cfgval = ctrl_buf[5];
    for (off = 0; off + 2 <= total && ctrl_buf[off] >= 2; off += ctrl_buf[off]) {
        p = &ctrl_buf[off];
        if (p[1] == 4) {                                    /* インターフェース */
            cur_class = p[5];
            if (p[5] == 2 && p[6] == 2 && comm_if == 0xff) {
                comm_if = p[2];                             /* CDC の通信の口（ACM） */
                cdc = 1;
            }
        } else if (p[1] == 5 && (p[3] & 3) == 2) {          /* バルクの口 */
            if ((ftdi && cur_class == 0xff) || (cdc && cur_class == 0x0a) || (ftdi && in == 0 && out == 0)) {
                if ((p[2] & 0x80) && !in) {
                    in = p[2];
                    mps_in = (p[4] | (p[5] << 8)) & 0x7ff;
                } else if (!(p[2] & 0x80) && !out) {
                    out = p[2];
                    mps_out = (p[4] | (p[5] << 8)) & 0x7ff;
                }
            }
        }
    }
    if ((!ftdi && !cdc) || !in || !out || mps_in == 0 || mps_out == 0 || mps_in > SER_BUF) {
        return 0;
    }
    if (control_out0(d->slot, 0x00, 9, cfgval, 0) != 0) {
        return 0;
    }
    epi = 2 * (in & 0x0f) + 1;
    epo = 2 * (out & 0x0f);
    maxep = epi > epo ? epi : epo;
    memset(&ser, 0, sizeof(ser));
    ser.slot = d->slot;
    ser.ep_in = epi;
    ser.ep_out = epo;
    ser.mps_in = mps_in;
    ser.mps_out = mps_out;
    ser.cyc_in = ser.cyc_out = 1;
    ser.ftdi = ftdi;
    memset(inctx, 0, sizeof(inctx));
    ictx(0)[1] = 1u | (1u << epi) | (1u << epo);
    sc = ictx(1);
    memcpy(sc, devctx[d->slot - 1], ctx_size);
    sc[0] = (sc[0] & ~(0x1fu << 27)) | (maxep << 27);
    ep = ictx(1 + epi);
    ep[1] = (3 << 1) | (6 << 3) | (mps_in << 16);           /* Bulk IN */
    ep[2] = (uint32_t)pa(ser.ring_in) | 1;
    ep[3] = (uint32_t)(pa(ser.ring_in) >> 32);
    ep[4] = mps_in;
    ep = ictx(1 + epo);
    ep[1] = (3 << 1) | (2 << 3) | (mps_out << 16);          /* Bulk OUT */
    ep[2] = (uint32_t)pa(ser.ring_out) | 1;
    ep[3] = (uint32_t)(pa(ser.ring_out) >> 32);
    ep[4] = mps_out;
    fence_rw();
    if (command(pa(inctx), 0, (T_CONFIG_EP << 10) | (d->slot << 24), NULL) != CC_SUCCESS) {
        uart_puts("[usb] serial: configure endpoint failed\r\n");
        return 0;
    }
    if (ftdi) {
        control_out0(d->slot, 0x40, 0, 0, 0);               /* リセット */
        control_out0(d->slot, 0x40, 3, 52, 0);              /* 57600bps（3MHz / 52） */
        control_out0(d->slot, 0x40, 4, 0x0008, 0);          /* 8 ビット・パリティなし・ストップ 1 */
        control_out0(d->slot, 0x40, 2, 0, 0);               /* フロー制御なし */
        control_out0(d->slot, 0x40, 1, 0x0303, 0);          /* DTR・RTS を立てる */
    } else {
        static const uint8_t lc[7] = { 0x00, 0xe1, 0x00, 0x00, 0, 0, 8 };      /* 57600bps・8N1 */
        control_out_data(d->slot, 0x21, 0x20, 0, (uint16_t)comm_if, lc, 7);   /* SET_LINE_CODING */
        control_out0(d->slot, 0x21, 0x22, 3, (uint16_t)comm_if);              /* DTR・RTS */
    }
    ser.on = 1;
    ser_queue_in();
    uart_puts(ftdi ? "[usb] serial ready (FTDI), 57600bps\r\n" : "[usb] serial ready (CDC-ACM), 57600bps\r\n");
    return 1;
}

/* 口が動いているか（テスト用） */
int rv_usbser_running(void)
{
    uint32_t *a, *b;
    if (!ser.on) {
        return 0;
    }
    a = (uint32_t *)(devctx[ser.slot - 1] + ser.ep_in * ctx_size);
    b = (uint32_t *)(devctx[ser.slot - 1] + ser.ep_out * ctx_size);
    return (a[0] & 7) == 1 && (b[0] & 7) == 1;
}

/* ================================================================ USB メモリ（大容量の装置）
 *
 *   クラス 8（Mass Storage）・サブクラス 6（SCSI の命令）・プロトコル 0x50（Bulk-Only Transport）の装置を 1 つ受け持つ。
 *   命令の包み（CBW 31 バイト）をバルクの OUT へ送り、データをやりとりし、答え（CSW 13 バイト）を IN から受ける。
 *   使う SCSI の命令: INQUIRY・TEST UNIT READY・REQUEST SENSE・READ CAPACITY(10)・READ(10)・WRITE(10)。
 *   1 回に 64KiB まで（バッファを 64KiB の境に合わせ、TRB 1 つで送る）。止まった（STALL）口は、
 *   Reset Endpoint・Set TR Dequeue Pointer・CLEAR_FEATURE で元に戻す。
 *   読み書きのあいだは usb_lock で輪を独り占めし、できごとは自分で取り出して振り分ける（キーボードなども止めない）。
 */
#define T_RESET_EP      14
#define T_SET_TR_DEQ    16
#define CC_STALL        6

static struct {
    int on;
    unsigned slot, ep_in, ep_out, addr_in, addr_out, mps_in, mps_out, iface;
    struct trb ring_in[RING_N] __attribute__((aligned(64)));
    struct trb ring_out[RING_N] __attribute__((aligned(64)));
    unsigned enq_in, cyc_in, enq_out, cyc_out;
    volatile int done_in, done_out;
    volatile unsigned cc_in, cc_out, left_in, left_out;
    uint32_t tag;
    uint32_t block;                 /* 1 ブロックのバイト数 */
    uint64_t nblocks;
    char vendor[9], product[17];
} msc;
static uint8_t msc_buf[65536] __attribute__((aligned(65536)));
static uint8_t msc_cbw[32] __attribute__((aligned(64)));
static uint8_t msc_csw[64] __attribute__((aligned(64)));
static volatile int usb_busy;       /* できごとの輪を取り出している人がいる */
volatile unsigned long rv_usbms_reads, rv_usbms_writes, rv_usbms_recoveries;

static int poll_event(struct trb *out);

static void usb_lock(void)
{
    for (;;) {
        unsigned long st;
        int got;
        SER_LOCK(st);
        got = !usb_busy;
        usb_busy = 1;
        SER_UNLOCK(st);
        if (got) {
            return;
        }
        tk_dly_tsk(1);
    }
}

static void usb_unlock(void)
{
    usb_busy = 0;
}

/* 旗が立つまで、できごとを取り出して振り分ける。立てば 1 */
static int msc_wait(volatile int *flag, int ms)
{
    int waited = 0;
    for (;;) {
        struct trb ev;
        while (poll_event(&ev)) {
            dispatch(&ev);
        }
        if (*flag) {
            return 1;
        }
        if (waited++ >= ms) {
            return 0;
        }
        tk_dly_tsk(1);
    }
}

/* バルクの転送 1 回。完了コードを返し、*got に実際に運んだバイト数 */
static int msc_xfer(int in, void *buf, unsigned len, unsigned *got, int ms)
{
    if (in) {
        msc.done_in = 0;
        push(msc.ring_in, &msc.enq_in, &msc.cyc_in, pa(buf), len, (T_NORMAL << 10) | (1 << 5) | (1 << 2));
        fence_rw();
        wr(dbs + 4 * msc.slot, msc.ep_in);
        if (!msc_wait(&msc.done_in, ms)) {
            return -1;
        }
        if (got) {
            *got = msc.left_in <= len ? len - msc.left_in : 0;
        }
        return (int)msc.cc_in;
    }
    msc.done_out = 0;
    push(msc.ring_out, &msc.enq_out, &msc.cyc_out, pa(buf), len, (T_NORMAL << 10) | (1 << 5));
    fence_rw();
    wr(dbs + 4 * msc.slot, msc.ep_out);
    if (!msc_wait(&msc.done_out, ms)) {
        return -1;
    }
    if (got) {
        *got = msc.left_out <= len ? len - msc.left_out : 0;
    }
    return (int)msc.cc_out;
}

/* 止まった口を元に戻す */
static void msc_clear_halt(int in)
{
    unsigned epid = in ? msc.ep_in : msc.ep_out;
    struct trb *ring = in ? msc.ring_in : msc.ring_out;
    unsigned enq = in ? msc.enq_in : msc.enq_out, cyc = in ? msc.cyc_in : msc.cyc_out;
    rv_usbms_recoveries++;
    command(0, 0, (T_RESET_EP << 10) | (epid << 16) | (msc.slot << 24), NULL);
    command(pa(&ring[enq]) | (cyc & 1), 0, (T_SET_TR_DEQ << 10) | (epid << 16) | (msc.slot << 24), NULL);
    control_out0(msc.slot, 0x02, 1, 0, (uint16_t)(in ? msc.addr_in : msc.addr_out));   /* CLEAR_FEATURE(ENDPOINT_HALT) */
}

static void msc_reset_recovery(void)
{
    control_out0(msc.slot, 0x21, 0xff, 0, (uint16_t)msc.iface);                         /* Bulk-Only Mass Storage Reset */
    msc_clear_halt(1);
    msc_clear_halt(0);
}

static void put32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32le(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t get32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

/* SCSI の命令を 1 つ。0 = 成功、1 = 命令の失敗（CHECK CONDITION）、−1 = 通信の失敗 */
static int msc_cmd(const uint8_t *cb, unsigned cblen, int in, void *buf, unsigned len)
{
    unsigned got = 0;
    int cc, i;
    for (i = 0; i < 32; i++) {
        msc_cbw[i] = 0;
    }
    put32le(msc_cbw, 0x43425355);                   /* "USBC" */
    put32le(msc_cbw + 4, ++msc.tag);
    put32le(msc_cbw + 8, len);
    msc_cbw[12] = in ? 0x80 : 0x00;
    msc_cbw[13] = 0;                                /* LUN 0 */
    msc_cbw[14] = (uint8_t)cblen;
    for (i = 0; i < (int)cblen; i++) {
        msc_cbw[15 + i] = cb[i];
    }
    cc = msc_xfer(0, msc_cbw, 31, NULL, 2000);
    if (cc != CC_SUCCESS) {
        msc_reset_recovery();
        return -1;
    }
    if (len) {
        cc = msc_xfer(in, buf, len, &got, 5000);
        if (cc == CC_STALL) {
            msc_clear_halt(in);
        } else if (cc != CC_SUCCESS && cc != CC_SHORT) {
            msc_reset_recovery();
            return -1;
        }
    }
    cc = msc_xfer(1, msc_csw, 13, &got, 2000);
    if (cc == CC_STALL) {
        msc_clear_halt(1);
        cc = msc_xfer(1, msc_csw, 13, &got, 2000);
    }
    if ((cc != CC_SUCCESS && cc != CC_SHORT) || got != 13 || get32le(msc_csw) != 0x53425355
        || get32le(msc_csw + 4) != msc.tag || msc_csw[12] == 2) {
        msc_reset_recovery();
        return -1;
    }
    return msc_csw[12] ? 1 : 0;
}

static int msc_sense(void)
{
    static const uint8_t cb[6] = { 0x03, 0, 0, 0, 18, 0 };
    return msc_cmd(cb, 6, 1, msc_buf, 18) == 0 ? (msc_buf[2] & 0x0f) : -1;      /* センスキー */
}

/* n ブロック（n × block ≤ 64KiB）を msc_buf と読み書きする。0 = 成功 */
static int msc_rw(uint64_t lba, unsigned n, int write)
{
    uint8_t cb[10];
    int r, tries;
    if (lba + n > msc.nblocks || n == 0 || n * msc.block > sizeof(msc_buf)) {
        return -1;
    }
    cb[0] = write ? 0x2a : 0x28;
    cb[1] = 0;
    cb[2] = (uint8_t)(lba >> 24); cb[3] = (uint8_t)(lba >> 16); cb[4] = (uint8_t)(lba >> 8); cb[5] = (uint8_t)lba;
    cb[6] = 0;
    cb[7] = (uint8_t)(n >> 8); cb[8] = (uint8_t)n;
    cb[9] = 0;
    for (tries = 0; tries < 3; tries++) {
        r = msc_cmd(cb, 10, !write, msc_buf, n * msc.block);
        if (r == 0) {
            if (write) {
                rv_usbms_writes += n;
            } else {
                rv_usbms_reads += n;
            }
            return 0;
        }
        if (r == 1) {
            msc_sense();
        }
    }
    return -1;
}

static void copy_str(char *dst, const uint8_t *src, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? (char)src[i] : ' ';
    }
    while (n > 0 && dst[n - 1] == ' ') {
        n--;
    }
    dst[n] = 0;
}

/* 設定の記述子を読み、USB メモリなら口を開き、大きさを調べる。使えたら 1 */
static int setup_msc(struct usb_dev *d)
{
    static const uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    static const uint8_t tur[6] = { 0, 0, 0, 0, 0, 0 };
    static const uint8_t cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t *p, cfgval;
    unsigned total, off, iface = 0xff, in = 0, out = 0, mps_in = 0, mps_out = 0, epi, epo, maxep;
    int cand = 0, i, ok = 0;
    uint32_t *sc, *ep;

    if (control_in(d->slot, 0x80, 6, 0x0200, 0, ctrl_buf, 9) != 0) {
        return 0;
    }
    total = ctrl_buf[2] | (ctrl_buf[3] << 8);
    if (total > sizeof(ctrl_buf)) {
        total = sizeof(ctrl_buf);
    }
    if (control_in(d->slot, 0x80, 6, 0x0200, 0, ctrl_buf, (uint16_t)total) != 0) {
        return 0;
    }
    cfgval = ctrl_buf[5];
    for (off = 0; off + 2 <= total && ctrl_buf[off] >= 2; off += ctrl_buf[off]) {
        p = &ctrl_buf[off];
        if (p[1] == 4) {
            cand = iface == 0xff && p[5] == 8 && p[6] == 6 && p[7] == 0x50;
            if (cand) {
                iface = p[2];
            }
        } else if (p[1] == 5 && cand && (p[3] & 3) == 2) {
            if ((p[2] & 0x80) && !in) {
                in = p[2];
                mps_in = (p[4] | (p[5] << 8)) & 0x7ff;
            } else if (!(p[2] & 0x80) && !out) {
                out = p[2];
                mps_out = (p[4] | (p[5] << 8)) & 0x7ff;
            }
        }
    }
    if (iface == 0xff || !in || !out) {
        return 0;
    }
    if (control_out0(d->slot, 0x00, 9, cfgval, 0) != 0) {
        return 0;
    }
    epi = 2 * (in & 0x0f) + 1;
    epo = 2 * (out & 0x0f);
    maxep = epi > epo ? epi : epo;
    memset(&msc, 0, sizeof(msc));
    msc.slot = d->slot;
    msc.ep_in = epi;
    msc.ep_out = epo;
    msc.addr_in = in;
    msc.addr_out = out;
    msc.mps_in = mps_in;
    msc.mps_out = mps_out;
    msc.iface = iface;
    msc.cyc_in = msc.cyc_out = 1;
    memset(inctx, 0, sizeof(inctx));
    ictx(0)[1] = 1u | (1u << epi) | (1u << epo);
    sc = ictx(1);
    memcpy(sc, devctx[d->slot - 1], ctx_size);
    sc[0] = (sc[0] & ~(0x1fu << 27)) | (maxep << 27);
    ep = ictx(1 + epi);
    ep[1] = (3 << 1) | (6 << 3) | (mps_in << 16);           /* Bulk IN */
    ep[2] = (uint32_t)pa(msc.ring_in) | 1;
    ep[3] = (uint32_t)(pa(msc.ring_in) >> 32);
    ep[4] = 3072;
    ep = ictx(1 + epo);
    ep[1] = (3 << 1) | (2 << 3) | (mps_out << 16);          /* Bulk OUT */
    ep[2] = (uint32_t)pa(msc.ring_out) | 1;
    ep[3] = (uint32_t)(pa(msc.ring_out) >> 32);
    ep[4] = 3072;
    fence_rw();
    if (command(pa(inctx), 0, (T_CONFIG_EP << 10) | (d->slot << 24), NULL) != CC_SUCCESS) {
        uart_puts("[usb] storage: configure endpoint failed\r\n");
        return 0;
    }
    msc.on = 1;
    usb_lock();
    if (msc_cmd(inquiry, 6, 1, msc_buf, 36) == 0) {
        copy_str(msc.vendor, msc_buf + 8, 8);
        copy_str(msc.product, msc_buf + 16, 16);
    }
    for (i = 0; i < 10; i++) {                      /* 初めは「状態が変わった」と答えることが多い */
        int r = msc_cmd(tur, 6, 0, NULL, 0);
        if (r == 0) {
            break;
        }
        if (r == 1) {
            msc_sense();
        }
        tk_dly_tsk(20);
    }
    if (msc_cmd(cap, 10, 1, msc_buf, 8) == 0) {
        msc.nblocks = (uint64_t)get32be(msc_buf) + 1;
        msc.block = get32be(msc_buf + 4);
        ok = msc.block >= 512 && msc.block <= 4096 && (msc.block & (msc.block - 1)) == 0;
    }
    usb_unlock();
    if (!ok) {
        msc.on = 0;
        uart_puts("[usb] storage: no medium or unknown block size\r\n");
        return 0;
    }
    uart_puts("[usb] storage ready: ");
    uart_puts(msc.vendor);
    uart_puts(" ");
    uart_puts(msc.product);
    uart_puts(", ");
    put_dec((unsigned)(msc.nblocks * msc.block / 1024));
    uart_puts(" KiB (");
    put_dec(msc.block);
    uart_puts("-byte blocks): /dev/sda\r\n");
    return 1;
}

/* ---- /dev/sda のための口（kernel/sysdepend/cpu/rv64/linux_fs.c から） */
extern int rv_user_copy_in(void *dst, uintptr_t uva, size_t len);
extern int rv_user_copy_out(uintptr_t uva, const void *src, size_t len);

int rv_usbms_present(void)
{
    return msc.on;
}

uint64_t rv_usbms_size(void)
{
    return msc.on ? msc.nblocks * msc.block : 0;
}

uint32_t rv_usbms_block(void)
{
    return msc.block;
}

/* バイトの位置で読み書きする（ブロックの途中からでもよい。書くときは、半端なブロックを読んでから書く）。
 * user = 1 なら buf は利用者の空間。運んだバイト数か、負の errno */
long rv_usbms_rw(uint64_t off, uintptr_t buf, size_t len, int write, int user)
{
    uint64_t size = rv_usbms_size();
    size_t done = 0;
    if (!msc.on) {
        return -19;                                 /* ENODEV */
    }
    if (off >= size) {
        return write && len ? -28 : 0;              /* ENOSPC / ファイルの終わり */
    }
    if (len > size - off) {
        len = (size_t)(size - off);
    }
    usb_lock();
    while (done < len) {
        uint64_t lba = (off + done) / msc.block;
        unsigned head = (unsigned)((off + done) % msc.block);
        unsigned maxn = sizeof(msc_buf) / msc.block;
        size_t want = len - done, span;
        unsigned n = (unsigned)((head + want + msc.block - 1) / msc.block);
        if (n > maxn) {
            n = maxn;
        }
        if (lba + n > msc.nblocks) {
            n = (unsigned)(msc.nblocks - lba);
        }
        span = (size_t)n * msc.block - head;
        if (span > want) {
            span = want;
        }
        if (!write || head != 0 || span % msc.block != 0) {
            if (msc_rw(lba, n, 0) != 0) {           /* 読む。書くときの半端なブロックも */
                break;
            }
        }
        if (write) {
            if (user ? rv_user_copy_in(msc_buf + head, buf + done, span) != 0
                     : (memcpy(msc_buf + head, (const void *)(buf + done), span), 0)) {
                usb_unlock();
                return done ? (long)done : -14;     /* EFAULT */
            }
            if (msc_rw(lba, n, 1) != 0) {
                break;
            }
        } else if (user ? rv_user_copy_out(buf + done, msc_buf + head, span) != 0
                        : (memcpy((void *)(buf + done), msc_buf + head, span), 0)) {
            usb_unlock();
            return done ? (long)done : -14;
        }
        done += span;
    }
    usb_unlock();
    return done ? (long)done : (len ? -5 : 0);      /* EIO */
}

int rv_usbms_running(void)
{
    uint32_t *a, *b;
    if (!msc.on) {
        return 0;
    }
    a = (uint32_t *)(devctx[msc.slot - 1] + msc.ep_in * ctx_size);
    b = (uint32_t *)(devctx[msc.slot - 1] + msc.ep_out * ctx_size);
    return (a[0] & 7) == 1 && (b[0] & 7) == 1;
}

const char *rv_usbms_name(void)
{
    return msc.product;
}

/* できごとを 1 つ取り出す（待たない）。あれば 1 */
static int poll_event(struct trb *out)
{
    struct trb *e = &evt_ring[evt_deq];
    if ((*(volatile uint32_t *)&e->c & 1) != evt_cycle) {
        return 0;
    }
    fence_rw();
    *out = *e;
    evt_deq++;
    if (evt_deq == RING_N) {
        evt_deq = 0;
        evt_cycle ^= 1;
    }
    wr64(rts + 0x20 + 0x18, pa(&evt_ring[evt_deq]) | 8);
    return 1;
}

static void inject(int c)
{
    rv_uart_inject(c);
}

/* できごとを 1 つ、持ち主へ振り分ける */
static void dispatch(const struct trb *ev)
{
    unsigned type = (ev->c >> 10) & 0x3f;
    if (type == T_EV_TRANSFER && kbd.on && (ev->c >> 24) == kbd.slot
        && ((ev->c >> 16) & 0x1f) == kbd.epid) {
        unsigned cc = ev->s >> 24;
        if (cc == CC_SUCCESS || cc == CC_SHORT) {
            rv_usb_kbd_reports++;
            kbd_report(kbd.buf, kbd.prev, inject);
        }
        kbd_queue();
    } else if (type == T_EV_TRANSFER && ptr.on && (ev->c >> 24) == ptr.slot
               && ((ev->c >> 16) & 0x1f) == ptr.epid) {
        unsigned cc = ev->s >> 24;
        if (cc == CC_SUCCESS || cc == CC_SHORT) {
            unsigned want = ptr.mps > 64 ? 64 : ptr.mps, left = ev->s & 0xffffff;
            ptr_report(&ptr, ptr.buf, left <= want ? want - left : want);
        }
        ptr_queue();
    } else if (type == T_EV_TRANSFER && ser.on && (ev->c >> 24) == ser.slot) {
        unsigned epid = (ev->c >> 16) & 0x1f, cc = ev->s >> 24;
        if (epid == ser.ep_in) {
            if (cc == CC_SUCCESS || cc == CC_SHORT) {
                unsigned left = ev->s & 0xffffff;
                ser_received(left <= ser.mps_in ? ser.mps_in - left : 0);
            }
            ser_queue_in();
        } else if (epid == ser.ep_out) {
            ser.out_busy = 0;
        }
    } else if (type == T_EV_TRANSFER && msc.on && (ev->c >> 24) == msc.slot) {
        unsigned epid = (ev->c >> 16) & 0x1f;
        if (epid == msc.ep_in) {
            msc.cc_in = ev->s >> 24;
            msc.left_in = ev->s & 0xffffff;
            msc.done_in = 1;
        } else if (epid == msc.ep_out) {
            msc.cc_out = ev->s >> 24;
            msc.left_out = ev->s & 0xffffff;
            msc.done_out = 1;
        }
    }
}

/* USB の見回り（8 ミリ秒ごと） */
static void usb_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;
    for (;;) {
        struct trb ev;
        usb_lock();
        while (poll_event(&ev)) {
            dispatch(&ev);
        }
        if (ser.on) {
            if (rv_usbser_tick) {
                rv_usbser_tick();
            }
            ser_send();
        }
        usb_unlock();
        if (kbd.on && kbd.rep_key) {
            if (kbd.rep_wait <= 8) {
                key_to_chars(kbd.rep_mod, kbd.rep_key, inject);
                rv_evdev_kbd_repeat(kbd.rep_key);
                kbd.rep_wait = 33;
            } else {
                kbd.rep_wait -= 8;
            }
        }
        tk_dly_tsk(8);
    }
}

/* キーボードを探して開き、見回りのタスクを始める。開けたら 1 */
int usb_hid_start(void)
{
    int i;
    T_CTSK ct;

    if (!xhci_ok) {
        return 0;
    }
    for (i = 0; i < ndevs; i++) {
        if (!kbd.on && setup_keyboard(&devs[i])) {
            continue;
        }
        if (!ptr.on && setup_pointer(&devs[i])) {
            continue;
        }
        if (!msc.on && setup_msc(&devs[i])) {
            continue;
        }
        if (!ser.on) {
            setup_serial(&devs[i]);
        }
    }
    memset(&ct, 0, sizeof(ct));
    ct.tskatr = TA_HLNG | TA_RNG0;
    ct.itskpri = 8;
    ct.stksz = 4096;
    ct.task = (FP)usb_task;
    usb_tid = tk_cre_tsk(&ct);
    if (usb_tid > 0) {
        tk_sta_tsk(usb_tid, 0);
    }
    return kbd.on;
}

/* テスト用 */
int rv_usb_kbd_on(void)
{
    return kbd.on;
}

/* キーボードの口が動いているか（装置の表の EP の状態 = 1 Running） */
int rv_usb_kbd_running(void)
{
    uint32_t *ep;
    if (!kbd.on) {
        return 0;
    }
    ep = (uint32_t *)(devctx[kbd.slot - 1] + kbd.epid * ctx_size);
    return (ep[0] & 7) == 1;
}

static char st_buf[64];
static int st_n;
static void st_out(int c)
{
    if (st_n < (int)sizeof(st_buf) - 1) {
        st_buf[st_n++] = (char)c;
    }
}

/* 報告から文字への変換を、作った報告で確かめる。結果の文字列を返す */
const char *rv_usb_kbd_selftest(void)
{
    static const uint8_t reps[][8] = {
        { 0x00, 0, 0x04, 0, 0, 0, 0, 0 },       /* a */
        { 0x02, 0, 0x04, 0x05, 0, 0, 0, 0 },    /* （a を押したまま）Shift + b → B */
        { 0x00, 0, 0, 0, 0, 0, 0, 0 },          /* 全部離す */
        { 0x02, 0, 0x1f, 0, 0, 0, 0, 0 },       /* Shift + 2 → "（JIS） */
        { 0x00, 0, 0x2f, 0, 0, 0, 0, 0 },       /* @（JIS） */
        { 0x00, 0, 0x34, 0, 0, 0, 0, 0 },       /* :（JIS） */
        { 0x02, 0, 0x87, 0, 0, 0, 0, 0 },       /* Shift + ろ → _ */
        { 0x01, 0, 0x06, 0, 0, 0, 0, 0 },       /* Ctrl + c → 0x03 */
        { 0x00, 0, 0x52, 0, 0, 0, 0, 0 },       /* ↑ */
        { 0x00, 0, 0x28, 0, 0, 0, 0, 0 },       /* Enter */
        { 0x00, 0, 0, 0, 0, 0, 0, 0 },
    };
    uint8_t prev[8] = { 0 };
    unsigned i;
    uint8_t save_key = kbd.rep_key, save_mod = kbd.rep_mod;
    unsigned save_wait = kbd.rep_wait;
    st_n = 0;
    for (i = 0; i < sizeof(reps) / sizeof(reps[0]); i++) {
        kbd_report(reps[i], prev, st_out);
    }
    st_buf[st_n] = 0;
    kbd.rep_key = save_key;
    kbd.rep_mod = save_mod;
    kbd.rep_wait = save_wait;
    return st_buf;
}
