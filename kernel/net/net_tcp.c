/*
 * （bfree_x86_64 の kernel/net_tcp.c をそのまま持ってきたもの。変えたのは見出しの
 *  include のパスと、診断の表示を止めたことだけ。下位は kernel/net/net_ip.c）
 *
 * net_tcp.c - TCP for the B-Free guest LAN (QEMU slirp 10.0.2.0/24 over e1000).
 * See net_tcp.h for the feature list.  Replaces the toy net_tcp_min.c, which had a single
 * outstanding segment, a 2 KiB receive buffer, no FIN handling (EOF was never reported),
 * no listen/accept and printed to the serial port for every received segment.
 *
 * Segments are built and parsed bytewise (big endian on the wire) so there is no struct
 * padding / endianness pitfall; the pseudo header checksum is verified on input.
 */
#include "net_tcp.h"

#include <stddef.h>
#include <stdint.h>

#include "net_ipv4.h"

extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);
extern uint64_t knl_get_current_time(void); /* microseconds since boot */
extern void uart_puts(const char *s);
extern void uart_puthex64(uint64_t val);

/* Tamazusa（RISC-V）: UART はコンソールなので、診断の表示は出さない */
#define uart_puts(s)      ((void)(s))
#define uart_puthex64(v)  ((void)(v))

#ifndef BFREE_TCP_DEEPEN_REXMIT
#define BFREE_TCP_DEEPEN_REXMIT 0
#endif

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

enum {
    ST_CLOSED = 0, ST_LISTEN, ST_SYN_SENT, ST_SYN_RCVD, ST_ESTAB, ST_FIN_WAIT1,
    ST_FIN_WAIT2, ST_CLOSING, ST_TIME_WAIT, ST_CLOSE_WAIT, ST_LAST_ACK
};

/* 4 KiB per direction: the pcb table lives in .bss, which must stay under 0x400000
 * (kernel.lnk ASSERT). 40 small pcbs use less memory than the 12 big ones did, and a
 * web page needs far more connections than 12 ("No file descriptors available"). */
#define RXBUF 4096u
#define TXBUF 4096u
#define OUR_MSS 1460u
#define DEF_MSS 536u
#if BFREE_TCP_DEEPEN_REXMIT
#define RTO_INIT_US 80000u /* deepen smoke: fire before guest read spin budget */
#else
#define RTO_INIT_US 250000u
#endif
#define RTO_MAX_US 4000000u
#define MAX_RETRIES 8u
#define SYN_RETRIES 6u
#define TIME_WAIT_US 1000000ull
#define FW2_ORPHAN_US 15000000ull

/* errno values reported to the socket layer (positive; the API returns negatives) */
#define E_PIPE 32
#define E_NOTCONN 107
#define E_CONNRESET 104
#define E_CONNREFUSED 111
#define E_TIMEDOUT 110
#define E_AGAIN 11
#define E_INPROGRESS 115
#define E_NOBUFS 105
#define E_ADDRINUSE 98
#define E_INVAL 22

typedef struct {
    uint8_t used;
    uint8_t state;
    uint8_t orphan;      /* the application closed its descriptor */
    uint8_t is_listener;
    uint8_t accepted;
    uint8_t fin_queued;  /* application wants to send FIN after the queued data */
    uint8_t fin_sent;
    uint8_t fin_rcvd;
    uint8_t rd_shut;     /* SHUT_RD: discard incoming data */
    uint8_t rto_on;
    uint8_t retries;
#if BFREE_TCP_DEEPEN_REXMIT
    uint8_t deepen_skip_ack; /* smoke: swallow first data ACK once to exercise RTO */
#endif
    int err;             /* pending errno (positive) */
    int parent;          /* listener id for passive connections, else -1 */
    int backlog;
    uint32_t order;
    uint32_t lip, rip;   /* wire format */
    uint16_t lport, rport;
    uint16_t mss;
    uint32_t iss, snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2;
    uint32_t irs, rcv_nxt;
    uint32_t tx_base;    /* sequence number of tx[tx_off] */
    uint32_t tx_off, tx_len;
    uint32_t rx_off, rx_len;
    uint32_t rto_us;
    uint64_t rto_at;
    uint64_t life_at;    /* TIME_WAIT / orphan FIN_WAIT2 expiry (0 = none) */
    uint8_t tx[TXBUF];
    uint8_t rx[RXBUF];
} pcb_t;

static pcb_t g_pcb[BFREE_TCP_MAX];
static uint16_t g_eph = 41000;
static uint32_t g_iss_ctr = 0x1000u;
static uint32_t g_order;
static uint8_t g_seg[64 + 1500];

#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((int32_t)((a) - (b)) >= 0)

static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static inline uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t now_us(void) { return knl_get_current_time(); }

/* one's complement sum of `n` bytes taken as big-endian 16-bit words */
static uint32_t csum_add(uint32_t sum, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i + 1 < n; i += 2) {
        sum += (uint32_t)(((uint32_t)p[i] << 8) | p[i + 1]);
    }
    if (n & 1u) {
        sum += (uint32_t)p[n - 1] << 8;
    }
    return sum;
}

/* `src`/`dst` in wire format: byte 0 of the address is the low byte of the uint32_t. */
static uint32_t csum_pseudo(uint32_t src, uint32_t dst, size_t tcp_len)
{
    uint8_t ph[12];

    ph[0] = (uint8_t)src; ph[1] = (uint8_t)(src >> 8); ph[2] = (uint8_t)(src >> 16); ph[3] = (uint8_t)(src >> 24);
    ph[4] = (uint8_t)dst; ph[5] = (uint8_t)(dst >> 8); ph[6] = (uint8_t)(dst >> 16); ph[7] = (uint8_t)(dst >> 24);
    ph[8] = 0; ph[9] = 6; ph[10] = (uint8_t)(tcp_len >> 8); ph[11] = (uint8_t)tcp_len;
    return csum_add(0, ph, sizeof(ph));
}

static uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)sum;
}

/* Build and transmit one segment from (src ip/port) to (dst ip/port). */
static int seg_out(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport,
                   uint32_t seq, uint32_t ack, uint8_t flags, uint16_t win,
                   int with_mss, const uint8_t *d1, size_t l1, const uint8_t *d2, size_t l2)
{
    uint8_t *h = g_seg;
    size_t hl = with_mss ? 24u : 20u;
    size_t total = hl + l1 + l2;
    uint16_t c;

    if (total > sizeof(g_seg)) {
        return -1;
    }
    memset(h, 0, hl);
    put16(h + 0, sport);
    put16(h + 2, dport);
    put32(h + 4, seq);
    put32(h + 8, ack);
    h[12] = (uint8_t)((hl / 4u) << 4);
    h[13] = flags;
    put16(h + 14, win);
    if (with_mss) {
        h[20] = 2; h[21] = 4; put16(h + 22, (uint16_t)OUR_MSS);
    }
    if (l1) {
        memcpy(h + hl, d1, l1);
    }
    if (l2) {
        memcpy(h + hl + l1, d2, l2);
    }
    c = (uint16_t)~csum_fold(csum_add(csum_pseudo(sip, dip, total), h, total));
    put16(h + 16, c);
    return ipv4_send(dip, 6, h, total);
}

static uint16_t rcv_wnd(const pcb_t *p)
{
    uint32_t w = p->rd_shut ? RXBUF : RXBUF - p->rx_len;

    return (uint16_t)(w > 65535u ? 65535u : w);
}

/* data bytes in the ring that have not been transmitted yet (for the current snd_nxt) */
static uint32_t tx_sent_data(const pcb_t *p)
{
    uint32_t s = p->snd_nxt - p->tx_base;

    if ((int32_t)s < 0) {
        return 0;
    }
    return s > p->tx_len ? p->tx_len : s;
}

static void send_ctl(pcb_t *p, uint8_t flags, uint32_t seq)
{
    (void)seg_out(p->lip, p->rip, p->lport, p->rport, seq,
                  (flags & TCP_ACK) ? p->rcv_nxt : 0, flags, rcv_wnd(p),
                  (flags & TCP_SYN) != 0, 0, 0, 0, 0);
}

static void send_ack(pcb_t *p)
{
    send_ctl(p, TCP_ACK, p->snd_nxt);
}

static void arm_rto(pcb_t *p)
{
    if (!p->rto_on) {
        p->rto_on = 1;
        p->rto_at = now_us() + p->rto_us;
    }
}

/* Reset (RST) in reply to a segment that belongs to no connection. */
static void reply_rst(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport,
                      uint32_t seq, uint32_t ack, uint8_t flags, size_t plen)
{
    if (flags & TCP_RST) {
        return;
    }
    if (flags & TCP_ACK) {
        (void)seg_out(dip, sip, dport, sport, ack, 0, TCP_RST, 0, 0, 0, 0, 0, 0);
    } else {
        uint32_t a = seq + (uint32_t)plen + ((flags & TCP_SYN) ? 1u : 0u) + ((flags & TCP_FIN) ? 1u : 0u);

        (void)seg_out(dip, sip, dport, sport, 0, a, (uint8_t)(TCP_RST | TCP_ACK), 0, 0, 0, 0, 0, 0);
    }
}

static void pcb_free(pcb_t *p)
{
    p->used = 0;
    p->state = ST_CLOSED;
}

static void pcb_reset_fields(pcb_t *p)
{
    memset(p, 0, offsetof(pcb_t, tx));
    p->parent = -1;
    p->rto_us = RTO_INIT_US;
    p->mss = (uint16_t)DEF_MSS;
}

static pcb_t *pcb_alloc(void)
{
    int i;

    for (i = 0; i < BFREE_TCP_MAX; ++i) {
        if (!g_pcb[i].used) {
            pcb_reset_fields(&g_pcb[i]);
            g_pcb[i].used = 1;
            g_pcb[i].order = ++g_order;
            return &g_pcb[i];
        }
    }
    /* Every slot busy: reclaim the oldest TIME_WAIT / closed orphan. */
    for (i = 0; i < BFREE_TCP_MAX; ++i) {
        if (g_pcb[i].orphan && (g_pcb[i].state == ST_TIME_WAIT || g_pcb[i].state == ST_CLOSED)) {
            pcb_reset_fields(&g_pcb[i]);
            g_pcb[i].used = 1;
            g_pcb[i].order = ++g_order;
            return &g_pcb[i];
        }
    }
    return 0;
}

static int pcb_id(const pcb_t *p) { return (int)(p - g_pcb); }

static uint32_t new_iss(void)
{
    g_iss_ctr += 0x3d09u + (uint32_t)(now_us() & 0xffffu) * 17u;
    return g_iss_ctr;
}

static int port_in_use(uint16_t port)
{
    int i;

    for (i = 0; i < BFREE_TCP_MAX; ++i) {
        if (g_pcb[i].used && g_pcb[i].lport == port) {
            return 1;
        }
    }
    return 0;
}

static uint16_t alloc_port(void)
{
    int tries;

    for (tries = 0; tries < 20000; ++tries) {
        uint16_t p = g_eph++;

        if (g_eph >= 60999) {
            g_eph = 41000;
        }
        if (!port_in_use(p)) {
            return p;
        }
    }
    return 0;
}

void tcp_init(void)
{
    int i;

    for (i = 0; i < BFREE_TCP_MAX; ++i) {
        g_pcb[i].used = 0;
        g_pcb[i].state = ST_CLOSED;
    }
    g_eph = 41000;
    g_order = 0;
}

int tcp_active(void)
{
    int i;
    int n = 0;

    for (i = 0; i < BFREE_TCP_MAX; ++i) {
        if (g_pcb[i].used) {
            ++n;
        }
    }
    return n;
}

/* ---- output ---------------------------------------------------------------- */

/* Copy `len` ring bytes starting at data offset `off` (relative to tx_base). Two spans. */
static void tx_spans(const pcb_t *p, uint32_t off, uint32_t len,
                     const uint8_t **d1, size_t *l1, const uint8_t **d2, size_t *l2)
{
    uint32_t start = (p->tx_off + off) % TXBUF;
    uint32_t first = TXBUF - start;

    if (first > len) {
        first = len;
    }
    *d1 = &p->tx[start];
    *l1 = first;
    *d2 = p->tx;
    *l2 = len - first;
}

static int can_output(const pcb_t *p)
{
    return p->state == ST_ESTAB || p->state == ST_CLOSE_WAIT || p->state == ST_FIN_WAIT1 ||
           p->state == ST_CLOSING || p->state == ST_LAST_ACK;
}

static void tcp_output(pcb_t *p)
{
    int guard;

    if (!can_output(p)) {
        return;
    }
    for (guard = 0; guard < 64; ++guard) {
        uint32_t sent = tx_sent_data(p);
        uint32_t unsent = p->tx_len - sent;
        uint32_t inflight = p->snd_nxt - p->snd_una;
        uint32_t usable;
        uint32_t n;
        const uint8_t *d1, *d2;
        size_t l1, l2;
        uint8_t fl;

        if (unsent > 0 && p->snd_nxt == p->tx_base + sent) {
            usable = p->snd_wnd > inflight ? p->snd_wnd - inflight : 0;
            n = unsent;
            if (n > usable) {
                n = usable;
            }
            if (n > p->mss) {
                n = p->mss;
            }
            if (n == 0) {
                if (p->snd_wnd == 0 && inflight == 0) {
                    arm_rto(p); /* persist timer: probe when it fires */
                }
                return;
            }
            tx_spans(p, sent, n, &d1, &l1, &d2, &l2);
            fl = (uint8_t)(TCP_ACK | ((n == unsent) ? TCP_PSH : 0));
            (void)seg_out(p->lip, p->rip, p->lport, p->rport, p->snd_nxt, p->rcv_nxt, fl,
                          rcv_wnd(p), 0, d1, l1, d2, l2);
            p->snd_nxt += n;
            arm_rto(p);
            continue;
        }
        if (unsent == 0 && p->fin_queued && p->snd_nxt == p->tx_base + p->tx_len) {
            send_ctl(p, (uint8_t)(TCP_FIN | TCP_ACK), p->snd_nxt);
            p->snd_nxt += 1;
            p->fin_sent = 1;
            if (p->state == ST_ESTAB) {
                p->state = ST_FIN_WAIT1;
            } else if (p->state == ST_CLOSE_WAIT) {
                p->state = ST_LAST_ACK;
            }
            arm_rto(p);
        }
        return;
    }
}

/* ---- timers ---------------------------------------------------------------- */

static void pcb_abort(pcb_t *p, int err)
{
    if (err) {
        p->err = err;
    }
    p->rto_on = 0;
    p->state = ST_CLOSED;
    if (p->orphan) {
        pcb_free(p);
    }
}

static void tcp_rto_fire(pcb_t *p)
{
    uint32_t inflight = p->snd_nxt - p->snd_una;
    uint32_t sent;
    uint32_t unsent;

    p->rto_on = 0;
    if (p->state == ST_SYN_SENT || p->state == ST_SYN_RCVD) {
        if (++p->retries > SYN_RETRIES) {
            pcb_abort(p, E_TIMEDOUT);
            return;
        }
        p->rto_us = p->rto_us * 2u > RTO_MAX_US ? RTO_MAX_US : p->rto_us * 2u;
        uart_puts("[TCP] rexmit\n");
        send_ctl(p, p->state == ST_SYN_SENT ? (uint8_t)TCP_SYN : (uint8_t)(TCP_SYN | TCP_ACK), p->iss);
        arm_rto(p);
        return;
    }
    if (!can_output(p)) {
        return;
    }
    sent = tx_sent_data(p);
    unsent = p->tx_len - sent;
    if (inflight == 0) {
        if (unsent > 0) {
            if (p->snd_wnd == 0) {
                /* zero-window probe: one byte beyond the window */
                const uint8_t *d1, *d2;
                size_t l1, l2;

                tx_spans(p, sent, 1, &d1, &l1, &d2, &l2);
                uart_puts("[TCP] rexmit\n");
                (void)seg_out(p->lip, p->rip, p->lport, p->rport, p->snd_nxt, p->rcv_nxt,
                              (uint8_t)(TCP_ACK), rcv_wnd(p), 0, d1, l1, d2, l2);
                p->snd_nxt += 1;
                p->rto_us = p->rto_us * 2u > RTO_MAX_US ? RTO_MAX_US : p->rto_us * 2u;
                arm_rto(p);
            } else {
                uart_puts("[TCP] rexmit\n");
                tcp_output(p);
            }
        }
        return;
    }
    if (++p->retries > MAX_RETRIES) {
        pcb_abort(p, E_TIMEDOUT);
        return;
    }
    p->rto_us = p->rto_us * 2u > RTO_MAX_US ? RTO_MAX_US : p->rto_us * 2u;
    /* go-back-N: resend everything from the first unacknowledged byte */
    p->snd_nxt = p->snd_una;
    if (SEQ_LT(p->snd_nxt, p->tx_base)) {
        p->snd_nxt = p->tx_base; /* cannot happen; keep the ring arithmetic sane */
    }
    uart_puts("[TCP] rexmit\n");
    arm_rto(p);
    tcp_output(p);
    if (!p->rto_on) {
        arm_rto(p);
    }
}

void tcp_tick(void)
{
    uint64_t now = now_us();
    int i;
    static uint64_t s_dump_at;
    static uint32_t s_dump_n;
    int dump = 0;

    /* Diagnosis: every 5 s, one line per open connection (state, receive buffer fill, windows,
     * unacked bytes), at most 400 rounds. */
    if (now >= s_dump_at && s_dump_n < 400u) {
        s_dump_at = now + 5000000ULL;
        dump = 1;
        s_dump_n++;
    }

    for (i = 0; i < BFREE_TCP_MAX; ++i) {
        pcb_t *p = &g_pcb[i];

        if (!p->used) {
            continue;
        }
        if (dump && !p->is_listener) {
            uart_puts("[TCPDIAG] id=");
            uart_puthex64((uint64_t)i);
            uart_puts(" st=");
            uart_puthex64((uint64_t)p->state);
            uart_puts(" rx=");
            uart_puthex64((uint64_t)p->rx_len);
            uart_puts(" rcvwnd=");
            uart_puthex64((uint64_t)rcv_wnd(p));
            uart_puts(" sndwnd=");
            uart_puthex64((uint64_t)p->snd_wnd);
            uart_puts(" txlen=");
            uart_puthex64((uint64_t)p->tx_len);
            uart_puts(" rto=");
            uart_puthex64((uint64_t)p->rto_on);
            uart_puts(" rcvnxt=");
            uart_puthex64((uint64_t)p->rcv_nxt);
            uart_puts("\n");
        }
        if (p->rto_on && now >= p->rto_at) {
            tcp_rto_fire(p);
            if (!p->used) {
                continue;
            }
        }
        if (p->life_at != 0 && now >= p->life_at) {
            p->life_at = 0;
            if (p->state == ST_TIME_WAIT) {
                pcb_free(p);
            } else if (p->state == ST_FIN_WAIT2 && p->orphan) {
                pcb_abort(p, 0);
            }
        }
    }
}

/* ---- input ----------------------------------------------------------------- */

static void parse_syn_opts(pcb_t *p, const uint8_t *h, size_t hl)
{
    size_t i = 20;

    p->mss = (uint16_t)DEF_MSS;
    while (i < hl) {
        uint8_t kind = h[i];

        if (kind == 0) {
            break;
        }
        if (kind == 1) {
            ++i;
            continue;
        }
        if (i + 1 >= hl || h[i + 1] < 2 || i + h[i + 1] > hl) {
            break;
        }
        if (kind == 2 && h[i + 1] == 4) {
            uint16_t m = get16(h + i + 2);

            if (m >= 64) {
                p->mss = m > OUR_MSS ? (uint16_t)OUR_MSS : m;
            }
        }
        i += h[i + 1];
    }
}

static void enter_time_wait(pcb_t *p)
{
    p->state = ST_TIME_WAIT;
    p->rto_on = 0;
    p->life_at = now_us() + TIME_WAIT_US;
}

static pcb_t *find_conn(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport)
{
    int i;

    for (i = 0; i < BFREE_TCP_MAX; ++i) {
        pcb_t *p = &g_pcb[i];

        if (p->used && !p->is_listener && p->state != ST_CLOSED &&
            p->lport == dport && p->rport == sport && p->rip == sip && p->lip == dip) {
            return p;
        }
    }
    return 0;
}

static pcb_t *find_listener(uint32_t dip, uint16_t dport)
{
    int i;

    for (i = 0; i < BFREE_TCP_MAX; ++i) {
        pcb_t *p = &g_pcb[i];

        if (p->used && p->is_listener && p->lport == dport && (p->lip == 0 || p->lip == dip)) {
            return p;
        }
    }
    return 0;
}

static int listener_backlog_full(int lid)
{
    int i;
    int n = 0;

    for (i = 0; i < BFREE_TCP_MAX; ++i) {
        if (g_pcb[i].used && g_pcb[i].parent == lid && !g_pcb[i].accepted) {
            ++n;
        }
    }
    return n >= g_pcb[lid].backlog;
}

void tcp_min_input(uint32_t src_ip, const uint8_t *pkt, size_t len)
{
    const uint8_t *h = pkt;
    uint32_t dip = ipv4_get_local_ip();
    size_t hl;
    size_t plen;
    const uint8_t *payload;
    uint16_t sport, dport, win;
    uint32_t seq, ack;
    uint8_t flags;
    pcb_t *p;
    int fin_ok = 0;

    if (len < 20) {
        return;
    }
    hl = (size_t)(h[12] >> 4) * 4u;
    if (hl < 20 || hl > len) {
        return;
    }
    if ((uint16_t)~csum_fold(csum_add(csum_pseudo(src_ip, dip, len), h, len)) != 0) {
        return; /* bad checksum */
    }
    sport = get16(h + 0);
    dport = get16(h + 2);
    seq = get32(h + 4);
    ack = get32(h + 8);
    flags = h[13];
    win = get16(h + 14);
    payload = h + hl;
    plen = len - hl;

    p = find_conn(src_ip, dip, sport, dport);
    if (!p) {
        pcb_t *l;

        if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN && (l = find_listener(dip, dport)) != 0) {
            pcb_t *c;
            int lid = pcb_id(l);

            if (listener_backlog_full(lid) || (c = pcb_alloc()) == 0) {
                return; /* drop: the peer retries the SYN */
            }
            c->parent = lid;
            c->lip = dip;
            c->rip = src_ip;
            c->lport = dport;
            c->rport = sport;
            c->state = ST_SYN_RCVD;
            c->irs = seq;
            c->rcv_nxt = seq + 1;
            c->iss = new_iss();
            c->snd_una = c->iss;
            c->snd_nxt = c->iss + 1;
            c->tx_base = c->iss + 1;
            c->snd_wnd = win;
            c->snd_wl1 = seq;
            c->snd_wl2 = c->iss;
            parse_syn_opts(c, h, hl);
            send_ctl(c, (uint8_t)(TCP_SYN | TCP_ACK), c->iss);
            arm_rto(c);
            return;
        }
        reply_rst(src_ip, dip, sport, dport, seq, ack, flags, plen);
        return;
    }

    /* ---- SYN_SENT ---- */
    if (p->state == ST_SYN_SENT) {
        if (flags & TCP_ACK) {
            if (ack != p->iss + 1) {
                if (!(flags & TCP_RST)) {
                    reply_rst(src_ip, dip, sport, dport, seq, ack, flags, plen);
                }
                return;
            }
        }
        if (flags & TCP_RST) {
            if (flags & TCP_ACK) {
                pcb_abort(p, E_CONNREFUSED);
            }
            return;
        }
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            p->irs = seq;
            p->rcv_nxt = seq + 1;
            p->snd_una = ack;
            p->snd_wnd = win;
            p->snd_wl1 = seq;
            p->snd_wl2 = ack;
            parse_syn_opts(p, h, hl);
            p->state = ST_ESTAB;
            p->rto_on = 0;
            p->retries = 0;
            p->rto_us = RTO_INIT_US;
            send_ack(p);
            tcp_output(p);
        }
        return;
    }

    /* ---- RST ---- */
    if (flags & TCP_RST) {
        uint32_t rw = rcv_wnd(p);

        if (p->state == ST_SYN_RCVD || (SEQ_GEQ(seq, p->rcv_nxt) && SEQ_LT(seq, p->rcv_nxt + (rw ? rw : 1u))) ||
            seq == p->rcv_nxt) {
            if (p->state == ST_SYN_RCVD) {
                pcb_free(p); /* back to LISTEN: the half-open child just disappears */
                return;
            }
            pcb_abort(p, E_CONNRESET);
        }
        return;
    }

    if (p->state == ST_SYN_RCVD) {
        if (flags & TCP_SYN) { /* retransmitted SYN: repeat our SYN|ACK */
            send_ctl(p, (uint8_t)(TCP_SYN | TCP_ACK), p->iss);
            return;
        }
        if (!(flags & TCP_ACK)) {
            return;
        }
        if (ack != p->iss + 1) {
            reply_rst(src_ip, dip, sport, dport, seq, ack, flags, plen);
            return;
        }
        p->state = ST_ESTAB;
        p->snd_una = ack;
        p->snd_wnd = win;
        p->snd_wl1 = seq;
        p->snd_wl2 = ack;
        p->rto_on = 0;
        p->retries = 0;
        p->rto_us = RTO_INIT_US;
        /* fall through: the segment may carry data */
    }

    if (!(flags & TCP_ACK)) {
        return;
    }
    if (flags & TCP_SYN) { /* SYN inside an established connection */
        send_ack(p);
        return;
    }

    /* ---- ACK processing ---- */
    if (SEQ_GT(ack, p->snd_nxt)) {
        send_ack(p); /* acks something we never sent */
        return;
    }
    if (SEQ_LT(p->snd_una, ack)) {
        uint32_t acked = ack - p->snd_una;
        uint32_t data_acked = acked;
        uint32_t base_end = p->tx_base + p->tx_len;

        /* the FIN, when acked, is the sequence number after the last data byte */
        if (SEQ_GT(ack, base_end)) {
            data_acked = base_end - p->tx_base;
        } else {
            data_acked = ack - p->tx_base;
        }
#if BFREE_TCP_DEEPEN_REXMIT
        /* F2 deepen: leave first positive data ACK unapplied so RTO go-back-N runs once.
         * Incoming payload (e.g. PONG) is still processed below. */
        if (!p->deepen_skip_ack && (int32_t)data_acked > 0) {
            p->deepen_skip_ack = 1;
            if (!p->rto_on) {
                arm_rto(p);
            }
        } else
#endif
        {
            if ((int32_t)data_acked > 0 && data_acked <= p->tx_len) {
                p->tx_off = (p->tx_off + data_acked) % TXBUF;
                p->tx_len -= data_acked;
                p->tx_base += data_acked;
            }
            p->snd_una = ack;
            p->retries = 0;
            p->rto_us = RTO_INIT_US;
            if (p->snd_una == p->snd_nxt) {
                p->rto_on = 0;
            } else {
                p->rto_on = 0;
                arm_rto(p);
            }
        }
        (void)acked;
    }
    if (SEQ_LEQ(p->snd_una, ack) &&
        (SEQ_LT(p->snd_wl1, seq) || (p->snd_wl1 == seq && SEQ_LEQ(p->snd_wl2, ack)))) {
        p->snd_wnd = win;
        p->snd_wl1 = seq;
        p->snd_wl2 = ack;
    }
    if (p->fin_sent && SEQ_GT(p->snd_una, p->tx_base + p->tx_len) && p->snd_una == p->snd_nxt) {
        if (p->state == ST_FIN_WAIT1) {
            p->state = ST_FIN_WAIT2;
            if (p->orphan) {
                p->life_at = now_us() + FW2_ORPHAN_US;
            }
        } else if (p->state == ST_CLOSING) {
            enter_time_wait(p);
        } else if (p->state == ST_LAST_ACK) {
            p->state = ST_CLOSED;
            p->rto_on = 0;
            if (p->orphan) {
                pcb_free(p);
                return;
            }
        }
    }

    /* ---- data + FIN ---- */
    if (p->state == ST_ESTAB || p->state == ST_FIN_WAIT1 || p->state == ST_FIN_WAIT2) {
        uint32_t off = 0;
        int need_ack = 0;

        if (plen > 0 || (flags & TCP_FIN)) {
            if (SEQ_LT(seq, p->rcv_nxt)) {
                uint32_t d = p->rcv_nxt - seq; /* bytes of this segment we already have */

                if (d > plen) { /* payload and FIN (at seq+plen) both old: just re-ACK */
                    send_ack(p);
                    tcp_output(p);
                    return;
                }
                off = d;
            } else if (SEQ_GT(seq, p->rcv_nxt)) {
                send_ack(p); /* out of order: duplicate ACK, drop */
                return;
            }
            {
                uint32_t avail = (uint32_t)plen - off;
                uint32_t room = p->rd_shut ? avail : RXBUF - p->rx_len;
                uint32_t n = avail < room ? avail : room;
                const uint8_t *src = payload + off;

                if (n > 0) {
                    if (!p->rd_shut) {
                        uint32_t wpos = (p->rx_off + p->rx_len) % RXBUF;
                        uint32_t first = RXBUF - wpos;

                        if (first > n) {
                            first = n;
                        }
                        memcpy(&p->rx[wpos], src, first);
                        if (n > first) {
                            memcpy(&p->rx[0], src + first, n - first);
                        }
                        p->rx_len += n;
                    }
                    p->rcv_nxt += n;
                    need_ack = 1;
                }
                fin_ok = (flags & TCP_FIN) && n == avail;
                if (avail > n) {
                    need_ack = 1; /* window overflow: the peer must retransmit later */
                }
            }
            if (fin_ok && !p->fin_rcvd) {
                p->rcv_nxt += 1;
                p->fin_rcvd = 1;
                need_ack = 1;
                if (p->state == ST_ESTAB) {
                    p->state = ST_CLOSE_WAIT;
                } else if (p->state == ST_FIN_WAIT1) {
                    p->state = ST_CLOSING;
                } else if (p->state == ST_FIN_WAIT2) {
                    enter_time_wait(p);
                }
            }
            if (need_ack) {
                send_ack(p);
            }
        }
    } else if (p->state == ST_TIME_WAIT) {
        if ((flags & TCP_FIN) || plen > 0) {
            send_ack(p);
            p->life_at = now_us() + TIME_WAIT_US;
        }
    } else if (plen > 0 || (flags & TCP_FIN)) {
        send_ack(p); /* CLOSE_WAIT & co: nothing more to read */
    }
    tcp_output(p);
}

/* ---- application interface ------------------------------------------------- */

int tcp_connect(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port)
{
    pcb_t *p;
    uint16_t lport = src_port;
    int i;

    if (dst_port == 0) {
        return -E_INVAL;
    }
    if (lport == 0) {
        lport = alloc_port();
        if (lport == 0) {
            return -E_ADDRINUSE;
        }
    } else {
        for (i = 0; i < BFREE_TCP_MAX; ++i) {
            if (g_pcb[i].used && !g_pcb[i].is_listener && g_pcb[i].lport == lport &&
                g_pcb[i].rport == dst_port && g_pcb[i].rip == dst_ip) {
                return -E_ADDRINUSE;
            }
        }
    }
    p = pcb_alloc();
    if (!p) {
        return -24; /* EMFILE */
    }
    p->lip = ipv4_get_local_ip();
    p->rip = dst_ip;
    p->lport = lport;
    p->rport = dst_port;
    p->state = ST_SYN_SENT;
    p->iss = new_iss();
    p->snd_una = p->iss;
    p->snd_nxt = p->iss + 1;
    p->tx_base = p->iss + 1;
    p->snd_wnd = 0;
    send_ctl(p, TCP_SYN, p->iss);
    arm_rto(p);
    return pcb_id(p);
}

int tcp_connect_status(int id)
{
    pcb_t *p;

    if (id < 0 || id >= BFREE_TCP_MAX || !g_pcb[id].used) {
        return -9;
    }
    p = &g_pcb[id];
    if (p->err) {
        return -p->err;
    }
    if (p->state == ST_SYN_SENT || p->state == ST_SYN_RCVD) {
        return -E_INPROGRESS;
    }
    if (p->state == ST_CLOSED) {
        return -E_CONNREFUSED;
    }
    return 0;
}

int tcp_listen(uint32_t addr, uint16_t port, int backlog)
{
    pcb_t *p;
    int i;

    if (port == 0) {
        return -E_INVAL;
    }
    for (i = 0; i < BFREE_TCP_MAX; ++i) {
        if (g_pcb[i].used && g_pcb[i].is_listener && g_pcb[i].lport == port &&
            (g_pcb[i].lip == addr || g_pcb[i].lip == 0 || addr == 0)) {
            return -E_ADDRINUSE;
        }
    }
    p = pcb_alloc();
    if (!p) {
        return -24;
    }
    p->is_listener = 1;
    p->state = ST_LISTEN;
    p->lip = addr;
    p->lport = port;
    p->backlog = backlog < 1 ? 1 : (backlog > 8 ? 8 : backlog);
    return pcb_id(p);
}

int tcp_accept(int lid, uint32_t *rip, uint16_t *rport)
{
    int i;
    int best = -1;

    if (lid < 0 || lid >= BFREE_TCP_MAX || !g_pcb[lid].used || !g_pcb[lid].is_listener) {
        return -E_INVAL;
    }
    for (i = 0; i < BFREE_TCP_MAX; ++i) {
        pcb_t *c = &g_pcb[i];

        if (!c->used || c->parent != lid || c->accepted) {
            continue;
        }
        if (c->state == ST_SYN_RCVD) {
            continue;
        }
        if (c->state == ST_CLOSED) { /* reset before the application got to it */
            pcb_free(c);
            continue;
        }
        if (best < 0 || c->order < g_pcb[best].order) {
            best = i;
        }
    }
    if (best < 0) {
        return -E_AGAIN;
    }
    g_pcb[best].accepted = 1;
    g_pcb[best].parent = -1;
    if (rip) {
        *rip = g_pcb[best].rip;
    }
    if (rport) {
        *rport = g_pcb[best].rport;
    }
    return best;
}

int tcp_send(int id, const uint8_t *data, size_t len)
{
    pcb_t *p;
    uint32_t room;
    uint32_t n;
    uint32_t wpos;
    uint32_t first;

    if (id < 0 || id >= BFREE_TCP_MAX || !g_pcb[id].used || g_pcb[id].is_listener) {
        return -E_NOTCONN;
    }
    p = &g_pcb[id];
    if (p->err && p->state == ST_CLOSED) {
        return -p->err;
    }
    if (p->state == ST_SYN_SENT || p->state == ST_SYN_RCVD) {
        return -E_AGAIN;
    }
    if ((p->state != ST_ESTAB && p->state != ST_CLOSE_WAIT) || p->fin_queued) {
        return p->err ? -p->err : -E_PIPE;
    }
    if (len == 0) {
        return 0;
    }
    room = TXBUF - p->tx_len;
    if (room == 0) {
        tcp_output(p);
        return -E_AGAIN;
    }
    n = len < room ? (uint32_t)len : room;
    wpos = (p->tx_off + p->tx_len) % TXBUF;
    first = TXBUF - wpos;
    if (first > n) {
        first = n;
    }
    memcpy(&p->tx[wpos], data, first);
    if (n > first) {
        memcpy(&p->tx[0], data + first, n - first);
    }
    p->tx_len += n;
    tcp_output(p);
    return (int)n;
}

int tcp_recv(int id, uint8_t *buf, size_t len, int peek)
{
    pcb_t *p;
    uint32_t n;
    uint32_t first;
    uint16_t before;

    if (id < 0 || id >= BFREE_TCP_MAX || !g_pcb[id].used || g_pcb[id].is_listener) {
        return -E_NOTCONN;
    }
    p = &g_pcb[id];
    if (p->rx_len == 0) {
        if (p->fin_rcvd) {
            return 0;
        }
        if (p->err) {
            int e = p->err;

            p->err = 0;
            return -e;
        }
        if (p->state == ST_SYN_SENT || p->state == ST_SYN_RCVD) {
            return -E_AGAIN;
        }
        if (p->state == ST_CLOSED) {
            return -E_NOTCONN;
        }
        return -E_AGAIN;
    }
    if (len == 0) {
        return 0;
    }
    n = p->rx_len < len ? p->rx_len : (uint32_t)len;
    first = RXBUF - p->rx_off;
    if (first > n) {
        first = n;
    }
    memcpy(buf, &p->rx[p->rx_off], first);
    if (n > first) {
        memcpy(buf + first, &p->rx[0], n - first);
    }
    if (peek) {
        return (int)n;
    }
    before = rcv_wnd(p);
    p->rx_off = (p->rx_off + n) % RXBUF;
    p->rx_len -= n;
    if (before < OUR_MSS && rcv_wnd(p) >= OUR_MSS && p->state != ST_CLOSED) {
        send_ack(p); /* window update: the sender may be stalled on a closed window */
    }
    return (int)n;
}

int tcp_shutdown(int id, int how)
{
    pcb_t *p;

    if (id < 0 || id >= BFREE_TCP_MAX || !g_pcb[id].used || g_pcb[id].is_listener) {
        return -E_NOTCONN;
    }
    p = &g_pcb[id];
    if (p->state == ST_SYN_SENT || p->state == ST_CLOSED || p->state == ST_SYN_RCVD) {
        return -E_NOTCONN;
    }
    if (how == 0 || how == 2) {
        p->rd_shut = 1;
        p->rx_len = 0;
        p->rx_off = 0;
    }
    if (how == 1 || how == 2) {
        p->fin_queued = 1;
        tcp_output(p);
    }
    return 0;
}

void tcp_close(int id)
{
    pcb_t *p;
    int i;

    if (id < 0 || id >= BFREE_TCP_MAX || !g_pcb[id].used) {
        return;
    }
    p = &g_pcb[id];
    if (p->is_listener) {
        for (i = 0; i < BFREE_TCP_MAX; ++i) {
            pcb_t *c = &g_pcb[i];

            if (c->used && c->parent == id && !c->accepted) {
                if (c->state == ST_ESTAB || c->state == ST_SYN_RCVD) {
                    (void)seg_out(c->lip, c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt,
                                  (uint8_t)(TCP_RST | TCP_ACK), 0, 0, 0, 0, 0, 0);
                }
                pcb_free(c);
            }
        }
        pcb_free(p);
        return;
    }
    switch (p->state) {
    case ST_CLOSED:
    case ST_SYN_SENT:
    case ST_TIME_WAIT:
        pcb_free(p);
        return;
    default:
        break;
    }
    if (p->rx_len > 0 && p->state != ST_LAST_ACK) {
        /* unread data: abortive close, like Linux */
        (void)seg_out(p->lip, p->rip, p->lport, p->rport, p->snd_nxt, p->rcv_nxt,
                      (uint8_t)(TCP_RST | TCP_ACK), 0, 0, 0, 0, 0, 0);
        pcb_free(p);
        return;
    }
    p->rx_len = 0;
    p->orphan = 1;
    p->rd_shut = 1;
    if (p->state == ST_ESTAB || p->state == ST_CLOSE_WAIT || p->state == ST_SYN_RCVD) {
        p->fin_queued = 1;
        if (p->state == ST_SYN_RCVD) {
            p->state = ST_ESTAB; /* unusual: close before the handshake finished */
        }
        tcp_output(p);
    } else if (p->state == ST_FIN_WAIT2) {
        p->life_at = now_us() + FW2_ORPHAN_US;
    }
}

int tcp_poll_flags(int id)
{
    pcb_t *p;
    int f = 0;
    int i;

    if (id < 0 || id >= BFREE_TCP_MAX || !g_pcb[id].used) {
        return TCPF_HUP | TCPF_ERR;
    }
    p = &g_pcb[id];
    if (p->is_listener) {
        for (i = 0; i < BFREE_TCP_MAX; ++i) {
            if (g_pcb[i].used && g_pcb[i].parent == id && !g_pcb[i].accepted &&
                g_pcb[i].state != ST_SYN_RCVD) {
                return TCPF_IN;
            }
        }
        return 0;
    }
    if (p->state == ST_SYN_SENT || p->state == ST_SYN_RCVD) {
        f |= TCPF_CONNECTING;
        if (p->err) {
            f |= TCPF_ERR | TCPF_HUP | TCPF_IN | TCPF_OUT;
        }
        return f;
    }
    if (p->rx_len > 0) {
        f |= TCPF_IN;
    }
    if (p->fin_rcvd) {
        f |= TCPF_IN | TCPF_RDHUP;
        if (p->fin_sent || p->state == ST_CLOSED) {
            f |= TCPF_HUP;
        }
    }
    if (p->err) {
        f |= TCPF_ERR | TCPF_IN;
        if (p->state == ST_CLOSED) {
            f |= TCPF_HUP;
        }
    }
    if (p->state == ST_CLOSED) {
        f |= TCPF_HUP | TCPF_IN;
    }
    if ((p->state == ST_ESTAB || p->state == ST_CLOSE_WAIT) && !p->fin_queued &&
        p->tx_len < TXBUF) {
        f |= TCPF_OUT;
    }
    return f;
}

int tcp_take_error(int id)
{
    int e;

    if (id < 0 || id >= BFREE_TCP_MAX || !g_pcb[id].used) {
        return 0;
    }
    e = g_pcb[id].err;
    g_pcb[id].err = 0;
    return e;
}

int tcp_get_addrs(int id, uint32_t *lip, uint16_t *lport, uint32_t *rip, uint16_t *rport)
{
    if (id < 0 || id >= BFREE_TCP_MAX || !g_pcb[id].used) {
        return -9;
    }
    if (lip) {
        *lip = g_pcb[id].lip ? g_pcb[id].lip : ipv4_get_local_ip();
    }
    if (lport) {
        *lport = g_pcb[id].lport;
    }
    if (rip) {
        *rip = g_pcb[id].rip;
    }
    if (rport) {
        *rport = g_pcb[id].rport;
    }
    return 0;
}

int tcp_rx_avail(int id)
{
    if (id < 0 || id >= BFREE_TCP_MAX || !g_pcb[id].used) {
        return 0;
    }
    return (int)g_pcb[id].rx_len;
}

int tcp_tx_free(int id)
{
    if (id < 0 || id >= BFREE_TCP_MAX || !g_pcb[id].used) {
        return 0;
    }
    return (int)(TXBUF - g_pcb[id].tx_len);
}
