/*
 * virtio_snd.c — QEMU virt の仮想の音の装置（virtio-sound、virtio-mmio 版 2）（S-mode + MMU のみ）
 *
 *   virtio-mmio の窓のうち DeviceID = 25 が音。QEMU では
 *     -audiodev pa,id=snd0 -device virtio-sound-device,audiodev=snd0
 *   で付く（Makefile の qemu ターゲット。パソコンの音の仕組みに合わせて pa・pipewire・none を選ぶ）。
 *
 *   形は固定: 22050Hz・16 ビット符号つき（リトルエンディアン）・モノラル。
 *   Linux 互換層の /dev/dsp に書いたものがそのまま鳴る（linux_fs.c）。
 *
 *   キュー 0（制御）: 要求と返事の 2 つで 1 組。返事が来るまで待つ。
 *   キュー 2（送り出し）: 1 回分（PERIOD = 2048 バイト、約 46 ミリ秒）ごとに
 *     「ストリームの番号 / 音のデータ / 状態（装置が書く）」の 3 つで 1 組。TXN 組（約 0.7 秒分）まで先に渡す。
 *     装置は鳴らし終わった組を返す。割込みは使わず、空きがなければ 5 ミリ秒ずつ待って見直す。
 *   最初に書いたときに START、閉じるときに残りを鳴らし終えるのを待って STOP。
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>
#include <libstr.h>
#include <rv_priv.h>
#include "virtio_mmio_board.h"

extern int rv_user_copy_in(void *dst, uintptr_t uva, size_t len);
extern int rv_linux_sig_interrupt(void);


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
#define DEVID_SND       25

#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2

/* virtio-sound の要求 */
#define R_PCM_INFO      0x0100
#define R_PCM_SET_PARAMS 0x0101
#define R_PCM_PREPARE   0x0102
#define R_PCM_RELEASE   0x0103
#define R_PCM_START     0x0104
#define R_PCM_STOP      0x0105
#define S_OK            0x8000
#define D_OUTPUT        0
#define FMT_S16         5
#define RATE_22050      4

#define QN      64                  /* 記述子の数 */
#define TXN     16                  /* 送り出しの組の数（1 組 = 記述子 3 つ） */
#define PERIOD  2048

#define ENODEV  19
#define EBUSY   16
#define EINTR   4
#define EFAULT  14
#define EIO     5

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
static struct vq ctlq __attribute__((aligned(4096)));
static struct vq txq __attribute__((aligned(4096)));

struct snd_pcm_set_params {
    uint32_t code, stream_id;
    uint32_t buffer_bytes, period_bytes, features;
    uint8_t channels, format, rate, padding;
};
struct snd_pcm_info {
    uint32_t hda_fn_nid, features;
    uint64_t formats, rates;
    uint8_t direction, channels_min, channels_max, padding[5];
};

static uint8_t  ctl_req[64] __attribute__((aligned(16)));
static uint8_t  ctl_resp[512] __attribute__((aligned(16)));
static uint32_t tx_hdr[TXN];
static uint32_t tx_status[TXN][2];
static uint8_t  tx_buf[TXN][PERIOD] __attribute__((aligned(16)));
static int      tx_inflight;        /* 装置に渡して、まだ返ってこない組の数 */
static int      tx_next;            /* 次に使う組 */
static uint8_t  part[PERIOD];       /* 1 組に満たない書きかけ */
static unsigned part_len;

static uintptr_t snd_base;
static uint32_t  snd_stream;        /* 出力のストリームの番号 */
static int       snd_open, snd_started;
volatile unsigned long rv_snd_bytes, rv_snd_periods_done;

static inline uint32_t rd(uintptr_t off) { return *(volatile uint32_t *)(snd_base + off); }
static inline void wr(uintptr_t off, uint32_t v) { *(volatile uint32_t *)(snd_base + off) = v; }
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
    q->avail.flags = 1;             /* 割込みは要らない（見に行く） */
    return 0;
}

/* 制御の要求を出して返事を待つ。返事の状態（S_OK など）、待ちきれなければ 0 */
static uint32_t control(unsigned req_len, unsigned resp_len)
{
    int waited;
    ctlq.desc[0].addr = (uint64_t)(uintptr_t)ctl_req;
    ctlq.desc[0].len = req_len;
    ctlq.desc[0].flags = VIRTQ_DESC_F_NEXT;
    ctlq.desc[0].next = 1;
    ctlq.desc[1].addr = (uint64_t)(uintptr_t)ctl_resp;
    ctlq.desc[1].len = resp_len;
    ctlq.desc[1].flags = VIRTQ_DESC_F_WRITE;
    ctlq.desc[1].next = 0;
    memset(ctl_resp, 0, resp_len);
    ctlq.avail.ring[ctlq.avail.idx % QN] = 0;
    fence_rw();
    ctlq.avail.idx++;
    fence_rw();
    wr(VM_QUEUE_NOTIFY, 0);
    for (waited = 0; *(volatile uint16_t *)&ctlq.used.idx == ctlq.used_seen; waited++) {
        if (waited > 2000) {
            return 0;
        }
        tk_dly_tsk(1);
    }
    fence_rw();
    ctlq.used_seen++;
    return *(volatile uint32_t *)ctl_resp;
}

static uint32_t pcm_cmd(uint32_t code)
{
    uint32_t *r = (uint32_t *)ctl_req;
    r[0] = code;
    r[1] = snd_stream;
    return control(8, 4);
}

/* 見つかって使える状態なら 1 */
int virtio_snd_init(void)
{
    unsigned i;
    uint32_t streams;

    snd_base = 0;
    for (i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uintptr_t b = VIRTIO_MMIO_BASE + i * VIRTIO_MMIO_STRIDE;
        if (*(volatile uint32_t *)(b + VM_MAGIC) == VIRTIO_MAGIC
            && *(volatile uint32_t *)(b + VM_DEVICE_ID) == DEVID_SND
            && *(volatile uint32_t *)(b + VM_VERSION) == 2) {
            snd_base = b;
            break;
        }
    }
    if (snd_base == 0) {
        return 0;
    }
    wr(VM_STATUS, 0);
    wr(VM_STATUS, ST_ACK);
    wr(VM_STATUS, ST_ACK | ST_DRIVER);
    wr(VM_DEV_FEATURES_SEL, 1);
    if (!(rd(VM_DEV_FEATURES) & 1)) {                   /* VERSION_1 */
        wr(VM_STATUS, ST_FAILED);
        snd_base = 0;
        return 0;
    }
    wr(VM_DRV_FEATURES_SEL, 0);
    wr(VM_DRV_FEATURES, 0);
    wr(VM_DRV_FEATURES_SEL, 1);
    wr(VM_DRV_FEATURES, 1);
    wr(VM_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    if (!(rd(VM_STATUS) & ST_FEATURES_OK) || setup_queue(0, &ctlq) != 0 || setup_queue(2, &txq) != 0) {
        wr(VM_STATUS, ST_FAILED);
        snd_base = 0;
        return 0;
    }
    wr(VM_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
    streams = *(volatile uint32_t *)(snd_base + VM_CONFIG + 4);

    /* 出力のストリームを探す */
    snd_stream = 0xffffffffU;
    for (i = 0; i < streams && i < 8; i++) {
        uint32_t *r = (uint32_t *)ctl_req;
        struct snd_pcm_info *info = (struct snd_pcm_info *)(ctl_resp + 4);
        r[0] = R_PCM_INFO;
        r[1] = i;                   /* 最初の番号 */
        r[2] = 1;                   /* 数 */
        r[3] = sizeof(struct snd_pcm_info);
        if (control(16, 4 + sizeof(struct snd_pcm_info)) != S_OK) {
            continue;
        }
        if (info->direction == D_OUTPUT && (info->formats & (1ULL << FMT_S16))
            && (info->rates & (1ULL << RATE_22050)) && info->channels_min <= 1) {
            snd_stream = i;
            break;
        }
    }
    if (snd_stream == 0xffffffffU) {
        snd_base = 0;
        return 0;
    }
    {
        struct snd_pcm_set_params *p = (struct snd_pcm_set_params *)ctl_req;
        memset(p, 0, sizeof(*p));
        p->code = R_PCM_SET_PARAMS;
        p->stream_id = snd_stream;
        p->buffer_bytes = TXN * PERIOD;
        p->period_bytes = PERIOD;
        p->channels = 1;
        p->format = FMT_S16;
        p->rate = RATE_22050;
        if (control(sizeof(*p), 4) != S_OK || pcm_cmd(R_PCM_PREPARE) != S_OK) {
            snd_base = 0;
            return 0;
        }
    }
    for (i = 0; i < TXN; i++) {
        tx_hdr[i] = snd_stream;
    }
    return 1;
}

int virtio_snd_present(void)
{
    return snd_base != 0;
}

/* 鳴らし終わった組を回収する */
static void reap(void)
{
    while (*(volatile uint16_t *)&txq.used.idx != txq.used_seen) {
        fence_rw();
        txq.used_seen++;
        tx_inflight--;
        rv_snd_periods_done++;
    }
}

/* 1 組（PERIOD バイト）を渡す。空きを待つ。シグナルで止まれば -EINTR */
static int submit(const uint8_t *data)
{
    int b, d;

    for (;;) {
        reap();
        if (tx_inflight < TXN) {
            break;
        }
        if (rv_linux_sig_interrupt()) {
            return -EINTR;
        }
        tk_dly_tsk(5);
    }
    b = tx_next;
    tx_next = (tx_next + 1) % TXN;
    memcpy(tx_buf[b], data, PERIOD);
    d = b * 3;
    txq.desc[d].addr = (uint64_t)(uintptr_t)&tx_hdr[b];
    txq.desc[d].len = 4;
    txq.desc[d].flags = VIRTQ_DESC_F_NEXT;
    txq.desc[d].next = (uint16_t)(d + 1);
    txq.desc[d + 1].addr = (uint64_t)(uintptr_t)tx_buf[b];
    txq.desc[d + 1].len = PERIOD;
    txq.desc[d + 1].flags = VIRTQ_DESC_F_NEXT;
    txq.desc[d + 1].next = (uint16_t)(d + 2);
    txq.desc[d + 2].addr = (uint64_t)(uintptr_t)tx_status[b];
    txq.desc[d + 2].len = 8;
    txq.desc[d + 2].flags = VIRTQ_DESC_F_WRITE;
    txq.desc[d + 2].next = 0;
    txq.avail.ring[txq.avail.idx % QN] = (uint16_t)d;
    fence_rw();
    txq.avail.idx++;
    tx_inflight++;
    fence_rw();
    wr(VM_QUEUE_NOTIFY, 2);
    if (!snd_started) {
        /* 少したまってから始める（途切れにくいように） */
        if (tx_inflight >= 4) {
            if (pcm_cmd(R_PCM_START) != S_OK) {
                return -EIO;
            }
            snd_started = 1;
        }
    }
    return 0;
}

int rv_snd_open(void)
{
    if (snd_base == 0) {
        return -ENODEV;
    }
    if (snd_open) {
        return -EBUSY;
    }
    snd_open = 1;
    part_len = 0;
    return 0;
}

/* 書く（ユーザーの領域から）。書けたバイト数か負のエラー */
long rv_snd_write(uintptr_t ubuf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        size_t n = PERIOD - part_len;
        if (n > len - done) {
            n = len - done;
        }
        if (rv_user_copy_in(part + part_len, ubuf + done, n) != 0) {
            return done > 0 ? (long)done : -EFAULT;
        }
        part_len += (unsigned)n;
        done += n;
        if (part_len == PERIOD) {
            int r = submit(part);
            if (r < 0) {
                return done > n ? (long)(done - n) : r;
            }
            part_len = 0;
        }
    }
    rv_snd_bytes += len;
    return (long)len;
}

/* 閉じる: 書きかけを無音で埋めて渡し、鳴り終わるのを待って止める */
void rv_snd_close(void)
{
    int waited;

    if (!snd_open) {
        return;
    }
    if (part_len > 0) {
        memset(part + part_len, 0, PERIOD - part_len);
        submit(part);
        part_len = 0;
    }
    if (tx_inflight > 0 && !snd_started) {
        if (pcm_cmd(R_PCM_START) == S_OK) {
            snd_started = 1;
        }
    }
    for (waited = 0; tx_inflight > 0 && waited < 400; waited++) {     /* 最大 2 秒 */
        reap();
        if (tx_inflight == 0) {
            break;
        }
        tk_dly_tsk(5);
    }
    if (snd_started) {
        pcm_cmd(R_PCM_STOP);
        snd_started = 0;
    }
    reap();
    snd_open = 0;
}

/* テスト用: 装置に渡して返ってこない組の数 */
int rv_snd_inflight(void)
{
    return tx_inflight;
}
