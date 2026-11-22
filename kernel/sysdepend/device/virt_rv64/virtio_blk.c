/*
 * virtio_blk.c — QEMU virt の仮想ディスク（virtio-blk、virtio-mmio 版 2）
 *
 *   virtio-mmio の窓は 0x1000_1000 から 0x1000 おきに 8 個。DeviceID = 2 がブロック装置。
 *   QEMU は既定で古い形式（版 1）を見せるので、起動時に
 *     -global virtio-mmio.force-legacy=false
 *   を付ける（Makefile の qemu ターゲットで指定）。
 *
 *   完了の待ち方
 *     MMU=1（S-mode、PLIC あり）: 完了の割込み（virtio-mmio の窓 i なら PLIC の 1+i 番）で
 *       待っているタスクを起こす。待つ間は眠るので、ほかのタスクが動ける。
 *       要求は 1 本ずつ（ミューテックス）。
 *     それ以外: 割込みを使わず、完了を見て回る（ポーリング）。
 *       要求は 1 本ずつ（ディスパッチ禁止の間に出して待つ）。
 *   ディスクは 2 台まで: 読み取り専用（VIRTIO_BLK_F_RO）のものがルート（tar）、もう 1 台が書き込めるデータのディスク（ext2、/data）。
 *   読み取り専用のものがなければ、最初に見つけたものをルートにする（今までどおり）。
 *   データは静的な受け渡し用バッファ（4KiB）を経由する。
 *   カーネルは恒等マップなので、DMA に渡すアドレスは変数のアドレスそのまま。
 */
#include <stdint.h>
#include <stddef.h>
#include <rv_priv.h>

extern void uart_puts(const char *s);
extern int tk_dis_dsp(void);
extern int tk_ena_dsp(void);
#if TK2_MMU
#include <tk/tkernel.h>
#include <libstr.h>
#include <rv_priv.h>
extern ER tk_slp_tsk(TMO tmout);
extern ER tk_wup_tsk(ID tskid);
extern ID tk_get_tid(void);
#endif

#include "virtio_mmio_board.h"

#define irq_save()      RV_IRQ_SAVE()
#define irq_restore(st) RV_IRQ_RESTORE(st)

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

#define VIRTIO_MAGIC    0x74726976U     /* "virt" */
#define DEVID_BLOCK     2

#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2

#define VIRTIO_BLK_T_IN     0
#define VIRTIO_BLK_S_OK     0

#define QN              8               /* キューの長さ */
#define BLK_SECTOR      512
#define BOUNCE_SIZE     32768           /* 1 回の要求で読む最大（64 セクタ） */

struct virtq_desc  { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[QN]; uint16_t used_event; };
struct virtq_used_elem { uint32_t id; uint32_t len; };
struct virtq_used  { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[QN]; uint16_t avail_event; };

struct blk_req_hdr { uint32_t type; uint32_t reserved; uint64_t sector; };

#define VIRTIO_BLK_T_OUT    1
#define VIRTIO_BLK_F_RO     5

/* ディスク 1 台ぶん（0 = ルート、1 = データ） */
struct vblk {
    struct virtq_desc  vq_desc[QN]   __attribute__((aligned(4096)));
    struct virtq_avail vq_avail      __attribute__((aligned(4096)));
    struct virtq_used  vq_used       __attribute__((aligned(4096)));
    struct blk_req_hdr req_hdr       __attribute__((aligned(16)));
    volatile uint8_t   req_status;
    uint8_t bounce[BOUNCE_SIZE]      __attribute__((aligned(4096)));
    uintptr_t base;                 /* 0 = 見つかっていない */
    uint64_t  capacity;             /* 512 バイトのセクタ数 */
    uint16_t  used_seen;
    unsigned  irq;                  /* PLIC の番号（0 = 使わない） */
    int       ro;
#if TK2_MMU
    volatile int waiter;            /* 完了を待っているタスク（0 = なし） */
    ID mtx;
#endif
};
static struct vblk vb[2];
#if TK2_MMU
volatile unsigned long rv_blk_irq_count; /* テスト用: 完了の割込みの回数 */
#endif

static inline uint32_t rdv(struct vblk *v, uintptr_t off)
{
    return *(volatile uint32_t *)(v->base + off);
}

static inline void wrv(struct vblk *v, uintptr_t off, uint32_t x)
{
    *(volatile uint32_t *)(v->base + off) = x;
}

static inline void fence_rw(void)
{
    VIRTIO_FENCE();
}

/* 窓 b のブロック装置を使える状態にする。成功 1 */
static int vblk_setup(struct vblk *v, uintptr_t b, unsigned irq)
{
    unsigned i;
    uint32_t st, f0;

    v->base = b;
    v->irq = irq;
    if (rdv(v, VM_VERSION) != 2) {
        uart_puts("[virtio-blk] legacy virtio-mmio (version 1): add -global virtio-mmio.force-legacy=false\r\n");
        v->base = 0;
        return 0;
    }
    wrv(v, VM_STATUS, 0);                               /* リセット */
    wrv(v, VM_STATUS, ST_ACK);
    wrv(v, VM_STATUS, ST_ACK | ST_DRIVER);

    /* 機能: VIRTIO_F_VERSION_1（ビット 32）だけ受け入れる。読み取り専用（ビット 5）は見るだけ */
    wrv(v, VM_DEV_FEATURES_SEL, 0);
    f0 = rdv(v, VM_DEV_FEATURES);
    v->ro = (f0 >> VIRTIO_BLK_F_RO) & 1;
    wrv(v, VM_DEV_FEATURES_SEL, 1);
    if (!(rdv(v, VM_DEV_FEATURES) & 1)) {
        wrv(v, VM_STATUS, ST_FAILED);
        v->base = 0;
        return 0;
    }
    wrv(v, VM_DRV_FEATURES_SEL, 0);
    wrv(v, VM_DRV_FEATURES, 0);
    wrv(v, VM_DRV_FEATURES_SEL, 1);
    wrv(v, VM_DRV_FEATURES, 1);
    wrv(v, VM_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    st = rdv(v, VM_STATUS);
    if (!(st & ST_FEATURES_OK)) {
        wrv(v, VM_STATUS, ST_FAILED);
        v->base = 0;
        return 0;
    }

    /* キュー 0 */
    wrv(v, VM_QUEUE_SEL, 0);
    if (rdv(v, VM_QUEUE_READY) != 0 || rdv(v, VM_QUEUE_NUM_MAX) < QN) {
        wrv(v, VM_STATUS, ST_FAILED);
        v->base = 0;
        return 0;
    }
    for (i = 0; i < QN; i++) {
        v->vq_desc[i].addr = 0;
        v->vq_desc[i].len = 0;
        v->vq_desc[i].flags = 0;
        v->vq_desc[i].next = 0;
    }
#if TK2_MMU
    v->vq_avail.flags = 0;                              /* 完了の割込みがほしい */
#else
    v->vq_avail.flags = 1;                              /* 割込み不要（ポーリング） */
#endif
    v->vq_avail.idx = 0;
    v->vq_used.idx = 0;
    v->used_seen = 0;
    wrv(v, VM_QUEUE_NUM, QN);
    wrv(v, VM_QUEUE_DESC_LO,  (uint32_t)(uintptr_t)v->vq_desc);
    wrv(v, VM_QUEUE_DESC_HI,  (uint32_t)((uint64_t)(uintptr_t)v->vq_desc >> 32));
    wrv(v, VM_QUEUE_AVAIL_LO, (uint32_t)(uintptr_t)&v->vq_avail);
    wrv(v, VM_QUEUE_AVAIL_HI, (uint32_t)((uint64_t)(uintptr_t)&v->vq_avail >> 32));
    wrv(v, VM_QUEUE_USED_LO,  (uint32_t)(uintptr_t)&v->vq_used);
    wrv(v, VM_QUEUE_USED_HI,  (uint32_t)((uint64_t)(uintptr_t)&v->vq_used >> 32));
    wrv(v, VM_QUEUE_READY, 1);

    wrv(v, VM_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);

    v->capacity = (uint64_t)rdv(v, VM_CONFIG) | ((uint64_t)rdv(v, VM_CONFIG + 4) << 32);
#if TK2_MMU
    VIRTIO_IRQ_ENABLE(v->irq);
#endif
    return 1;
}

/* 見つかって使える状態なら 1（ルートのディスク）。2 台目があれば、データのディスクとして準備する */
int virtio_blk_init(void)
{
    unsigned i, n = 0;
    uintptr_t bases[VIRTIO_MMIO_COUNT];
    unsigned irqs[VIRTIO_MMIO_COUNT];
    int root = -1, data = -1;

    vb[0].base = 0;
    vb[1].base = 0;
    for (i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uintptr_t b = VIRTIO_MMIO_BASE + i * VIRTIO_MMIO_STRIDE;
        if (*(volatile uint32_t *)(b + VM_MAGIC) == VIRTIO_MAGIC
            && *(volatile uint32_t *)(b + VM_DEVICE_ID) == DEVID_BLOCK) {
            bases[n] = b;
            irqs[n] = VIRTIO_MMIO_IRQ(i);
            n++;
        }
    }
    if (n == 0) {
        uart_puts("[virtio-blk] no disk (run QEMU with a virtio-blk drive)\r\n");
        return 0;
    }
    /* 読み取り専用のものをルートに（機能のビットを見るだけ。準備は下で） */
    for (i = 0; i < n; i++) {
        volatile uint32_t *sel = (volatile uint32_t *)(bases[i] + VM_DEV_FEATURES_SEL);
        volatile uint32_t *f = (volatile uint32_t *)(bases[i] + VM_DEV_FEATURES);
        *sel = 0;
        if (((*f >> VIRTIO_BLK_F_RO) & 1) && root < 0) {
            root = (int)i;
        }
    }
    if (root < 0) {
        root = 0;
    }
    for (i = 0; i < n; i++) {
        if ((int)i != root) {
            data = (int)i;
            break;
        }
    }
    if (!vblk_setup(&vb[0], bases[root], irqs[root])) {
        return 0;
    }
    if (data >= 0 && vblk_setup(&vb[1], bases[data], irqs[data]) && vb[1].ro) {
        vb[1].base = 0;                 /* 2 台とも読み取り専用: データのディスクにはしない */
    }
    return 1;
}

uint64_t virtio_blk_capacity(void)
{
    return vb[0].capacity;
}

/* 1 回の要求（count セクタ、count * 512 <= BOUNCE_SIZE）。成功 0、失敗 -1 */
static int blk_once(struct vblk *v, uint64_t sector, void *buf, unsigned count, int write)
{
    uint16_t slot;
    unsigned long spins = 0;
    unsigned k;
    int ok;

    if (write) {
        for (k = 0; k < count * BLK_SECTOR; k++) {
            v->bounce[k] = ((const uint8_t *)buf)[k];
        }
    }
    v->req_hdr.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    v->req_hdr.reserved = 0;
    v->req_hdr.sector = sector;
    v->req_status = 0xff;

    v->vq_desc[0].addr = (uint64_t)(uintptr_t)&v->req_hdr;
    v->vq_desc[0].len = sizeof(v->req_hdr);
    v->vq_desc[0].flags = VIRTQ_DESC_F_NEXT;
    v->vq_desc[0].next = 1;
    v->vq_desc[1].addr = (uint64_t)(uintptr_t)v->bounce;
    v->vq_desc[1].len = count * BLK_SECTOR;
    v->vq_desc[1].flags = VIRTQ_DESC_F_NEXT | (write ? 0 : VIRTQ_DESC_F_WRITE);    /* 読むときは装置が書く */
    v->vq_desc[1].next = 2;
    v->vq_desc[2].addr = (uint64_t)(uintptr_t)&v->req_status;
    v->vq_desc[2].len = 1;
    v->vq_desc[2].flags = VIRTQ_DESC_F_WRITE;
    v->vq_desc[2].next = 0;

    slot = v->vq_avail.idx % QN;
    v->vq_avail.ring[slot] = 0;                         /* 先頭の記述子 */
    fence_rw();
    v->vq_avail.idx++;
    fence_rw();
    wrv(v, VM_QUEUE_NOTIFY, 0);

#if TK2_MMU
    /* 割込み禁止の間に「終わったか」を見て、まだなら待つタスクを記録して眠る。
     * 割込みが先に来ていれば、起床要求が数えられているので眠らずに戻る */
    (void)spins;
    for (;;) {
        unsigned long st;
        int done;
        st = irq_save();
        done = (*(volatile uint16_t *)&v->vq_used.idx != v->used_seen);
        v->waiter = done ? 0 : tk_get_tid();
        irq_restore(st);
        if (done) {
            break;
        }
        if (tk_slp_tsk(2000) == E_TMOUT) {
            v->waiter = 0;
            uart_puts("[virtio-blk] timeout (no completion interrupt)\r\n");
            return -1;
        }
    }
#else
    while (*(volatile uint16_t *)&v->vq_used.idx == v->used_seen) {
        if (++spins > 200000000UL) {
            uart_puts("[virtio-blk] timeout\r\n");
            return -1;
        }
    }
#endif
    fence_rw();
    v->used_seen++;
#if !TK2_MMU
    wrv(v, VM_INT_ACK, rdv(v, VM_INT_STATUS));
#endif

    ok = (v->req_status == VIRTIO_BLK_S_OK);
    if (ok && !write) {
        for (k = 0; k < count * BLK_SECTOR; k++) {
            ((uint8_t *)buf)[k] = v->bounce[k];
        }
    }
    return ok ? 0 : -1;
}

/* 読む・書く（count セクタ、何回かに分けて）。成功 0、失敗 -1 */
static int vblk_rw(struct vblk *v, uint64_t sector, void *buf, unsigned count, int write)
{
    uint8_t *p = buf;
    int r = 0;

    if (v->base == 0 || (write && v->ro) || sector + count > v->capacity) {
        return -1;
    }
#if TK2_MMU
    if (v->mtx <= 0) {                  /* 要求は 1 本ずつ（初回に作る） */
        T_CMTX cm;
        memset(&cm, 0, sizeof(cm));
        cm.mtxatr = TA_TFIFO;
        tk_dis_dsp();
        if (v->mtx <= 0) {
            v->mtx = tk_cre_mtx(&cm);
        }
        tk_ena_dsp();
        if (v->mtx <= 0) {
            return -1;
        }
    }
    tk_loc_mtx(v->mtx, TMO_FEVR);
#else
    tk_dis_dsp();                       /* 要求は 1 本ずつ */
#endif
    while (count > 0 && r == 0) {
        unsigned n = count;
        if (n > BOUNCE_SIZE / BLK_SECTOR) {
            n = BOUNCE_SIZE / BLK_SECTOR;
        }
        r = blk_once(v, sector, p, n, write);
        sector += n;
        p += n * BLK_SECTOR;
        count -= n;
    }
#if TK2_MMU
    tk_unl_mtx(v->mtx);
#else
    tk_ena_dsp();
#endif
    return r;
}

/* データのディスク（2 台目、書き込める）。キャッシュはファイルシステムの側で持つ */
int virtio_blk_data_present(void)
{
    return vb[1].base != 0;
}

uint64_t virtio_blk_data_capacity(void)
{
    return vb[1].base ? vb[1].capacity : 0;
}

int virtio_blk_data_read(uint64_t sector, void *buf, unsigned count)
{
    return vblk_rw(&vb[1], sector, buf, count, 0);
}

int virtio_blk_data_write(uint64_t sector, const void *buf, unsigned count)
{
    return vblk_rw(&vb[1], sector, (void *)buf, count, 1);
}

/* sector から count セクタを buf に読む（キャッシュを通らず、必ず装置に要求を出す）。成功 0、失敗 -1 */
int virtio_blk_read_nocache(uint64_t sector, void *buf, unsigned count)
{
    if (vb[0].base == 0) {
        return -1;
    }
    return vblk_rw(&vb[0], sector, buf, count, 0);
}

#if TK2_MMU
/*
 * セクタのキャッシュ。ディスクは読み取り専用（QEMU の readonly=on）なので、一度読んだ
 * セクタはメモリに取っておけば、中身が古くなることはない。
 * 以前は、ファイルを探すたび・プログラムを起動するたびに 512 バイトずつ何百回も
 * ディスクを待っていた。そのたびに待っていたタスクは実行可能キューの最後に並び直すので、
 * 計算し続けるプロセスがいると、プログラムの起動に何十秒もかかっていた。
 * 先頭から BCACHE_SECTORS（16MiB）まで。読めていないところは 32KiB ずつまとめて読む。
 */
#define BCACHE_SECTORS  32768
#define READ_AHEAD      (BOUNCE_SIZE / BLK_SECTOR)
static uint8_t bcache[BCACHE_SECTORS][BLK_SECTOR] __attribute__((aligned(4096)));
static volatile uint8_t bvalid[BCACHE_SECTORS];
volatile unsigned long rv_blk_cache_hits, rv_blk_cache_misses;

int virtio_blk_read(uint64_t sector, void *buf, unsigned count)
{
    uint8_t *p = buf;

    while (count > 0) {
        if (sector >= BCACHE_SECTORS) {
            return virtio_blk_read_nocache(sector, p, count);   /* キャッシュの外 */
        }
        if (!bvalid[sector]) {
            /* 読めていないところから、まとめて読んでおく（ディスクの終わり・キャッシュの終わりまで） */
            uint64_t n = READ_AHEAD, k;
            if (sector + n > BCACHE_SECTORS) {
                n = BCACHE_SECTORS - sector;
            }
            if (vb[0].capacity > 0 && sector + n > vb[0].capacity) {
                n = (sector < vb[0].capacity) ? vb[0].capacity - sector : 1;
            }
            if (virtio_blk_read_nocache(sector, bcache[sector], (unsigned)n) != 0) {
                return -1;
            }
            fence_rw();
            for (k = 0; k < n; k++) {
                bvalid[sector + k] = 1;
            }
            rv_blk_cache_misses++;
        } else {
            rv_blk_cache_hits++;
        }
        memcpy(p, bcache[sector], BLK_SECTOR);
        p += BLK_SECTOR;
        sector++;
        count--;
    }
    return 0;
}

/* PLIC から（割込み処理中）: 番号が virtio-blk のものなら 1 を返す */
int virtio_blk_irq(unsigned irq)
{
    int i;
    for (i = 0; i < 2; i++) {
        struct vblk *v = &vb[i];
        if (v->base == 0 || irq != v->irq) {
            continue;
        }
        rv_blk_irq_count++;
        wrv(v, VM_INT_ACK, rdv(v, VM_INT_STATUS));
        if (v->waiter > 0) {
            int t = v->waiter;
            v->waiter = 0;
            tk_wup_tsk(t);
        }
        return 1;
    }
    return 0;
}
#endif

#if !TK2_MMU
/* MMU=0 / M-mode: キャッシュなし */
int virtio_blk_read(uint64_t sector, void *buf, unsigned count)
{
    return virtio_blk_read_nocache(sector, buf, count);
}
#endif
