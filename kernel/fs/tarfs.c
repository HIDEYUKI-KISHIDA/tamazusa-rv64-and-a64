/*
 * tarfs.c — ディスク上の tar（ustar）を読み取り専用のファイルシステムとして読む
 *
 *   tar の見出しは 512 バイト = 1 セクタで、ファイルの中身はその直後から
 *   512 バイト境界で続く。ファイル名で探し、開始セクタと大きさを返す。
 *   作り方（ホスト側）: tar --format=ustar -cf disk.img ファイル...
 *
 *   作業用のセクタは呼んだタスクのスタックに置く（複数のタスクから同時に呼べる）。
 *   この段の制約: 通常のファイルのみ。名前は 100 文字まで（prefix 欄は使わない）。
 */
#include <stdint.h>
#include <stddef.h>
#include "tarfs.h"

#define SECTOR      512
#define MAX_ENTRIES 4096                /* 壊れた tar で回り続けないための上限 */

static int is_zero_block(const uint8_t *b)
{
    unsigned i;
    for (i = 0; i < SECTOR; i++) {
        if (b[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* 8 進数の欄（空白・NUL で終わる）。不正なら -1 */
static int64_t octal_field(const uint8_t *f, unsigned len)
{
    int64_t v = 0;
    unsigned i = 0;

    while (i < len && f[i] == ' ') {
        i++;
    }
    for (; i < len && f[i] != 0 && f[i] != ' '; i++) {
        if (f[i] < '0' || f[i] > '7') {
            return -1;
        }
        v = v * 8 + (f[i] - '0');
    }
    return v;
}

/* 見出しの検査和（chksum 欄を空白とみなした全バイトの和） */
static int checksum_ok(const uint8_t *b)
{
    int64_t want = octal_field(b + 148, 8);
    uint32_t sum = 0;
    unsigned i;

    for (i = 0; i < SECTOR; i++) {
        sum += (i >= 148 && i < 156) ? ' ' : b[i];
    }
    return want >= 0 && (uint32_t)want == sum;
}

static int name_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/*
 * index 番目のファイルの情報を f に入れる（通常ファイルだけを数える）。
 * 成功 1、終わり 0、壊れている -1
 */
int tarfs_entry(int index, struct tarfs_file *f)
{
    uint8_t hdr[SECTOR] __attribute__((aligned(8)));   /* タスクごと（スタック上） */
    uint64_t s = 0;
    uint64_t cap = virtio_blk_capacity();
    int n = 0;
    int guard;

    for (guard = 0; guard < MAX_ENTRIES; guard++) {
        int64_t size;
        unsigned i;
        const char *nm;

        if (s >= cap || virtio_blk_read(s, hdr, 1) != 0) {
            return -1;
        }
        if (is_zero_block(hdr)) {
            return 0;                   /* tar の終わり */
        }
        if (!checksum_ok(hdr) || hdr[257] != 'u' || hdr[258] != 's' || hdr[259] != 't'
            || hdr[260] != 'a' || hdr[261] != 'r') {
            return -1;
        }
        size = octal_field(hdr + 124, 12);
        if (size < 0) {
            return -1;
        }
        if (hdr[156] == '0' || hdr[156] == 0) {        /* 通常ファイル */
            if (n == index) {
                nm = (const char *)hdr;
                if (nm[0] == '.' && nm[1] == '/') {
                    nm += 2;
                }
                for (i = 0; i < 100 && nm[i] != 0 && &nm[i] < (const char *)hdr + 100; i++) {
                    f->name[i] = nm[i];
                }
                f->name[i] = 0;
                f->start = s + 1;
                f->size = (uint64_t)size;
                f->mode = (uint32_t)(octal_field(hdr + 100, 8) & 07777);
                if (f->start + (f->size + SECTOR - 1) / SECTOR > cap) {
                    return -1;          /* 中身がディスクの外 */
                }
                return 1;
            }
            n++;
        }
        s += 1 + ((uint64_t)size + SECTOR - 1) / SECTOR;
    }
    return -1;
}

/* 名前で探す。見つかれば 1、なければ 0、壊れていれば -1 */
int tarfs_find(const char *name, struct tarfs_file *f)
{
    int i, r;

    for (i = 0; ; i++) {
        r = tarfs_entry(i, f);
        if (r <= 0) {
            return r;
        }
        if (name_eq(f->name, name)) {
            return 1;
        }
    }
}

/* ファイルの中身を先頭から最大 max バイト読む。読んだバイト数、失敗 -1 */
int64_t tarfs_read_all(const struct tarfs_file *f, void *buf, uint64_t max)
{
    uint8_t sec[SECTOR] __attribute__((aligned(8)));   /* タスクごと（スタック上） */
    uint8_t *p = buf;
    uint64_t want = (f->size < max) ? f->size : max;
    uint64_t done = 0;
    uint64_t full = want / SECTOR;

    if (full > 0) {
        if (virtio_blk_read(f->start, p, (unsigned)full) != 0) {
            return -1;
        }
        done = full * SECTOR;
    }
    if (done < want) {
        unsigned k;
        if (virtio_blk_read(f->start + full, sec, 1) != 0) {
            return -1;
        }
        for (k = 0; done < want; k++) {
            p[done++] = sec[k];
        }
    }
    return (int64_t)done;
}

/* ファイルの offset から最大 len バイトを buf に読む。読んだバイト数（終わりなら 0）、失敗 -1 */
int64_t tarfs_read(const struct tarfs_file *f, uint64_t offset, void *buf, uint64_t len)
{
    uint8_t sec[SECTOR] __attribute__((aligned(8)));   /* タスクごと（スタック上） */
    uint8_t *p = buf;
    uint64_t done = 0;

    if (offset >= f->size) {
        return 0;
    }
    if (len > f->size - offset) {
        len = f->size - offset;
    }
    while (done < len) {
        uint64_t pos = offset + done;
        uint64_t in = pos % SECTOR;
        uint64_t n = SECTOR - in;
        uint64_t k;

        if (n > len - done) {
            n = len - done;
        }
        if (virtio_blk_read(f->start + pos / SECTOR, sec, 1) != 0) {
            return -1;
        }
        for (k = 0; k < n; k++) {
            p[done + k] = sec[in + k];
        }
        done += n;
    }
    return (int64_t)done;
}
