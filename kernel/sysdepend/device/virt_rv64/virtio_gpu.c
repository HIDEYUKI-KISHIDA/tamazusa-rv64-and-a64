/*
 * virtio_gpu.c — QEMU virt の仮想の画面（virtio-gpu の 2D、virtio-mmio 版 2）（S-mode + MMU のみ）
 *
 *   virtio-mmio の窓のうち DeviceID = 16 が画面。QEMU では
 *     -device virtio-gpu-device -display gtk
 *   で付く（Makefile の qemu ターゲット。パソコンに窓が出せなければ -display none）。
 *
 *   装置に出すのは画面全体（1024×768、1 点 4 バイト: B, G, R, 使わない）。カーネルの中に 1 枚分の入れ物
 *   （3MiB）を持ち、装置に「資源 1 番」として渡しておく。中身はデスクトップ（kernel/gui/desktop.c）が組み立て、
 *   変わった四角を装置へ写し（TRANSFER_TO_HOST_2D）、窓に出させる（RESOURCE_FLUSH）。
 *   /dev/fb0（linux_fs.c）は 640×400（PC-9801 と同じ）のまま。書いた絵は別の入れ物に入り、
 *   デスクトップの「画面」の窓の中に出る。
 *   制御の要求は 1 つずつ出し、返事が来るまで待つ（割込みは使わない）。
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>
#include <libstr.h>
#include <rv_priv.h>
#include "virtio_mmio_board.h"
#include "../../../gui/desktop.h"

extern int rv_user_copy_in(void *dst, uintptr_t uva, size_t len);


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

#define ST_ACK          1
#define ST_DRIVER       2
#define ST_DRIVER_OK    4
#define ST_FEATURES_OK  8
#define ST_FAILED       128
#define VIRTIO_MAGIC    0x74726976U
#define DEVID_GPU       16

#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2

/* virtio-gpu の要求 */
#define CMD_RESOURCE_CREATE_2D      0x0101
#define CMD_SET_SCANOUT             0x0103
#define CMD_RESOURCE_FLUSH          0x0104
#define CMD_TRANSFER_TO_HOST_2D     0x0105
#define CMD_RESOURCE_ATTACH_BACKING 0x0106
#define RESP_OK_NODATA              0x1100
#define FORMAT_B8G8R8X8             2

#define FB_W    APP_W
#define FB_H    APP_H
#define FB_BYTES (FB_W * FB_H * 4)
#define SCR_W   DESK_W
#define SCR_H   DESK_H
#define RES_ID  1
#define QN      8

#define ENODEV  19
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

struct gpu_hdr { uint32_t type, flags; uint64_t fence_id; uint32_t ctx_id; uint8_t ring_idx, pad[3]; };
struct gpu_rect { uint32_t x, y, w, h; };

static uint8_t req[128] __attribute__((aligned(16)));
static struct gpu_hdr resp __attribute__((aligned(16)));
static uint8_t fb[FB_BYTES] __attribute__((aligned(4096)));
static uint32_t scr[SCR_W * SCR_H] __attribute__((aligned(4096)));

static uintptr_t gpu_base;
static int gpu_lock_tid;            /* 複数のプロセスが同時に書いても要求が混ざらないように */
volatile unsigned long rv_gpu_flushes, rv_gpu_errors;

static inline uint32_t rd(uintptr_t off) { return *(volatile uint32_t *)(gpu_base + off); }
static inline void wr(uintptr_t off, uint32_t v) { *(volatile uint32_t *)(gpu_base + off) = v; }
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
    q->avail.flags = 1;
    return 0;
}

/* 要求（req の先頭 len バイト）を出して返事を待つ。成功 1 */
static int command(unsigned len)
{
    int waited;
    ctlq.desc[0].addr = (uint64_t)(uintptr_t)req;
    ctlq.desc[0].len = len;
    ctlq.desc[0].flags = VIRTQ_DESC_F_NEXT;
    ctlq.desc[0].next = 1;
    ctlq.desc[1].addr = (uint64_t)(uintptr_t)&resp;
    ctlq.desc[1].len = sizeof(resp);
    ctlq.desc[1].flags = VIRTQ_DESC_F_WRITE;
    ctlq.desc[1].next = 0;
    resp.type = 0;
    ctlq.avail.ring[ctlq.avail.idx % QN] = 0;
    fence_rw();
    ctlq.avail.idx++;
    fence_rw();
    wr(VM_QUEUE_NOTIFY, 0);
    for (waited = 0; *(volatile uint16_t *)&ctlq.used.idx == ctlq.used_seen; waited++) {
        if (waited > 20000) {
            rv_gpu_errors++;
            return 0;
        }
        if (waited > 50) {
            tk_dly_tsk(1);
        }
    }
    fence_rw();
    ctlq.used_seen++;
    if (resp.type != RESP_OK_NODATA) {
        rv_gpu_errors++;
        return 0;
    }
    return 1;
}

static struct gpu_hdr *hdr(uint32_t type)
{
    struct gpu_hdr *h = (struct gpu_hdr *)req;
    memset(req, 0, sizeof(req));
    h->type = type;
    return h;
}

/* 画面全体の四角 (x, y, w, h) を装置へ写して窓に出す。成功 1 */
int virtio_gpu_show(unsigned x, unsigned y, unsigned w, unsigned h)
{
    uint32_t *p;
    int me = tk_get_tid(), ok = 0;
    if (gpu_base == 0 || x >= SCR_W || y >= SCR_H || w == 0 || h == 0) {
        return 0;
    }
    if (x + w > SCR_W) {
        w = SCR_W - x;
    }
    if (y + h > SCR_H) {
        h = SCR_H - y;
    }
    while (gpu_lock_tid != 0 && gpu_lock_tid != me) {
        tk_dly_tsk(1);
    }
    gpu_lock_tid = me;
    hdr(CMD_TRANSFER_TO_HOST_2D);
    p = (uint32_t *)(req + sizeof(struct gpu_hdr));
    p[0] = x;                       /* rect */
    p[1] = y;
    p[2] = w;
    p[3] = h;
    *(uint64_t *)&p[4] = ((uint64_t)y * SCR_W + x) * 4;    /* offset（四角の左上） */
    p[6] = RES_ID;
    p[7] = 0;
    if (command(sizeof(struct gpu_hdr) + 32)) {
        hdr(CMD_RESOURCE_FLUSH);
        p[0] = x;
        p[1] = y;
        p[2] = w;
        p[3] = h;
        p[4] = RES_ID;
        p[5] = 0;
        ok = command(sizeof(struct gpu_hdr) + 24);
        if (ok) {
            rv_gpu_flushes++;
        }
    }
    gpu_lock_tid = 0;
    return ok;
}

/* 見つかって使える状態なら 1 */
int virtio_gpu_init(void)
{
    unsigned i;
    uint32_t *p;

    gpu_base = 0;
    for (i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uintptr_t b = VIRTIO_MMIO_BASE + i * VIRTIO_MMIO_STRIDE;
        if (*(volatile uint32_t *)(b + VM_MAGIC) == VIRTIO_MAGIC
            && *(volatile uint32_t *)(b + VM_DEVICE_ID) == DEVID_GPU
            && *(volatile uint32_t *)(b + VM_VERSION) == 2) {
            gpu_base = b;
            break;
        }
    }
    if (gpu_base == 0) {
        return 0;
    }
    wr(VM_STATUS, 0);
    wr(VM_STATUS, ST_ACK);
    wr(VM_STATUS, ST_ACK | ST_DRIVER);
    wr(VM_DEV_FEATURES_SEL, 1);
    if (!(rd(VM_DEV_FEATURES) & 1)) {                   /* VERSION_1 */
        wr(VM_STATUS, ST_FAILED);
        gpu_base = 0;
        return 0;
    }
    wr(VM_DRV_FEATURES_SEL, 0);
    wr(VM_DRV_FEATURES, 0);                             /* 3D（virgl）・EDID は使わない */
    wr(VM_DRV_FEATURES_SEL, 1);
    wr(VM_DRV_FEATURES, 1);
    wr(VM_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    if (!(rd(VM_STATUS) & ST_FEATURES_OK) || setup_queue(0, &ctlq) != 0) {
        wr(VM_STATUS, ST_FAILED);
        gpu_base = 0;
        return 0;
    }
    wr(VM_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);

    /* 資源 1 番: 画面全体（1024×768）の B8G8R8X8 */
    hdr(CMD_RESOURCE_CREATE_2D);
    p = (uint32_t *)(req + sizeof(struct gpu_hdr));
    p[0] = RES_ID;
    p[1] = FORMAT_B8G8R8X8;
    p[2] = SCR_W;
    p[3] = SCR_H;
    if (!command(sizeof(struct gpu_hdr) + 16)) {
        gpu_base = 0;
        return 0;
    }
    /* 中身はカーネルの入れ物（物理的に続いている） */
    hdr(CMD_RESOURCE_ATTACH_BACKING);
    p[0] = RES_ID;
    p[1] = 1;                       /* nr_entries */
    *(uint64_t *)&p[2] = (uint64_t)(uintptr_t)scr;
    p[4] = SCR_W * SCR_H * 4;
    p[5] = 0;
    if (!command(sizeof(struct gpu_hdr) + 8 + 16)) {
        gpu_base = 0;
        return 0;
    }
    /* 画面 0 番に出す */
    hdr(CMD_SET_SCANOUT);
    p[0] = 0;
    p[1] = 0;
    p[2] = SCR_W;
    p[3] = SCR_H;
    p[4] = 0;                       /* scanout_id */
    p[5] = RES_ID;
    if (!command(sizeof(struct gpu_hdr) + 24)) {
        gpu_base = 0;
        return 0;
    }
    memset(fb, 0, FB_BYTES);
    desktop_start(scr, (uint32_t *)fb);    /* 組み立てて出すのはデスクトップの仕事 */
    return 1;
}

int virtio_gpu_present(void)
{
    return gpu_base != 0;
}

void rv_fb_geometry(unsigned *w, unsigned *h)
{
    *w = FB_W;
    *h = FB_H;
}

unsigned long rv_fb_size(void)
{
    return FB_BYTES;
}

/* 位置 off から書いて、かかった行を「画面」の窓に出す（デスクトップが次に組み立てるとき）。書けたバイト数か負のエラー */
long rv_fb_write(uint64_t off, uintptr_t ubuf, size_t len)
{

    if (gpu_base == 0) {
        return -ENODEV;
    }
    if (off >= FB_BYTES) {
        return 0;
    }
    if (len > FB_BYTES - off) {
        len = (size_t)(FB_BYTES - off);
    }
    if (rv_user_copy_in(fb + off, ubuf, len) != 0) {
        return -EFAULT;
    }
    desktop_app_damage((unsigned)(off / (FB_W * 4)), (unsigned)((off + len + FB_W * 4 - 1) / (FB_W * 4)));
    return (long)len;
}

/* 読む（いま出ているもの） */
long rv_fb_read(uint64_t off, uintptr_t ubuf, size_t len)
{
    extern int rv_user_copy_out(uintptr_t udst, const void *src, size_t n);
    if (gpu_base == 0) {
        return -ENODEV;
    }
    if (off >= FB_BYTES) {
        return 0;
    }
    if (len > FB_BYTES - off) {
        len = (size_t)(FB_BYTES - off);
    }
    return rv_user_copy_out(ubuf, fb + off, len) == 0 ? (long)len : -EFAULT;
}

/* テスト用: 点の色（0xRRGGBB） */
uint32_t rv_fb_pixel(unsigned x, unsigned y)
{
    const uint8_t *q;
    if (x >= FB_W || y >= FB_H) {
        return 0;
    }
    q = fb + (y * FB_W + x) * 4;
    return ((uint32_t)q[2] << 16) | ((uint32_t)q[1] << 8) | q[0];
}
