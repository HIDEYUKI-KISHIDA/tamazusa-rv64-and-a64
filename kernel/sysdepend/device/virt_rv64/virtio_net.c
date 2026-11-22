/*
 * virtio_net.c — QEMU virt の仮想 LAN（virtio-net、virtio-mmio 版 2）（S-mode + MMU のみ）
 *
 *   virtio-mmio の窓のうち DeviceID = 1 が LAN。QEMU では
 *     -netdev user,id=n0 -device virtio-net-device,netdev=n0
 *   で付く（Makefile の qemu ターゲット）。
 *
 *   受信: キュー 0 に 2KiB の入れ物を RXQ 個置いておく。届くと割込み（窓 i なら PLIC の 1+i 番）
 *         が来るので、受信を見るタスク（net_task、kernel/net/net_ip.c）を起こす。
 *         net_task が virtio_net_recv で取り出し、入れ物を戻す。
 *   送信: キュー 1。送るたびに 1 つの入れ物へ写して出し、終わったものは次の送信で回収する。
 *   どちらも先頭に virtio_net_hdr（版 1 では 12 バイト）が付く。使う機能は MAC と VERSION_1 だけ
 *   （チェックサムの肩代わり・大きな塊は使わない）。
 *   ネットワークの処理は net_ip.c のロックの中で呼ぶので、ここでは排他しない。
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>
#include <libstr.h>
#include <rv_priv.h>
#include "virtio_mmio_board.h"

extern void uart_puts(const char *s);


#define VM_MAGIC            0x000
#define VM_VERSION          0x004
#define VM_DEVICE_ID        0x008
#define VM_DEV_FEATURES     0x010
#define VM_DEV_FEATURES_SEL 0x014
#define VM_DRV_FEATURES     0x020
#define VM_DRV_FEATURES_SEL 0x024
#define VM_QUEUE_SEL        0x030
#define VM_QUEUE_NUM_MAX    0x034
#define VM_QUEUE_NUM        0x038
#define VM_QUEUE_READY      0x044
#define VM_QUEUE_NOTIFY     0x050
#define VM_INT_STATUS       0x060
#define VM_INT_ACK          0x064
#define VM_STATUS           0x070
#define VM_QUEUE_DESC_LO    0x080
#define VM_QUEUE_DESC_HI    0x084
#define VM_QUEUE_AVAIL_LO   0x090
#define VM_QUEUE_AVAIL_HI   0x094
#define VM_QUEUE_USED_LO    0x0a0
#define VM_QUEUE_USED_HI    0x0a4
#define VM_CONFIG           0x100

#define ST_ACK          1
#define ST_DRIVER       2
#define ST_DRIVER_OK    4
#define ST_FEATURES_OK  8
#define ST_FAILED       128
#define VIRTIO_MAGIC    0x74726976U
#define DEVID_NET       1
#define F_MAC           (1U << 5)

#define VIRTQ_DESC_F_WRITE  2
#define QN      16
#define BUFSZ   2048
#define HDRLEN  12

struct virtq_desc  { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[QN]; uint16_t used_event; };
struct virtq_used_elem { uint32_t id; uint32_t len; };
struct virtq_used  { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[QN]; uint16_t avail_event; };

struct vq {
    struct virtq_desc  desc[QN]   __attribute__((aligned(4096)));
    struct virtq_avail avail      __attribute__((aligned(4096)));
    struct virtq_used  used       __attribute__((aligned(4096)));
    uint16_t used_seen;
};
static struct vq rxq __attribute__((aligned(4096)));
static struct vq txq __attribute__((aligned(4096)));
static uint8_t rxbuf[QN][BUFSZ] __attribute__((aligned(16)));
static uint8_t txbuf[QN][BUFSZ] __attribute__((aligned(16)));
static uint16_t tx_free_count;
static uint16_t tx_next;            /* 次に使う送信の入れ物 */

static uintptr_t net_base;
static unsigned  net_irq;
static uint8_t   net_mac[6];
static volatile int net_waiter;     /* 受信を待つタスク（net_task） */
volatile unsigned long rv_net_irq_count, rv_net_rx_count, rv_net_tx_count;

static inline uint32_t rd(uintptr_t off) { return *(volatile uint32_t *)(net_base + off); }
static inline void wr(uintptr_t off, uint32_t v) { *(volatile uint32_t *)(net_base + off) = v; }
static inline void fence_rw(void) { VIRTIO_FENCE(); }

static int setup_queue(unsigned idx, struct vq *q)
{
    wr(VM_QUEUE_SEL, idx);
    if (rd(VM_QUEUE_READY) != 0 || rd(VM_QUEUE_NUM_MAX) < QN) {
        return -1;
    }
    memset(q, 0, sizeof(*q));
    wr(VM_QUEUE_NUM, QN);
    wr(VM_QUEUE_DESC_LO,  (uint32_t)(uintptr_t)q->desc);
    wr(VM_QUEUE_DESC_HI,  (uint32_t)((uint64_t)(uintptr_t)q->desc >> 32));
    wr(VM_QUEUE_AVAIL_LO, (uint32_t)(uintptr_t)&q->avail);
    wr(VM_QUEUE_AVAIL_HI, (uint32_t)((uint64_t)(uintptr_t)&q->avail >> 32));
    wr(VM_QUEUE_USED_LO,  (uint32_t)(uintptr_t)&q->used);
    wr(VM_QUEUE_USED_HI,  (uint32_t)((uint64_t)(uintptr_t)&q->used >> 32));
    wr(VM_QUEUE_READY, 1);
    return 0;
}

/* 見つかって使える状態なら 1。mac に MAC アドレス */
int virtio_net_init(uint8_t mac[6])
{
    unsigned i;

    net_base = 0;
    for (i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uintptr_t b = VIRTIO_MMIO_BASE + i * VIRTIO_MMIO_STRIDE;
        if (*(volatile uint32_t *)(b + VM_MAGIC) == VIRTIO_MAGIC
            && *(volatile uint32_t *)(b + VM_DEVICE_ID) == DEVID_NET
            && *(volatile uint32_t *)(b + VM_VERSION) == 2) {
            net_base = b;
            net_irq = VIRTIO_MMIO_IRQ(i);
            break;
        }
    }
    if (net_base == 0) {
        return 0;
    }
    wr(VM_STATUS, 0);
    wr(VM_STATUS, ST_ACK);
    wr(VM_STATUS, ST_ACK | ST_DRIVER);
    wr(VM_DEV_FEATURES_SEL, 1);
    if (!(rd(VM_DEV_FEATURES) & 1)) {                   /* VERSION_1 */
        wr(VM_STATUS, ST_FAILED);
        net_base = 0;
        return 0;
    }
    wr(VM_DEV_FEATURES_SEL, 0);
    {
        uint32_t want = rd(VM_DEV_FEATURES) & F_MAC;
        wr(VM_DRV_FEATURES_SEL, 0);
        wr(VM_DRV_FEATURES, want);
    }
    wr(VM_DRV_FEATURES_SEL, 1);
    wr(VM_DRV_FEATURES, 1);
    wr(VM_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    if (!(rd(VM_STATUS) & ST_FEATURES_OK) || setup_queue(0, &rxq) != 0 || setup_queue(1, &txq) != 0) {
        wr(VM_STATUS, ST_FAILED);
        net_base = 0;
        return 0;
    }
    /* 受信の入れ物をすべて置く */
    for (i = 0; i < QN; i++) {
        rxq.desc[i].addr = (uint64_t)(uintptr_t)rxbuf[i];
        rxq.desc[i].len = BUFSZ;
        rxq.desc[i].flags = VIRTQ_DESC_F_WRITE;
        rxq.avail.ring[i] = (uint16_t)i;
    }
    fence_rw();
    rxq.avail.idx = QN;
    txq.avail.flags = 1;                                /* 送信の完了の割込みは要らない */
    tx_free_count = QN;
    tx_next = 0;
    wr(VM_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
    fence_rw();
    wr(VM_QUEUE_NOTIFY, 0);

    for (i = 0; i < 6; i++) {
        net_mac[i] = *(volatile uint8_t *)(net_base + VM_CONFIG + i);
        mac[i] = net_mac[i];
    }
    VIRTIO_IRQ_ENABLE(net_irq);
    return 1;
}

int virtio_net_present(void)
{
    return net_base != 0;
}

/* 受信を待つタスクを登録する（割込みで起こす） */
void virtio_net_set_waiter(int tid)
{
    net_waiter = tid;
}

/* 届いたフレームを 1 つ buf に写して長さを返す（なければ 0） */
int virtio_net_recv(uint8_t *buf, unsigned max)
{
    uint16_t slot, id;
    uint32_t len;

    if (net_base == 0 || *(volatile uint16_t *)&rxq.used.idx == rxq.used_seen) {
        return 0;
    }
    fence_rw();
    slot = rxq.used_seen % QN;
    id = (uint16_t)rxq.used.ring[slot].id;
    len = rxq.used.ring[slot].len;
    rxq.used_seen++;
    if (len > HDRLEN && id < QN) {
        len -= HDRLEN;
        if (len > max) {
            len = max;
        }
        memcpy(buf, rxbuf[id] + HDRLEN, len);
    } else {
        len = 0;
    }
    /* 入れ物を戻す */
    rxq.avail.ring[rxq.avail.idx % QN] = id;
    fence_rw();
    rxq.avail.idx++;
    fence_rw();
    wr(VM_QUEUE_NOTIFY, 0);
    rv_net_rx_count++;
    return len > 0 ? (int)len : -1;     /* -1: 壊れたものを捨てた（続けて見る） */
}

/* フレーム（イーサネットの見出しから）を送る。成功 0、入れ物がなければ -1 */
int virtio_net_send(const uint8_t *frame, unsigned len)
{
    uint16_t i;

    if (net_base == 0 || len > BUFSZ - HDRLEN) {
        return -1;
    }
    /* 終わった送信を回収する */
    while (*(volatile uint16_t *)&txq.used.idx != txq.used_seen) {
        txq.used_seen++;
        tx_free_count++;
    }
    if (tx_free_count == 0) {
        return -1;
    }
    i = tx_next;
    tx_next = (uint16_t)((tx_next + 1) % QN);
    tx_free_count--;
    memset(txbuf[i], 0, HDRLEN);
    memcpy(txbuf[i] + HDRLEN, frame, len);
    txq.desc[i].addr = (uint64_t)(uintptr_t)txbuf[i];
    txq.desc[i].len = HDRLEN + len;
    txq.desc[i].flags = 0;
    txq.avail.ring[txq.avail.idx % QN] = i;
    fence_rw();
    txq.avail.idx++;
    fence_rw();
    wr(VM_QUEUE_NOTIFY, 1);
    rv_net_tx_count++;
    return 0;
}

/* PLIC から（割込み処理中）: 番号が virtio-net のものなら 1 */
int virtio_net_irq(unsigned irq)
{
    if (net_base == 0 || irq != net_irq) {
        return 0;
    }
    rv_net_irq_count++;
    wr(VM_INT_ACK, rd(VM_INT_STATUS));
    if (net_waiter > 0) {
        tk_wup_tsk(net_waiter);
    }
    return 1;
}
