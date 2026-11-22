/*
 * net_ip.c — Tamazusa（RISC-V）のネットワークの下の層（S-mode + MMU のみ）
 *
 *   イーサネット・ARP・IPv4・ICMP（echo）・UDP。TCP は net_tcp.c（x86_64 版から）。
 *   アドレスは QEMU のユーザーモードの LAN に合わせて固定:
 *     自分 10.0.2.15/24、出口（ゲートウェイ）10.0.2.2、名前解決 10.0.2.3
 *   127.x.x.x と自分あての送信は、LAN に出さずに受信の列へ戻す（ループバック）。
 *
 *   受信を見るタスク（net_task、優先度 9）が、LAN の割込みか 10ms ごとに起き、
 *   届いたフレームと戻した送信を処理し、TCP の時計（tcp_tick）を進める。
 *   ネットワークの状態は 1 つのミューテックス（net_lock）で守る。システムコール
 *   （linux_net.c）も同じロックの中で TCP・UDP を呼ぶ。待っているプロセスは
 *   net_wait で眠り、何か処理されると起こされる。
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>
#include <libstr.h>
#include "net_ipv4.h"
#include "net_tcp.h"
#include <rv_priv.h>

extern void uart_puts(const char *s);
extern int  virtio_net_init(uint8_t mac[6]);
extern int  virtio_net_recv(uint8_t *buf, unsigned max);
extern int  virtio_net_send(const uint8_t *frame, unsigned len);
extern void virtio_net_set_waiter(int tid);
extern ER tk_slp_tsk(TMO tmout);
extern ER tk_wup_tsk(ID tskid);
extern ID tk_get_tid(void);

#define MY_IP       NET_IP(10, 0, 2, 15)
#define GW_IP       NET_IP(10, 0, 2, 2)
#define NETMASK     NET_IP(255, 255, 255, 0)
#define ETH_HDR     14
#define FRAME_MAX   1514
#define IP_MAX      (FRAME_MAX - ETH_HDR)

static uint8_t  my_mac[6];
static int      nic_ok;
static ID       net_mtx;
static ID       net_tid;
static uint16_t ip_id;
volatile unsigned long rv_net_ip_in, rv_net_ip_out, rv_net_loop;   /* テスト用 */

/* ---- 小道具 ---- */

static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline void put_ip(uint8_t *p, uint32_t ip) { p[0] = (uint8_t)ip; p[1] = (uint8_t)(ip >> 8); p[2] = (uint8_t)(ip >> 16); p[3] = (uint8_t)(ip >> 24); }
static inline uint32_t get_ip(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint32_t csum_add(uint32_t sum, const uint8_t *p, size_t n)
{
    while (n > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        n -= 2;
    }
    if (n) {
        sum += (uint32_t)(p[0] << 8);
    }
    return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

uint64_t knl_get_current_time(void)
{
    uint64_t t;
    t = RV_RDTIME();
    return t / 10;                      /* QEMU virt の time は 10MHz */
}

/* net_tcp.c の診断用（表示はしない） */
void uart_puthex64(uint64_t v) { (void)v; }

uint32_t ipv4_get_local_ip(void) { return MY_IP; }

uint32_t net_map_loopback(uint32_t ip)
{
    return ((ip & 0xff) == 127) ? MY_IP : ip;
}

int net_has_nic(void) { return nic_ok; }

/* ---- ロックと待ち合わせ ---- */

void net_lock(void)   { tk_loc_mtx(net_mtx, TMO_FEVR); }
void net_unlock(void) { tk_unl_mtx(net_mtx); }

#define NWAIT 16
static volatile ID waiters[NWAIT];

void net_wait(int ms)
{
    ID me = tk_get_tid();
    int i, slot = -1;

    tk_dis_dsp();
    for (i = 0; i < NWAIT; i++) {
        if (waiters[i] == 0) {
            waiters[i] = me;
            slot = i;
            break;
        }
    }
    tk_ena_dsp();
    tk_slp_tsk(ms > 0 ? ms : 1);
    if (slot >= 0) {
        waiters[slot] = 0;
    }
}

static void wake_waiters(void)
{
    int i;
    for (i = 0; i < NWAIT; i++) {
        ID t = waiters[i];
        if (t > 0) {
            tk_wup_tsk(t);
        }
    }
}

/* ---- ループバックの列（自分あての IP パケット） ---- */

#define LOOPQ 32
static uint8_t  loop_buf[LOOPQ][IP_MAX];
static uint16_t loop_len[LOOPQ];
static unsigned loop_head, loop_tail;

static void loop_put(const uint8_t *pkt, size_t len)
{
    unsigned next = (loop_head + 1) % LOOPQ;
    if (next == loop_tail || len > IP_MAX) {
        return;                         /* いっぱい: 捨てる（TCP が送り直す） */
    }
    memcpy(loop_buf[loop_head], pkt, len);
    loop_len[loop_head] = (uint16_t)len;
    loop_head = next;
    rv_net_loop++;
}

/* ---- ARP ---- */

#define ARPN 16
struct arp_ent {
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  valid;
    uint16_t pend_len;                  /* MAC が分かったら送るもの（1 つ） */
    uint8_t  pend[IP_MAX];
};
static struct arp_ent arp_tab[ARPN];
static unsigned arp_rr;

static void eth_send(const uint8_t *dmac, uint16_t type, const uint8_t *payload, size_t len)
{
    static uint8_t f[FRAME_MAX];
    if (!nic_ok || len > IP_MAX) {
        return;
    }
    memcpy(f, dmac, 6);
    memcpy(f + 6, my_mac, 6);
    put16(f + 12, type);
    memcpy(f + ETH_HDR, payload, len);
    if (len + ETH_HDR < 60) {
        memset(f + ETH_HDR + len, 0, 60 - ETH_HDR - len);
        len = 60 - ETH_HDR;
    }
    (void)virtio_net_send(f, (unsigned)(len + ETH_HDR));
}

static void arp_send(uint16_t op, const uint8_t *tmac, uint32_t tip)
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t a[28];
    put16(a + 0, 1);                    /* イーサネット */
    put16(a + 2, 0x0800);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, my_mac, 6);
    put_ip(a + 14, MY_IP);
    memcpy(a + 18, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : tmac, 6);
    put_ip(a + 24, tip);
    eth_send(op == 1 ? bcast : tmac, 0x0806, a, sizeof(a));
}

static struct arp_ent *arp_find(uint32_t ip)
{
    int i;
    for (i = 0; i < ARPN; i++) {
        if (arp_tab[i].ip == ip && (arp_tab[i].valid || arp_tab[i].pend_len)) {
            return &arp_tab[i];
        }
    }
    return NULL;
}

static void arp_learn(uint32_t ip, const uint8_t *mac)
{
    struct arp_ent *e = arp_find(ip);
    if (e == NULL) {
        e = &arp_tab[arp_rr++ % ARPN];
        e->pend_len = 0;
    }
    e->ip = ip;
    memcpy(e->mac, mac, 6);
    e->valid = 1;
    if (e->pend_len) {
        eth_send(e->mac, 0x0800, e->pend, e->pend_len);
        e->pend_len = 0;
    }
}

static void arp_input(const uint8_t *a, size_t len)
{
    uint32_t sip, tip;
    if (len < 28 || get16(a) != 1 || get16(a + 2) != 0x0800) {
        return;
    }
    sip = get_ip(a + 14);
    tip = get_ip(a + 24);
    if (tip != MY_IP) {
        return;
    }
    arp_learn(sip, a + 8);
    if (get16(a + 6) == 1) {
        arp_send(2, a + 8, sip);        /* 問い合わせに答える */
    }
}

/* IP パケットを次の行き先へ（MAC が分からなければ問い合わせて、1 つだけ待たせる） */
static void ip_output_lan(uint32_t dst, const uint8_t *pkt, size_t len)
{
    uint32_t hop = ((dst & NETMASK) == (MY_IP & NETMASK)) ? dst : GW_IP;
    struct arp_ent *e;

    if (dst == 0xffffffffU) {
        static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        eth_send(bcast, 0x0800, pkt, len);
        return;
    }
    e = arp_find(hop);
    if (e != NULL && e->valid) {
        eth_send(e->mac, 0x0800, pkt, len);
        return;
    }
    if (e == NULL) {
        e = &arp_tab[arp_rr++ % ARPN];
        e->ip = hop;
        e->valid = 0;
    }
    memcpy(e->pend, pkt, len);
    e->pend_len = (uint16_t)len;
    arp_send(1, NULL, hop);
}

/* ---- IPv4 ---- */

int ipv4_send(uint32_t dst, uint8_t proto, const uint8_t *data, size_t len)
{
    static uint8_t pkt[IP_MAX];
    uint32_t src = MY_IP;

    if (len + 20 > IP_MAX) {
        return -1;
    }
    if ((dst & 0xff) == 127) {
        src = dst;
    }
    pkt[0] = 0x45;
    pkt[1] = 0;
    put16(pkt + 2, (uint16_t)(len + 20));
    put16(pkt + 4, ip_id++);
    put16(pkt + 6, 0x4000);             /* 分割しない */
    pkt[8] = 64;
    pkt[9] = proto;
    put16(pkt + 10, 0);
    put_ip(pkt + 12, src);
    put_ip(pkt + 16, dst);
    put16(pkt + 10, csum_fold(csum_add(0, pkt, 20)));
    memcpy(pkt + 20, data, len);
    rv_net_ip_out++;
    if (dst == MY_IP || (dst & 0xff) == 127) {
        loop_put(pkt, len + 20);
    } else {
        ip_output_lan(dst, pkt, len + 20);
    }
    return 0;
}

static uint32_t pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, size_t len)
{
    uint8_t ph[12];
    put_ip(ph, src);
    put_ip(ph + 4, dst);
    ph[8] = 0;
    ph[9] = proto;
    put16(ph + 10, (uint16_t)len);
    return csum_add(0, ph, 12);
}

/* ---- UDP ---- */

#define UDPQ 8
struct udp_sock {
    int used;
    uint32_t lip;
    uint16_t lport;
    unsigned head, tail;
    uint16_t len[UDPQ];
    uint32_t sip[UDPQ];
    uint16_t sport[UDPQ];
    uint8_t  data[UDPQ][1472];
};
static struct udp_sock udps[NET_UDP_MAX];
static uint16_t udp_eph = 50000;

int udp_open(void)
{
    int i;
    for (i = 0; i < NET_UDP_MAX; i++) {
        if (!udps[i].used) {
            memset(&udps[i], 0, sizeof(udps[i]) - sizeof(udps[i].data));
            udps[i].used = 1;
            return i;
        }
    }
    return -24;                         /* EMFILE */
}

static int udp_port_used(uint16_t port)
{
    int i;
    for (i = 0; i < NET_UDP_MAX; i++) {
        if (udps[i].used && udps[i].lport == port) {
            return 1;
        }
    }
    return 0;
}

int udp_bind(int u, uint32_t ip, uint16_t port)
{
    if (u < 0 || u >= NET_UDP_MAX || !udps[u].used) {
        return -9;
    }
    if (port == 0) {
        do {
            port = udp_eph++;
            if (udp_eph < 50000) {
                udp_eph = 50000;
            }
        } while (udp_port_used(port));
    } else if (udp_port_used(port)) {
        return -98;                     /* EADDRINUSE */
    }
    udps[u].lip = ip;
    udps[u].lport = port;
    return 0;
}

int udp_local(int u, uint32_t *ip, uint16_t *port)
{
    *ip = udps[u].lip ? udps[u].lip : MY_IP;
    *port = udps[u].lport;
    return 0;
}

int udp_sendto(int u, uint32_t dip, uint16_t dport, const uint8_t *buf, size_t len)
{
    static uint8_t seg[1480];
    uint32_t src;
    uint16_t c;

    if (len > 1472) {
        return -90;                     /* EMSGSIZE */
    }
    if (udps[u].lport == 0) {
        udp_bind(u, 0, 0);
    }
    src = ((dip & 0xff) == 127) ? dip : MY_IP;
    put16(seg, udps[u].lport);
    put16(seg + 2, dport);
    put16(seg + 4, (uint16_t)(len + 8));
    put16(seg + 6, 0);
    memcpy(seg + 8, buf, len);
    c = csum_fold(csum_add(pseudo_sum(src, dip, 17, len + 8), seg, len + 8));
    put16(seg + 6, c ? c : 0xffff);
    ipv4_send(dip, 17, seg, len + 8);
    return (int)len;
}

int udp_recv(int u, uint8_t *buf, size_t len, uint32_t *sip, uint16_t *sport, int peek)
{
    struct udp_sock *s = &udps[u];
    unsigned t = s->tail;
    size_t n;
    if (s->head == t) {
        return -11;                     /* EAGAIN */
    }
    n = s->len[t] < len ? s->len[t] : len;
    memcpy(buf, s->data[t], n);
    if (sip) *sip = s->sip[t];
    if (sport) *sport = s->sport[t];
    if (!peek) {
        s->tail = (t + 1) % UDPQ;
    }
    return (int)n;
}

int udp_readable(int u) { return udps[u].head != udps[u].tail; }

void udp_close(int u)
{
    if (u >= 0 && u < NET_UDP_MAX) {
        udps[u].used = 0;
        udps[u].lport = 0;
    }
}

static void udp_input(uint32_t sip, uint32_t dip, const uint8_t *seg, size_t len)
{
    uint16_t dport, ulen;
    int i;
    if (len < 8) {
        return;
    }
    ulen = get16(seg + 4);
    if (ulen < 8 || ulen > len) {
        return;
    }
    dport = get16(seg + 2);
    for (i = 0; i < NET_UDP_MAX; i++) {
        struct udp_sock *s = &udps[i];
        unsigned next;
        if (!s->used || s->lport != dport || (s->lip != 0 && s->lip != dip)) {
            continue;
        }
        next = (s->head + 1) % UDPQ;
        if (next == s->tail) {
            return;                     /* いっぱい: 捨てる */
        }
        s->len[s->head] = (uint16_t)(ulen - 8);
        s->sip[s->head] = sip;
        s->sport[s->head] = get16(seg);
        memcpy(s->data[s->head], seg + 8, ulen - 8u);
        s->head = next;
        return;
    }
}

/* ---- ICMP ---- */

#define ICMPN 4
#define ICMPQ 8
struct icmp_sock {
    int used, raw;
    unsigned head, tail;
    uint16_t len[ICMPQ];
    uint32_t sip[ICMPQ];
    uint8_t  data[ICMPQ][1500];
};
static struct icmp_sock icmps[ICMPN];

int icmp_open(int raw)
{
    int i;
    for (i = 0; i < ICMPN; i++) {
        if (!icmps[i].used) {
            icmps[i].used = 1;
            icmps[i].raw = raw;
            icmps[i].head = icmps[i].tail = 0;
            return i;
        }
    }
    return -24;
}

int icmp_sendto(int s, uint32_t dip, const uint8_t *buf, size_t len)
{
    static uint8_t m[1480];
    (void)s;
    if (len < 8 || len > sizeof(m)) {
        return -22;
    }
    memcpy(m, buf, len);
    if (!icmps[s].raw) {                /* ping のソケット: チェックサムはカーネルが付ける */
        put16(m + 2, 0);
        put16(m + 2, csum_fold(csum_add(0, m, len)));
    }
    ipv4_send(dip, 1, m, len);
    return (int)len;
}

int icmp_recv(int s, uint8_t *buf, size_t len, uint32_t *sip, int peek)
{
    struct icmp_sock *k = &icmps[s];
    unsigned t = k->tail;
    size_t n;
    if (k->head == t) {
        return -11;
    }
    n = k->len[t] < len ? k->len[t] : len;
    memcpy(buf, k->data[t], n);
    if (sip) *sip = k->sip[t];
    if (!peek) {
        k->tail = (t + 1) % ICMPQ;
    }
    return (int)n;
}

int icmp_readable(int s) { return icmps[s].head != icmps[s].tail; }
void icmp_close(int s) { if (s >= 0 && s < ICMPN) icmps[s].used = 0; }

static void icmp_input(uint32_t sip, const uint8_t *ip, size_t iplen, const uint8_t *m, size_t len)
{
    int i;
    if (len < 8 || csum_fold(csum_add(0, m, len)) != 0) {
        return;
    }
    if (m[0] == 8) {                    /* echo の要求 → 答える */
        static uint8_t r[1480];
        if (len > sizeof(r)) {
            return;
        }
        memcpy(r, m, len);
        r[0] = 0;
        put16(r + 2, 0);
        put16(r + 2, csum_fold(csum_add(0, r, len)));
        ipv4_send(sip, 1, r, len);
        return;
    }
    if (m[0] != 0) {
        return;
    }
    for (i = 0; i < ICMPN; i++) {       /* echo の答え → ping のソケットへ */
        struct icmp_sock *k = &icmps[i];
        unsigned next;
        const uint8_t *src = k->raw ? ip : m;
        size_t n = k->raw ? iplen : len;
        if (!k->used) {
            continue;
        }
        next = (k->head + 1) % ICMPQ;
        if (next == k->tail || n > sizeof(k->data[0])) {
            continue;
        }
        memcpy(k->data[k->head], src, n);
        k->len[k->head] = (uint16_t)n;
        k->sip[k->head] = sip;
        k->head = next;
    }
}

/* ---- 受信 ---- */

static void ip_input(const uint8_t *p, size_t len)
{
    size_t hl, tot;
    uint32_t sip, dip;

    if (len < 20 || (p[0] >> 4) != 4) {
        return;
    }
    hl = (size_t)(p[0] & 15) * 4;
    tot = get16(p + 2);
    if (hl < 20 || tot < hl || tot > len || csum_fold(csum_add(0, p, hl)) != 0) {
        return;
    }
    if ((get16(p + 6) & 0x3fff) != 0) {
        return;                         /* 分割されたものはまだ扱わない */
    }
    sip = get_ip(p + 12);
    dip = get_ip(p + 16);
    if (dip != MY_IP && (dip & 0xff) != 127 && dip != 0xffffffffU) {
        return;
    }
    rv_net_ip_in++;
    switch (p[9]) {
    case 1:  icmp_input(sip, p, tot, p + hl, tot - hl); break;
    case 6:  tcp_min_input(sip, p + hl, tot - hl); break;
    case 17: udp_input(sip, dip, p + hl, tot - hl); break;
    default: break;
    }
}

static void eth_input(const uint8_t *f, size_t len)
{
    uint16_t type;
    if (len < ETH_HDR) {
        return;
    }
    type = get16(f + 12);
    if (type == 0x0806) {
        arp_input(f + ETH_HDR, len - ETH_HDR);
    } else if (type == 0x0800) {
        ip_input(f + ETH_HDR, len - ETH_HDR);
    }
}

/* 届いたもの・戻したものをすべて処理する（ロックの中）。処理した数を返す */
static int net_poll_locked(void)
{
    static uint8_t frame[FRAME_MAX];
    int n = 0, r;

    while (nic_ok && (r = virtio_net_recv(frame, sizeof(frame))) != 0) {
        if (r > 0) {
            eth_input(frame, (size_t)r);
        }
        n++;
    }
    while (loop_tail != loop_head) {
        static uint8_t pkt[IP_MAX];
        size_t l = loop_len[loop_tail];
        memcpy(pkt, loop_buf[loop_tail], l);
        loop_tail = (loop_tail + 1) % LOOPQ;
        ip_input(pkt, l);
        n++;
    }
    return n;
}

static void net_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;
    virtio_net_set_waiter(tk_get_tid());
    for (;;) {
        int n;
        tk_slp_tsk(10);                 /* LAN の割込みか 10ms */
        net_lock();
        n = net_poll_locked();
        tcp_tick();
        n += net_poll_locked();         /* tick の送り直しがループバックにあれば */
        net_unlock();
        if (n > 0) {
            wake_waiters();
        }
    }
}

/* システムコールのあと（ロックを外す前）: ループバックのものはすぐ処理する */
void net_kick(void)
{
    if (loop_tail != loop_head && net_tid > 0) {
        tk_wup_tsk(net_tid);
    }
}

void net_init(void)
{
    T_CMTX cm;
    T_CTSK ct;

    memset(&cm, 0, sizeof(cm));
    cm.mtxatr = TA_TFIFO;
    net_mtx = tk_cre_mtx(&cm);
    tcp_init();
    nic_ok = virtio_net_init(my_mac);
    if (nic_ok) {
        uart_puts("[net] virtio-net up: 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3\r\n");
    } else {
        uart_puts("[net] no network card (loopback 127.0.0.1 only)\r\n");
    }
    memset(&ct, 0, sizeof(ct));
    ct.tskatr = TA_HLNG | TA_RNG0;
    ct.itskpri = 9;
    ct.stksz = 8192;
    ct.task = (FP)net_task;
    net_tid = tk_cre_tsk(&ct);
    if (net_tid > 0) {
        tk_sta_tsk(net_tid, 0);
    }
    if (nic_ok) {
        static const uint8_t z[6] = { 0 };
        (void)z;
        arp_send(1, NULL, GW_IP);       /* 出口の MAC を先に聞いておく */
    }
}
