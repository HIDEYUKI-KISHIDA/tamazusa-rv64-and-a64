/* kernel/fs/ext2.c — ext2 のファイルシステム（読み書き）
 *
 *   書き込めるディスク（/data）のためのファイルシステム。ext2 は Linux の標準の形のひとつで、
 *   許可（パーミッション）・シンボリックリンク・大文字と小文字の区別がある。ふつうの Linux の
 *   mke2fs で作り、e2fsck で確かめられる。
 *
 *   扱うもの: 普通のファイル（直接・1 段・2 段・3 段の間接ブロック）、ディレクトリ（線形の並び）、
 *     シンボリックリンク（60 バイト未満は inode の中、それ以上はブロック）、作る・消す・名前を変える・
 *     切り詰める・ディレクトリを作る・消す・許可を変える。
 *   扱わないもの（その機能のある ext2 はマウントしない）: incompat の filetype 以外（extents・64bit・flex_bg など）。
 *   ro_compat のうち知らないものがあれば、読み取り専用でマウントする。
 *   書き込みはいったんキャッシュにため、ext2_sync（1 秒ごとの見回り・sync・fsync・電源を切る前）でまとめてディスクへ（write-back）。
 *   ディスクの読み書きは、マウントのときに渡された関数でする（カーネルでもふつうの Linux でも試せる）。
 *   開いているファイルを消したときは、最後に閉じたときに中身を捨てる。
 */
#include <stdint.h>
#include <stddef.h>
#include "ext2.h"

#ifndef EXT2_HOST
#include <tk/tkernel.h>
extern int tk_dis_dsp(void);
extern int tk_ena_dsp(void);
static ID e2_mtx;
static void lock(void)
{
    if (e2_mtx <= 0) {
        T_CMTX cm;
        unsigned char *p = (unsigned char *)&cm;
        unsigned i;
        for (i = 0; i < sizeof(cm); i++) {
            p[i] = 0;
        }
        cm.mtxatr = TA_TFIFO;
        tk_dis_dsp();
        if (e2_mtx <= 0) {
            e2_mtx = tk_cre_mtx(&cm);
        }
        tk_ena_dsp();
    }
    tk_loc_mtx(e2_mtx, TMO_FEVR);
}
static void unlock(void)
{
    tk_unl_mtx(e2_mtx);
}
#else
static void lock(void) {}
static void unlock(void) {}
#endif

#define XE_NOENT   (-2)
#define XE_IO      (-5)
#define XE_BADF    (-9)
#define XE_NOMEM   (-12)
#define XE_EXIST   (-17)
#define XE_XDEV    (-18)
#define XE_NOTDIR  (-20)
#define XE_ISDIR   (-21)
#define XE_INVAL   (-22)
#define XE_FBIG    (-27)
#define XE_NOSPC   (-28)
#define XE_ROFS    (-30)
#define XE_MLINK   (-31)
#define XE_NAMETOOLONG (-36)
#define XE_NOTEMPTY (-39)
#define XE_LOOP    (-40)

#define S_IFMT   0xF000
#define S_IFREG  0x8000
#define S_IFDIR  0x4000
#define S_IFLNK  0xA000

#define FT_REG 1
#define FT_DIR 2
#define FT_LNK 7

#define MAXBS 4096

/* ---- ディスク */
static int (*dev_rd)(uint64_t sec, void *buf, unsigned n);
static int (*dev_wr)(uint64_t sec, const void *buf, unsigned n);
static uint32_t (*now_fn)(void);

/* ---- スーパーブロックとグループ */
static uint8_t sb[1024];
static int mounted, readonly;
static uint32_t bs, spb;                /* ブロックの大きさ、ブロックあたりのセクタ数 */
static uint32_t ngroups, ipg, bpg, first_data, inode_size, blocks_count, inodes_count;
static uint32_t ptrs;                   /* 1 ブロックに入るブロック番号の数 */
static uint8_t gdt[MAXBS * 4];          /* グループ記述子の表（いまは 4 ブロックまで） */
static uint32_t gdt_blocks;

static uint32_t g32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void mcpy(void *d, const void *s, size_t n)
{
    uint8_t *a = d;
    const uint8_t *b = s;
    while (n--) {
        *a++ = *b++;
    }
}

static void mset(void *d, int c, size_t n)
{
    uint8_t *a = d;
    while (n--) {
        *a++ = (uint8_t)c;
    }
}

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static uint32_t now(void)
{
    return now_fn ? now_fn() : 0;
}

/* ---- ブロックのキャッシュ（書いたものはためておき、あとでまとめてディスクへ）
 *   書くと、キャッシュの中身を変えて「汚れ」の印をつけるだけ。ディスクへ出すのは、
 *   ext2_sync（fsync・sync・1 秒ごとの見回り・電源を切る前）と、汚れた入れ物を別のブロックに使い回すとき。
 *   以前は書くたびにデータと inode を 2 回ずつディスクへ出していたので、1 行ずつ書くプログラム
 *   （seq 1 60000 > big など）がとても遅かった（パソコンの C: ドライブの上では、止まって見えるほど）。 */
#define NCACHE 256
static struct { uint32_t blk; uint32_t age; int valid, dirty; uint8_t data[MAXBS]; } cache[NCACHE];
static uint32_t cache_clock;
unsigned long ext2_reads, ext2_writes;

static int cache_find(uint32_t blk)
{
    int i;
    for (i = 0; i < NCACHE; i++) {
        if (cache[i].valid && cache[i].blk == blk) {
            cache[i].age = ++cache_clock;
            return i;
        }
    }
    return -1;
}

/* 入れ物 i をディスクへ出す（汚れていれば） */
static int cache_clean(int i)
{
    if (cache[i].valid && cache[i].dirty) {
        if (dev_wr((uint64_t)cache[i].blk * spb, cache[i].data, spb) != 0) {
            return XE_IO;
        }
        ext2_writes++;
        cache[i].dirty = 0;
    }
    return 0;
}

/* 空きの入れ物（なければいちばん古いものを、汚れていればディスクへ出してから）。失敗 -1 */
static int cache_slot(void)
{
    int i, best = 0;
    for (i = 0; i < NCACHE; i++) {
        if (!cache[i].valid) {
            return i;
        }
        if (cache[i].age < cache[best].age) {
            best = i;
        }
    }
    if (cache_clean(best) != 0) {
        return -1;
    }
    cache[best].valid = 0;
    return best;
}

static int bread(uint32_t blk, void *buf)
{
    int i = cache_find(blk);
    if (i < 0) {
        if (blk >= blocks_count) {
            return XE_IO;
        }
        i = cache_slot();
        if (i < 0) {
            return XE_IO;
        }
        if (dev_rd((uint64_t)blk * spb, cache[i].data, spb) != 0) {
            return XE_IO;
        }
        ext2_reads++;
        cache[i].blk = blk;
        cache[i].valid = 1;
        cache[i].dirty = 0;
        cache[i].age = ++cache_clock;
    }
    mcpy(buf, cache[i].data, bs);
    return 0;
}

/* キャッシュに書いて汚れの印をつける（ブロック 0 も受ける。外からは bwrite を使う） */
static int cache_put(uint32_t blk, const void *buf)
{
    int i;
    if (blk >= blocks_count) {
        return XE_IO;
    }
    i = cache_find(blk);
    if (i < 0) {
        i = cache_slot();
        if (i < 0) {
            return XE_IO;
        }
    }
    mcpy(cache[i].data, buf, bs);
    cache[i].blk = blk;
    cache[i].valid = 1;
    cache[i].dirty = 1;
    cache[i].age = ++cache_clock;
    return 0;
}

static int bwrite(uint32_t blk, const void *buf)
{
    if (blk == 0) {                             /* ブロック 0（起動の領域とスーパーブロック）は write_meta だけ */
        return XE_IO;
    }
    return cache_put(blk, buf);
}

/* 汚れているものを、ブロックの番号の小さい順にディスクへ出す */
static int cache_flush(void)
{
    int r = 0;
    for (;;) {
        int i, best = -1;
        for (i = 0; i < NCACHE; i++) {
            if (cache[i].valid && cache[i].dirty && (best < 0 || cache[i].blk < cache[best].blk)) {
                best = i;
            }
        }
        if (best < 0) {
            return r;
        }
        if (cache_clean(best) != 0) {
            cache[best].dirty = 0;              /* 書けなかったものは諦める（同じところで回り続けない） */
            r = XE_IO;
        }
    }
}

/* スーパーブロック（バイト 1024 から）とグループ記述子の表を書き戻す */
static int write_meta(void)
{
    static uint8_t buf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    uint32_t i;
    p32(sb + 48, now());                        /* s_wtime */
    if (bs == 1024) {
        if (bwrite(1, sb) != 0) {
            return XE_IO;
        }
    } else {
        if (bread(0, buf) != 0) {
            return XE_IO;
        }
        mcpy(buf + 1024, sb, 1024);
        if (cache_put(0, buf) != 0) {            /* ブロック 0 は bwrite で守っているので直接 */
            return XE_IO;
        }
    }
    for (i = 0; i < gdt_blocks; i++) {
        if (bwrite(first_data + 1 + i, gdt + i * bs) != 0) {
            return XE_IO;
        }
    }
    return 0;
}

static uint8_t *gd(uint32_t g)
{
    return gdt + g * 32;
}

/* ---- inode */
typedef struct {
    uint32_t ino;
    uint8_t raw[128];
} Inode;

static uint16_t i_mode(Inode *n) { return g16(n->raw + 0); }
static uint64_t i_size(Inode *n) { return g32(n->raw + 4) | ((i_mode(n) & S_IFMT) == S_IFREG ? (uint64_t)g32(n->raw + 108) << 32 : 0); }
static uint16_t i_links(Inode *n) { return g16(n->raw + 26); }
static uint32_t i_blocks(Inode *n) { return g32(n->raw + 28); }
static uint32_t i_blk(Inode *n, int k) { return g32(n->raw + 40 + 4 * k); }
static void set_size(Inode *n, uint64_t s)
{
    p32(n->raw + 4, (uint32_t)s);
    if ((i_mode(n) & S_IFMT) == S_IFREG) {
        p32(n->raw + 108, (uint32_t)(s >> 32));
    }
}
static void set_blk(Inode *n, int k, uint32_t b) { p32(n->raw + 40 + 4 * k, b); }

static int iloc(uint32_t ino, uint32_t *blk, uint32_t *off)
{
    uint32_t g, idx, byte;
    if (ino == 0 || ino > inodes_count) {
        return XE_INVAL;
    }
    g = (ino - 1) / ipg;
    idx = (ino - 1) % ipg;
    byte = idx * inode_size;
    *blk = g32(gd(g) + 8) + byte / bs;
    *off = byte % bs;
    return 0;
}

static int iget(uint32_t ino, Inode *n)
{
    static uint8_t buf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    uint32_t blk, off;
    if (iloc(ino, &blk, &off) != 0 || bread(blk, buf) != 0) {
        return XE_IO;
    }
    n->ino = ino;
    mcpy(n->raw, buf + off, 128);
    return 0;
}

static int iput(Inode *n)
{
    static uint8_t buf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    uint32_t blk, off;
    if (iloc(n->ino, &blk, &off) != 0 || bread(blk, buf) != 0) {
        return XE_IO;
    }
    mcpy(buf + off, n->raw, 128);
    return bwrite(blk, buf);
}

/* ---- ビットマップでの割り当て */
static int bit_alloc(uint32_t bitmap_blk, uint32_t limit, uint32_t start)
{
    static uint8_t buf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    uint32_t i;
    if (bread(bitmap_blk, buf) != 0) {
        return -1;
    }
    for (i = 0; i < limit; i++) {
        uint32_t k = (start + i) % limit;
        if (!(buf[k >> 3] & (1 << (k & 7)))) {
            buf[k >> 3] |= (uint8_t)(1 << (k & 7));
            if (bwrite(bitmap_blk, buf) != 0) {
                return -1;
            }
            return (int)k;
        }
    }
    return -1;
}

static int bit_free(uint32_t bitmap_blk, uint32_t k)
{
    static uint8_t buf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    if (bread(bitmap_blk, buf) != 0) {
        return XE_IO;
    }
    buf[k >> 3] &= (uint8_t)~(1 << (k & 7));
    return bwrite(bitmap_blk, buf);
}

static uint32_t group_blocks(uint32_t g)
{
    uint32_t left = blocks_count - first_data - g * bpg;
    return left < bpg ? left : bpg;
}

/* ブロックを 1 つ取る（0 で埋める）。goal のグループから探す。取れなければ 0 */
static uint32_t balloc(uint32_t goal_group, Inode *owner)
{
    uint32_t t, g;
    static uint8_t z[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    if (g32(sb + 12) == 0) {
        return 0;
    }
    for (t = 0; t < ngroups; t++) {
        g = (goal_group + t) % ngroups;
        if (g16(gd(g) + 12) > 0) {
            int k = bit_alloc(g32(gd(g) + 0), group_blocks(g), 0);
            if (k >= 0) {
                uint32_t blk = first_data + g * bpg + (uint32_t)k;
                p16(gd(g) + 12, (uint16_t)(g16(gd(g) + 12) - 1));
                p32(sb + 12, g32(sb + 12) - 1);
                mset(z, 0, bs);
                if (bwrite(blk, z) != 0 || write_meta() != 0) {
                    return 0;
                }
                if (owner) {
                    p32(owner->raw + 28, i_blocks(owner) + spb);
                }
                return blk;
            }
        }
    }
    return 0;
}

static void bfree(uint32_t blk, Inode *owner)
{
    uint32_t g, k;
    if (blk < first_data || blk >= blocks_count) {
        return;
    }
    g = (blk - first_data) / bpg;
    k = (blk - first_data) % bpg;
    if (bit_free(g32(gd(g) + 0), k) != 0) {
        return;
    }
    p16(gd(g) + 12, (uint16_t)(g16(gd(g) + 12) + 1));
    p32(sb + 12, g32(sb + 12) + 1);
    write_meta();
    if (owner && i_blocks(owner) >= spb) {
        p32(owner->raw + 28, i_blocks(owner) - spb);
    }
}

static uint32_t ialloc(uint32_t goal_group, int dir)
{
    uint32_t t, g, first_ino = g32(sb + 76) >= 1 ? g32(sb + 84) : 11;
    if (g32(sb + 16) == 0) {
        return 0;
    }
    for (t = 0; t < ngroups; t++) {
        g = (goal_group + t) % ngroups;
        if (g16(gd(g) + 14) > 0) {
            uint32_t start = g == 0 ? first_ino - 1 : 0;
            int k = bit_alloc(g32(gd(g) + 4), ipg, start);
            if (k >= 0) {
                uint32_t ino = g * ipg + (uint32_t)k + 1;
                if (ino < first_ino) {          /* 予約の inode には触らない */
                    bit_free(g32(gd(g) + 4), (uint32_t)k);
                    continue;
                }
                p16(gd(g) + 14, (uint16_t)(g16(gd(g) + 14) - 1));
                if (dir) {
                    p16(gd(g) + 16, (uint16_t)(g16(gd(g) + 16) + 1));
                }
                p32(sb + 16, g32(sb + 16) - 1);
                if (write_meta() != 0) {
                    return 0;
                }
                return ino;
            }
        }
    }
    return 0;
}

static void ifree(uint32_t ino, int dir)
{
    uint32_t g = (ino - 1) / ipg, k = (ino - 1) % ipg;
    if (bit_free(g32(gd(g) + 4), k) != 0) {
        return;
    }
    p16(gd(g) + 14, (uint16_t)(g16(gd(g) + 14) + 1));
    if (dir && g16(gd(g) + 16) > 0) {
        p16(gd(g) + 16, (uint16_t)(g16(gd(g) + 16) - 1));
    }
    p32(sb + 16, g32(sb + 16) + 1);
    write_meta();
}

/* ---- ファイルの中のブロック番号 → ディスクのブロック番号（alloc なら作る） */
static uint32_t bmap(Inode *n, uint64_t fb, int alloc, int *err)
{
    uint32_t path[4], depth, i, blk, goal = (n->ino - 1) / ipg;
    static uint8_t buf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    uint64_t p1 = ptrs, p2 = p1 * p1;

    *err = 0;
    if (fb < 12) {
        blk = i_blk(n, (int)fb);
        if (!blk && alloc) {
            blk = balloc(goal, n);
            if (!blk) {
                *err = XE_NOSPC;
                return 0;
            }
            set_blk(n, (int)fb, blk);
        }
        return blk;
    }
    fb -= 12;
    if (fb < p1) {
        depth = 1;
        path[0] = 12;
        path[1] = (uint32_t)fb;
    } else if ((fb -= p1) < p2) {
        depth = 2;
        path[0] = 13;
        path[1] = (uint32_t)(fb / p1);
        path[2] = (uint32_t)(fb % p1);
    } else if ((fb -= p2) < p2 * p1) {
        depth = 3;
        path[0] = 14;
        path[1] = (uint32_t)(fb / p2);
        path[2] = (uint32_t)((fb / p1) % p1);
        path[3] = (uint32_t)(fb % p1);
    } else {
        *err = XE_FBIG;
        return 0;
    }
    blk = i_blk(n, (int)path[0]);
    if (!blk) {
        if (!alloc) {
            return 0;
        }
        blk = balloc(goal, n);
        if (!blk) {
            *err = XE_NOSPC;
            return 0;
        }
        set_blk(n, (int)path[0], blk);
    }
    for (i = 1; i <= depth; i++) {
        uint32_t next;
        if (bread(blk, buf) != 0) {
            *err = XE_IO;
            return 0;
        }
        next = g32(buf + 4 * path[i]);
        if (!next) {
            if (!alloc) {
                return 0;
            }
            next = balloc(goal, n);
            if (!next) {
                *err = XE_NOSPC;
                return 0;
            }
            if (bread(blk, buf) != 0) {         /* balloc がキャッシュを動かしたかもしれない */
                *err = XE_IO;
                return 0;
            }
            p32(buf + 4 * path[i], next);
            if (bwrite(blk, buf) != 0) {
                *err = XE_IO;
                return 0;
            }
        }
        blk = next;
    }
    return blk;
}

/* ---- 切り詰め: from（ファイルの中のブロック番号）から後ろのブロックを返す */
static uint8_t lvl[4][MAXBS];

/* level 段の間接ブロック blk の下を、from から後ろだけ返す。下がすべて空になったら 1 */
static int trunc_ind(uint32_t blk, int level, uint64_t base, uint64_t from, Inode *n)
{
    uint64_t span = 1;
    uint32_t i;
    int dirty = 0, empty = 1;
    for (i = 1; i < (uint32_t)level; i++) {
        span *= ptrs;
    }
    if (bread(blk, lvl[level]) != 0) {
        return 0;
    }
    for (i = 0; i < ptrs; i++) {
        uint32_t child = g32(lvl[level] + 4 * i);
        uint64_t cbase = base + i * span;
        if (!child) {
            continue;
        }
        if (cbase + span <= from) {
            empty = 0;
            continue;
        }
        if (level == 1) {
            bfree(child, n);
            p32(lvl[level] + 4 * i, 0);
            dirty = 1;
        } else if (trunc_ind(child, level - 1, cbase, from, n)) {
            bread(blk, lvl[level]);             /* 下の段で上書きされたので読み直す */
            bfree(child, n);
            p32(lvl[level] + 4 * i, 0);
            dirty = 1;
        } else {
            bread(blk, lvl[level]);
            empty = 0;
        }
    }
    if (dirty && !empty) {
        bwrite(blk, lvl[level]);
    }
    return empty;
}

static void trunc_blocks(Inode *n, uint64_t from)
{
    uint64_t p1 = ptrs;
    int k;
    for (k = 0; k < 12; k++) {
        if ((uint64_t)k >= from && i_blk(n, k)) {
            bfree(i_blk(n, k), n);
            set_blk(n, k, 0);
        }
    }
    if (i_blk(n, 12) && trunc_ind(i_blk(n, 12), 1, 12, from, n)) {
        bfree(i_blk(n, 12), n);
        set_blk(n, 12, 0);
    }
    if (i_blk(n, 13) && trunc_ind(i_blk(n, 13), 2, 12 + p1, from, n)) {
        bfree(i_blk(n, 13), n);
        set_blk(n, 13, 0);
    }
    if (i_blk(n, 14) && trunc_ind(i_blk(n, 14), 3, 12 + p1 + p1 * p1, from, n)) {
        bfree(i_blk(n, 14), n);
        set_blk(n, 14, 0);
    }
}

static int is_fast_symlink(Inode *n)
{
    return (i_mode(n) & S_IFMT) == S_IFLNK && g32(n->raw + 104) == 0 && i_blocks(n) == 0;
}

/* ---- 中身の読み書き */
static long rw_inode(Inode *n, uint64_t off, void *buf, size_t len, int write)
{
    static uint8_t blkbuf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    size_t done = 0;
    uint64_t size = i_size(n);
    int err;

    if (!write) {
        if (off >= size) {
            return 0;
        }
        if (len > size - off) {
            len = (size_t)(size - off);
        }
    }
    while (done < len) {
        uint64_t fb = (off + done) / bs;
        uint32_t in = (uint32_t)((off + done) % bs), chunk = bs - in, blk;
        if (chunk > len - done) {
            chunk = (uint32_t)(len - done);
        }
        blk = bmap(n, fb, write, &err);
        if (err) {
            break;
        }
        if (write) {
            if (chunk != bs && bread(blk, blkbuf) != 0) {
                err = XE_IO;
                break;
            }
            mcpy(blkbuf + in, (const uint8_t *)buf + done, chunk);
            if (bwrite(blk, blkbuf) != 0) {
                err = XE_IO;
                break;
            }
        } else if (blk == 0) {
            mset((uint8_t *)buf + done, 0, chunk);      /* 穴 */
        } else {
            if (bread(blk, blkbuf) != 0) {
                err = XE_IO;
                break;
            }
            mcpy((uint8_t *)buf + done, blkbuf + in, chunk);
        }
        done += chunk;
    }
    if (write && done > 0) {
        if (off + done > size) {
            set_size(n, off + done);
        }
        p32(n->raw + 16, now());                        /* mtime */
        p32(n->raw + 12, now());
        iput(n);
    } else if (write) {
        iput(n);                                        /* 途中で取ったブロックの数を残す */
    }
    if (done == 0 && len > 0) {
        return err ? err : XE_IO;
    }
    return (long)done;
}

/* ---- ディレクトリ */
static int name_eq(const uint8_t *e, const char *name, size_t nl)
{
    size_t i;
    if (e[6] != nl) {
        return 0;
    }
    for (i = 0; i < nl; i++) {
        if (e[8 + i] != (uint8_t)name[i]) {
            return 0;
        }
    }
    return 1;
}

static int dir_lookup(Inode *dir, const char *name, size_t nl, uint32_t *ino)
{
    static uint8_t buf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    uint64_t size = i_size(dir), fb;
    int err;
    for (fb = 0; fb * bs < size; fb++) {
        uint32_t blk = bmap(dir, fb, 0, &err), off = 0;
        if (!blk || bread(blk, buf) != 0) {
            continue;
        }
        while (off + 8 <= bs) {
            uint8_t *e = buf + off;
            uint16_t rl = g16(e + 4);
            if (rl < 8 || off + rl > bs) {
                break;
            }
            if (g32(e) && name_eq(e, name, nl)) {
                *ino = g32(e);
                return 0;
            }
            off += rl;
        }
    }
    return XE_NOENT;
}

static uint16_t ent_size(size_t nl)
{
    return (uint16_t)((8 + nl + 3) & ~3u);
}

static int dir_add(Inode *dir, const char *name, size_t nl, uint32_t ino, uint8_t ft)
{
    static uint8_t buf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    uint64_t size = i_size(dir), fb;
    uint16_t need = ent_size(nl);
    uint32_t blk;
    int err;

    for (fb = 0; fb * bs < size; fb++) {
        uint32_t off = 0;
        blk = bmap(dir, fb, 0, &err);
        if (!blk || bread(blk, buf) != 0) {
            continue;
        }
        while (off + 8 <= bs) {
            uint8_t *e = buf + off;
            uint16_t rl = g16(e + 4), used = g32(e) ? ent_size(e[6]) : 0;
            if (rl < 8 || off + rl > bs) {
                break;
            }
            if (rl - used >= need) {
                uint8_t *ne = e + used;
                if (used) {
                    p16(e + 4, used);
                }
                p32(ne, ino);
                p16(ne + 4, (uint16_t)(rl - used));
                ne[6] = (uint8_t)nl;
                ne[7] = ft;
                mcpy(ne + 8, name, nl);
                if (bwrite(blk, buf) != 0) {
                    return XE_IO;
                }
                p32(dir->raw + 16, now());
                return iput(dir);
            }
            off += rl;
        }
    }
    /* 新しいブロックを足す */
    blk = bmap(dir, fb, 1, &err);
    if (!blk) {
        iput(dir);
        return err ? err : XE_NOSPC;
    }
    mset(buf, 0, bs);
    p32(buf, ino);
    p16(buf + 4, (uint16_t)bs);
    buf[6] = (uint8_t)nl;
    buf[7] = ft;
    mcpy(buf + 8, name, nl);
    if (bwrite(blk, buf) != 0) {
        return XE_IO;
    }
    set_size(dir, (fb + 1) * bs);
    p32(dir->raw + 16, now());
    return iput(dir);
}

static int dir_remove(Inode *dir, const char *name, size_t nl)
{
    static uint8_t buf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    uint64_t size = i_size(dir), fb;
    int err;
    for (fb = 0; fb * bs < size; fb++) {
        uint32_t blk = bmap(dir, fb, 0, &err), off = 0, prev = 0xffffffffu;
        if (!blk || bread(blk, buf) != 0) {
            continue;
        }
        while (off + 8 <= bs) {
            uint8_t *e = buf + off;
            uint16_t rl = g16(e + 4);
            if (rl < 8 || off + rl > bs) {
                break;
            }
            if (g32(e) && name_eq(e, name, nl)) {
                if (prev != 0xffffffffu) {
                    p16(buf + prev + 4, (uint16_t)(g16(buf + prev + 4) + rl));   /* 前のものに併せる */
                } else {
                    p32(e, 0);                              /* ブロックの先頭は inode を 0 に */
                }
                if (bwrite(blk, buf) != 0) {
                    return XE_IO;
                }
                p32(dir->raw + 16, now());
                return iput(dir);
            }
            prev = off;
            off += rl;
        }
    }
    return XE_NOENT;
}

/* ディレクトリの name を、別の inode に付け替える（rename で使う） */
static int dir_set(Inode *dir, const char *name, size_t nl, uint32_t ino, uint8_t ft)
{
    static uint8_t buf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    uint64_t size = i_size(dir), fb;
    int err;
    for (fb = 0; fb * bs < size; fb++) {
        uint32_t blk = bmap(dir, fb, 0, &err), off = 0;
        if (!blk || bread(blk, buf) != 0) {
            continue;
        }
        while (off + 8 <= bs) {
            uint8_t *e = buf + off;
            uint16_t rl = g16(e + 4);
            if (rl < 8 || off + rl > bs) {
                break;
            }
            if (g32(e) && name_eq(e, name, nl)) {
                p32(e, ino);
                e[7] = ft;
                return bwrite(blk, buf);
            }
            off += rl;
        }
    }
    return XE_NOENT;
}

static int dir_empty(Inode *dir)
{
    static uint8_t buf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    uint64_t size = i_size(dir), fb;
    int err;
    for (fb = 0; fb * bs < size; fb++) {
        uint32_t blk = bmap(dir, fb, 0, &err), off = 0;
        if (!blk || bread(blk, buf) != 0) {
            continue;
        }
        while (off + 8 <= bs) {
            uint8_t *e = buf + off;
            uint16_t rl = g16(e + 4);
            if (rl < 8 || off + rl > bs) {
                break;
            }
            if (g32(e) && !(e[6] == 1 && e[8] == '.') && !(e[6] == 2 && e[8] == '.' && e[9] == '.')) {
                return 0;
            }
            off += rl;
        }
    }
    return 1;
}

/* ---- 開いている inode（消されても、最後に閉じるまで中身を残す） */
#define NOPEN 64
static struct { uint32_t ino; int refs; int orphan; } opened[NOPEN];

static int open_slot(uint32_t ino)
{
    int i;
    for (i = 0; i < NOPEN; i++) {
        if (opened[i].refs > 0 && opened[i].ino == ino) {
            return i;
        }
    }
    return -1;
}

/* inode を本当に捨てる（リンクが 0 で、開いている人もいない） */
static void destroy(Inode *n)
{
    int dir = (i_mode(n) & S_IFMT) == S_IFDIR;
    if (!is_fast_symlink(n)) {
        trunc_blocks(n, 0);
    }
    set_size(n, 0);
    p32(n->raw + 20, now());                            /* dtime */
    p16(n->raw + 26, 0);
    iput(n);
    ifree(n->ino, dir);
}

static void drop_link(Inode *n)
{
    int s;
    if (i_links(n) > 0) {
        p16(n->raw + 26, (uint16_t)(i_links(n) - 1));
    }
    p32(n->raw + 12, now());
    if (i_links(n) > 0) {
        iput(n);
        return;
    }
    s = open_slot(n->ino);
    if (s >= 0) {
        opened[s].orphan = 1;                           /* 閉じたときに捨てる */
        iput(n);
        return;
    }
    destroy(n);
}

/* ---- パス（ファイルシステムの根からの相対。"" は根） */
static char mount_prefix[32] = "/data";

static long read_link(Inode *n, char *buf, size_t max)
{
    uint64_t size = i_size(n);
    if (size >= max) {
        return XE_NAMETOOLONG;
    }
    if (is_fast_symlink(n)) {
        mcpy(buf, n->raw + 40, (size_t)size);
    } else {
        long r = rw_inode(n, 0, buf, (size_t)size, 0);
        if (r < 0) {
            return r;
        }
        size = (uint64_t)r;
    }
    buf[size] = 0;
    return (long)size;
}

/* 文字列を足す。入りきらなければ 0 */
static int cat(char *dst, size_t *pos, size_t max, const char *src, size_t n)
{
    size_t i;
    if (*pos + n + 1 > max) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        dst[(*pos)++] = src[i];
    }
    dst[*pos] = 0;
    return 1;
}

/* path をたどる。途中（または follow_last のとき最後）にシンボリックリンクがあれば、
 * リンクの先を組み込んだ新しいパス（ファイルの仕組み全体の絶対パス）を redir に書いて EXT2_REDIRECT を返す */
static int walk(const char *path, int follow_last, uint32_t *out, char *redir)
{
    uint32_t cur = EXT2_ROOT_INO;
    const char *p = path;
    Inode n;

    for (;;) {
        const char *s;
        size_t nl;
        uint32_t next;
        int last, r;
        while (*p == '/') {
            p++;
        }
        if (*p == 0) {
            *out = cur;
            return 0;
        }
        s = p;
        while (*p && *p != '/') {
            p++;
        }
        nl = (size_t)(p - s);
        last = 1;
        {
            const char *q = p;
            while (*q == '/') {
                q++;
            }
            last = *q == 0;
        }
        if (nl > 255) {
            return XE_NAMETOOLONG;
        }
        if (iget(cur, &n) != 0) {
            return XE_IO;
        }
        if ((i_mode(&n) & S_IFMT) != S_IFDIR) {
            return XE_NOTDIR;
        }
        if (nl == 1 && s[0] == '.') {
            continue;
        }
        r = dir_lookup(&n, s, nl, &next);
        if (r != 0) {
            return r;
        }
        if (iget(next, &n) != 0) {
            return XE_IO;
        }
        if ((i_mode(&n) & S_IFMT) == S_IFLNK && (!last || follow_last)) {
            static char target[EXT2_PATH_MAX];
            size_t pos = 0;
            long k = read_link(&n, target, sizeof(target));
            if (k < 0) {
                return (int)k;
            }
            if (!redir) {
                return XE_LOOP;
            }
            redir[0] = 0;
            if (target[0] != '/') {                     /* 相対: このファイルシステムの中の、いまの場所から */
                if (!cat(redir, &pos, EXT2_PATH_MAX, mount_prefix, slen(mount_prefix))
                    || !cat(redir, &pos, EXT2_PATH_MAX, "/", 1)
                    || !cat(redir, &pos, EXT2_PATH_MAX, path, (size_t)(s - path))) {
                    return XE_NAMETOOLONG;
                }
            }
            while (*p == '/') {
                p++;
            }
            if (!cat(redir, &pos, EXT2_PATH_MAX, target, (size_t)k)
                || (*p && (!cat(redir, &pos, EXT2_PATH_MAX, "/", 1)
                           || !cat(redir, &pos, EXT2_PATH_MAX, p, slen(p))))) {
                return XE_NAMETOOLONG;
            }
            return EXT2_REDIRECT;
        }
        cur = next;
    }
}

/* path の最後の名前の手前まで: 親ディレクトリの inode と、最後の名前 */
static int split_parent(const char *path, uint32_t *parent, const char **name, size_t *nl, char *redir)
{
    static char dirpath[EXT2_PATH_MAX];
    size_t n = slen(path), i, last;
    Inode d;
    int r;
    while (n > 0 && path[n - 1] == '/') {
        n--;
    }
    if (n == 0) {
        return XE_EXIST;                                 /* 根そのもの */
    }
    last = n;
    while (last > 0 && path[last - 1] != '/') {
        last--;
    }
    if (n - last > 255) {
        return XE_NAMETOOLONG;
    }
    if (last >= sizeof(dirpath)) {
        return XE_NAMETOOLONG;
    }
    for (i = 0; i < last; i++) {
        dirpath[i] = path[i];
    }
    dirpath[last] = 0;
    r = walk(dirpath, 1, parent, redir);
    if (r == EXT2_REDIRECT) {                           /* 親の途中のリンク: 最後の名前を後ろに付ける */
        size_t pos = slen(redir);
        if (!cat(redir, &pos, EXT2_PATH_MAX, path + last, n - last)) {
            return XE_NAMETOOLONG;
        }
        return r;
    }
    if (r != 0) {
        return r;
    }
    if (iget(*parent, &d) != 0) {
        return XE_IO;
    }
    if ((i_mode(&d) & S_IFMT) != S_IFDIR) {
        return XE_NOTDIR;
    }
    *name = path + last;
    *nl = n - last;
    if (*nl == 1 && (*name)[0] == '.') {
        return XE_INVAL;
    }
    if (*nl == 2 && (*name)[0] == '.' && (*name)[1] == '.') {
        return XE_INVAL;
    }
    return 0;
}

/* 新しい inode を作る（親の中に名前も付ける） */
static int make_node(uint32_t parent, const char *name, size_t nl, uint16_t mode, uint32_t *out, Inode *n)
{
    Inode dir;
    uint32_t ino, t = now();
    int r, isdir = (mode & S_IFMT) == S_IFDIR;
    uint8_t ft = isdir ? FT_DIR : (mode & S_IFMT) == S_IFLNK ? FT_LNK : FT_REG;

    if (iget(parent, &dir) != 0) {
        return XE_IO;
    }
    if (isdir && i_links(&dir) >= 32000) {
        return XE_MLINK;
    }
    ino = ialloc((parent - 1) / ipg, isdir);
    if (!ino) {
        return XE_NOSPC;
    }
    mset(n->raw, 0, 128);
    n->ino = ino;
    p16(n->raw + 0, mode);
    p32(n->raw + 8, t);
    p32(n->raw + 12, t);
    p32(n->raw + 16, t);
    p16(n->raw + 26, isdir ? 2 : 1);
    if (iput(n) != 0) {
        return XE_IO;
    }
    r = dir_add(&dir, name, nl, ino, ft);
    if (r != 0) {
        ifree(ino, isdir);
        return r;
    }
    *out = ino;
    return 0;
}

/* ================================================================ 外への口（すべてロックの中で） */

int ext2_mount(int (*rd)(uint64_t, void *, unsigned), int (*wr)(uint64_t, const void *, unsigned),
               uint64_t nsectors, uint32_t (*clock)(void))
{
    uint32_t inc, roc;
    int r = 0;
    lock();
    mounted = 0;
    dev_rd = rd;
    dev_wr = wr;
    now_fn = clock;
    mset(cache, 0, sizeof(cache));
    if (nsectors < 8 || dev_rd(2, sb, 2) != 0) {
        r = XE_IO;
        goto out;
    }
    if (g16(sb + 56) != 0xEF53) {
        r = XE_INVAL;                                    /* ext2 ではない */
        goto out;
    }
    bs = 1024u << g32(sb + 24);
    if (bs < 1024 || bs > MAXBS) {
        r = XE_INVAL;
        goto out;
    }
    spb = bs / 512;
    inodes_count = g32(sb + 0);
    blocks_count = g32(sb + 4);
    first_data = g32(sb + 20);
    bpg = g32(sb + 32);
    ipg = g32(sb + 40);
    inode_size = g32(sb + 76) >= 1 ? g16(sb + 88) : 128;
    inc = g32(sb + 76) >= 1 ? g32(sb + 96) : 0;
    roc = g32(sb + 76) >= 1 ? g32(sb + 100) : 0;
    if (inc & ~0x2u) {                                  /* filetype のほかは扱えない */
        r = XE_INVAL;
        goto out;
    }
    readonly = (roc & ~0x3u) != 0;                      /* sparse_super・large_file のほかがあれば読むだけ */
    if (bpg == 0 || ipg == 0 || inode_size < 128 || (uint64_t)blocks_count * spb > nsectors) {
        r = XE_INVAL;
        goto out;
    }
    ngroups = (blocks_count - first_data + bpg - 1) / bpg;
    gdt_blocks = (ngroups * 32 + bs - 1) / bs;
    if (gdt_blocks * bs > sizeof(gdt)) {
        r = XE_INVAL;
        goto out;
    }
    ptrs = bs / 4;
    {
        uint32_t i;
        for (i = 0; i < gdt_blocks; i++) {
            if (bread(first_data + 1 + i, gdt + i * bs) != 0) {
                r = XE_IO;
                goto out;
            }
        }
    }
    mset(opened, 0, sizeof(opened));
    mounted = 1;
    if (!readonly) {
        p32(sb + 44, now());                            /* s_mtime */
        p16(sb + 52, (uint16_t)(g16(sb + 52) + 1));     /* s_mnt_count */
        write_meta();
    }
out:
    unlock();
    return r;
}

/* ためてあるものをディスクへ出す。成功 0 */
int ext2_sync(void)
{
    int r;
    if (!mounted || readonly) {
        return 0;
    }
    lock();
    r = cache_flush();
    unlock();
    return r;
}

/* 出していないブロックの数（テスト用） */
int ext2_dirty_blocks(void)
{
    int i, n = 0;
    for (i = 0; i < NCACHE; i++) {
        n += cache[i].valid && cache[i].dirty;
    }
    return n;
}

#ifndef EXT2_HOST
/* 見回り: 1 秒ごとに、ためてある分をディスクへ出す（プロセスの優先度 10 より先に動く 9） */
static void sync_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;
    for (;;) {
        tk_dly_tsk(1000);
        ext2_sync();
    }
}

void ext2_start_sync_task(void)
{
    static int started;
    T_CTSK ct;
    unsigned char *p = (unsigned char *)&ct;
    unsigned i;
    ID tid;
    if (started || !mounted || readonly) {
        return;
    }
    for (i = 0; i < sizeof(ct); i++) {
        p[i] = 0;
    }
    ct.tskatr = TA_HLNG | TA_RNG0;
    ct.itskpri = 9;
    ct.stksz = 4096;
    ct.task = (FP)sync_task;
    tid = tk_cre_tsk(&ct);
    if (tid > 0 && tk_sta_tsk(tid, 0) == E_OK) {
        started = 1;
    }
}
#endif

int ext2_mounted(void)
{
    return mounted;
}

int ext2_readonly(void)
{
    return readonly;
}

void ext2_set_prefix(const char *p)
{
    size_t i;
    for (i = 0; p[i] && i + 1 < sizeof(mount_prefix); i++) {
        mount_prefix[i] = p[i];
    }
    mount_prefix[i] = 0;
}

int ext2_lookup(const char *path, int follow, uint32_t *ino, char *redir)
{
    int r;
    if (!mounted) {
        return XE_NOENT;
    }
    lock();
    r = walk(path, follow, ino, redir);
    unlock();
    return r;
}

int ext2_stat(uint32_t ino, struct ext2_stat *st)
{
    Inode n;
    int r = 0;
    lock();
    if (iget(ino, &n) != 0) {
        r = XE_IO;
    } else {
        st->ino = ino;
        st->mode = i_mode(&n);
        st->nlink = i_links(&n);
        st->uid = g16(n.raw + 2);
        st->gid = g16(n.raw + 24);
        st->size = i_size(&n);
        st->blocks = i_blocks(&n);
        st->atime = g32(n.raw + 8);
        st->mtime = g32(n.raw + 16);
        st->ctime = g32(n.raw + 12);
        st->blksize = bs;
    }
    unlock();
    return r;
}

long ext2_read(uint32_t ino, uint64_t off, void *buf, size_t len)
{
    Inode n;
    long r;
    lock();
    if (iget(ino, &n) != 0) {
        r = XE_IO;
    } else if ((i_mode(&n) & S_IFMT) == S_IFDIR) {
        r = XE_ISDIR;
    } else {
        r = rw_inode(&n, off, buf, len, 0);
    }
    unlock();
    return r;
}

long ext2_write(uint32_t ino, uint64_t off, const void *buf, size_t len)
{
    Inode n;
    long r;
    if (readonly) {
        return XE_ROFS;
    }
    lock();
    if (iget(ino, &n) != 0) {
        r = XE_IO;
    } else if ((i_mode(&n) & S_IFMT) != S_IFREG) {
        r = XE_ISDIR;
    } else {
        r = rw_inode(&n, off, (void *)buf, len, 1);
    }
    unlock();
    return r;
}

int ext2_truncate(uint32_t ino, uint64_t len)
{
    Inode n;
    int r = 0;
    if (readonly) {
        return XE_ROFS;
    }
    lock();
    if (iget(ino, &n) != 0) {
        r = XE_IO;
    } else if ((i_mode(&n) & S_IFMT) != S_IFREG) {
        r = XE_ISDIR;
    } else {
        uint64_t old = i_size(&n);
        if (len < old) {
            uint64_t keep = (len + bs - 1) / bs;
            trunc_blocks(&n, keep);
            if (len % bs) {                             /* 残る最後のブロックの後ろを 0 に */
                static uint8_t buf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
                int err;
                uint32_t blk = bmap(&n, len / bs, 0, &err);
                if (blk && bread(blk, buf) == 0) {
                    mset(buf + len % bs, 0, bs - len % bs);
                    bwrite(blk, buf);
                }
            }
        }
        set_size(&n, len);
        p32(n.raw + 16, now());
        p32(n.raw + 12, now());
        r = iput(&n);
    }
    unlock();
    return r;
}

/* 普通のファイルを開く・作る。flags: EXT2_CREAT・EXT2_EXCL・EXT2_TRUNC */
int ext2_open(const char *path, int flags, uint16_t perm, uint32_t *ino, char *redir)
{
    uint32_t parent;
    const char *name;
    size_t nl;
    Inode n;
    int r;
    if (!mounted) {
        return XE_NOENT;
    }
    lock();
    r = walk(path, 1, ino, redir);
    if (r == 0) {
        if ((flags & EXT2_CREAT) && (flags & EXT2_EXCL)) {
            r = XE_EXIST;
        } else if ((flags & EXT2_TRUNC) && iget(*ino, &n) == 0 && (i_mode(&n) & S_IFMT) == S_IFREG) {
            if (readonly) {
                r = XE_ROFS;
            } else {
                unlock();
                r = ext2_truncate(*ino, 0);
                return r;
            }
        }
    } else if (r == XE_NOENT && (flags & EXT2_CREAT)) {
        if (readonly) {
            r = XE_ROFS;
        } else {
            r = split_parent(path, &parent, &name, &nl, redir);
            if (r == 0) {
                uint32_t dummy;
                Inode dir;
                if (iget(parent, &dir) == 0 && dir_lookup(&dir, name, nl, &dummy) == 0) {
                    r = XE_EXIST;                        /* 壊れたリンクなど */
                } else {
                    r = make_node(parent, name, nl, (uint16_t)(S_IFREG | (perm & 07777)), ino, &n);
                }
            }
        }
    }
    unlock();
    return r;
}

int ext2_mkdir(const char *path, uint16_t perm, char *redir)
{
    uint32_t parent, ino, dummy;
    const char *name;
    size_t nl;
    Inode n, dir;
    static uint8_t buf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    int r, err;
    uint32_t blk;
    if (readonly) {
        return XE_ROFS;
    }
    lock();
    r = split_parent(path, &parent, &name, &nl, redir);
    if (r != 0) {
        goto out;
    }
    if (iget(parent, &dir) != 0) {
        r = XE_IO;
        goto out;
    }
    if (dir_lookup(&dir, name, nl, &dummy) == 0) {
        r = XE_EXIST;
        goto out;
    }
    r = make_node(parent, name, nl, (uint16_t)(S_IFDIR | (perm & 07777)), &ino, &n);
    if (r != 0) {
        goto out;
    }
    blk = bmap(&n, 0, 1, &err);
    if (!blk) {
        r = err ? err : XE_NOSPC;
        goto out;
    }
    mset(buf, 0, bs);
    p32(buf, ino);                                      /* "." */
    p16(buf + 4, 12);
    buf[6] = 1;
    buf[7] = FT_DIR;
    buf[8] = '.';
    p32(buf + 12, parent);                              /* ".." */
    p16(buf + 16, (uint16_t)(bs - 12));
    buf[18] = 2;
    buf[19] = FT_DIR;
    buf[20] = '.';
    buf[21] = '.';
    if (bwrite(blk, buf) != 0) {
        r = XE_IO;
        goto out;
    }
    set_size(&n, bs);
    iput(&n);
    if (iget(parent, &dir) == 0) {                      /* 親の ".." の数 */
        p16(dir.raw + 26, (uint16_t)(i_links(&dir) + 1));
        iput(&dir);
    }
    r = 0;
out:
    unlock();
    return r;
}

int ext2_symlink(const char *target, const char *path, char *redir)
{
    uint32_t parent, ino, dummy;
    const char *name;
    size_t nl, tl = slen(target);
    Inode n, dir;
    int r;
    if (readonly) {
        return XE_ROFS;
    }
    if (tl == 0 || tl >= EXT2_PATH_MAX) {
        return XE_NAMETOOLONG;
    }
    lock();
    r = split_parent(path, &parent, &name, &nl, redir);
    if (r == 0 && iget(parent, &dir) == 0 && dir_lookup(&dir, name, nl, &dummy) == 0) {
        r = XE_EXIST;
    }
    if (r == 0) {
        r = make_node(parent, name, nl, (uint16_t)(S_IFLNK | 0777), &ino, &n);
    }
    if (r == 0) {
        if (tl < 60) {                                  /* 短いリンクは inode の中に */
            mcpy(n.raw + 40, target, tl);
            set_size(&n, tl);
            r = iput(&n);
        } else {
            long k = rw_inode(&n, 0, (void *)target, tl, 1);
            r = k < 0 ? (int)k : 0;
        }
    }
    unlock();
    return r;
}

long ext2_readlink(const char *path, char *buf, size_t max, char *redir)
{
    uint32_t ino;
    Inode n;
    long r;
    lock();
    r = walk(path, 0, &ino, redir);
    if (r == 0) {
        if (iget(ino, &n) != 0) {
            r = XE_IO;
        } else if ((i_mode(&n) & S_IFMT) != S_IFLNK) {
            r = XE_INVAL;
        } else {
            r = read_link(&n, buf, max);
        }
    }
    unlock();
    return r;
}

/* 名前を消す（dir = 1 なら rmdir） */
int ext2_remove(const char *path, int dir, char *redir)
{
    uint32_t parent, ino;
    const char *name;
    size_t nl;
    Inode n, d;
    int r;
    if (readonly) {
        return XE_ROFS;
    }
    lock();
    r = split_parent(path, &parent, &name, &nl, redir);
    if (r == XE_EXIST) {
        r = dir ? XE_INVAL : XE_ISDIR;                    /* 根は消せない */
    }
    if (r != 0) {
        goto out;
    }
    if (iget(parent, &d) != 0 || dir_lookup(&d, name, nl, &ino) != 0) {
        r = XE_NOENT;
        goto out;
    }
    if (iget(ino, &n) != 0) {
        r = XE_IO;
        goto out;
    }
    if (dir) {
        if ((i_mode(&n) & S_IFMT) != S_IFDIR) {
            r = XE_NOTDIR;
            goto out;
        }
        if (!dir_empty(&n)) {
            r = XE_NOTEMPTY;
            goto out;
        }
    } else if ((i_mode(&n) & S_IFMT) == S_IFDIR) {
        r = XE_ISDIR;
        goto out;
    }
    r = dir_remove(&d, name, nl);
    if (r != 0) {
        goto out;
    }
    if (dir) {
        p16(n.raw + 26, 1);                             /* "." の分を先に減らす */
        if (iget(parent, &d) == 0 && i_links(&d) > 2) {  /* 親の ".." の分 */
            p16(d.raw + 26, (uint16_t)(i_links(&d) - 1));
            iput(&d);
        }
    }
    drop_link(&n);
out:
    unlock();
    return r;
}

int ext2_rename(const char *from, const char *to, char *redir)
{
    uint32_t pf, pt, ino, old;
    const char *nf, *nt;
    size_t lf, lt;
    Inode n, dir, victim;
    int r, isdir;
    if (readonly) {
        return XE_ROFS;
    }
    lock();
    r = split_parent(from, &pf, &nf, &lf, redir);
    if (r == 0) {
        r = split_parent(to, &pt, &nt, &lt, redir);
    }
    if (r == XE_EXIST) {
        r = XE_INVAL;
    }
    if (r != 0) {
        goto out;
    }
    if (iget(pf, &dir) != 0 || dir_lookup(&dir, nf, lf, &ino) != 0) {
        r = XE_NOENT;
        goto out;
    }
    if (iget(ino, &n) != 0) {
        r = XE_IO;
        goto out;
    }
    isdir = (i_mode(&n) & S_IFMT) == S_IFDIR;
    if (isdir) {                                        /* 自分の下へは移せない */
        uint32_t up = pt;
        int guard = 0;
        while (up != EXT2_ROOT_INO && guard++ < 1000) {
            Inode u;
            if (up == ino) {
                r = XE_INVAL;
                goto out;
            }
            if (iget(up, &u) != 0 || dir_lookup(&u, "..", 2, &up) != 0) {
                break;
            }
        }
    }
    if (iget(pt, &dir) != 0) {
        r = XE_IO;
        goto out;
    }
    if (dir_lookup(&dir, nt, lt, &old) == 0) {
        if (old == ino) {
            r = 0;                                      /* 同じもの */
            goto out;
        }
        if (iget(old, &victim) != 0) {
            r = XE_IO;
            goto out;
        }
        if ((i_mode(&victim) & S_IFMT) == S_IFDIR) {
            if (!isdir) {
                r = XE_ISDIR;
                goto out;
            }
            if (!dir_empty(&victim)) {
                r = XE_NOTEMPTY;
                goto out;
            }
        } else if (isdir) {
            r = XE_NOTDIR;
            goto out;
        }
        r = dir_set(&dir, nt, lt, ino, isdir ? FT_DIR : (i_mode(&n) & S_IFMT) == S_IFLNK ? FT_LNK : FT_REG);
        if (r != 0) {
            goto out;
        }
        if ((i_mode(&victim) & S_IFMT) == S_IFDIR) {
            p16(victim.raw + 26, 1);
            if (iget(pt, &dir) == 0 && i_links(&dir) > 2) {
                p16(dir.raw + 26, (uint16_t)(i_links(&dir) - 1));
                iput(&dir);
            }
        }
        drop_link(&victim);
    } else {
        r = dir_add(&dir, nt, lt, ino, isdir ? FT_DIR : (i_mode(&n) & S_IFMT) == S_IFLNK ? FT_LNK : FT_REG);
        if (r != 0) {
            goto out;
        }
    }
    if (iget(pf, &dir) != 0) {
        r = XE_IO;
        goto out;
    }
    r = dir_remove(&dir, nf, lf);
    if (r != 0) {
        goto out;
    }
    if (isdir && pf != pt) {                            /* ".." を新しい親へ */
        if (iget(ino, &n) == 0) {
            dir_set(&n, "..", 2, pt, FT_DIR);
        }
        if (iget(pf, &dir) == 0) {
            p16(dir.raw + 26, (uint16_t)(i_links(&dir) - 1));
            iput(&dir);
        }
        if (iget(pt, &dir) == 0) {
            p16(dir.raw + 26, (uint16_t)(i_links(&dir) + 1));
            iput(&dir);
        }
    }
    if (iget(ino, &n) == 0) {
        p32(n.raw + 12, now());
        iput(&n);
    }
    r = 0;
out:
    unlock();
    return r;
}

/* ハードリンク: from（ディレクトリでないもの）に、to の名前をもう 1 つ付ける */
int ext2_link(const char *from, const char *to, char *redir)
{
    uint32_t ino, parent, dummy;
    const char *name;
    size_t nl;
    Inode n, dir;
    int r;
    if (readonly) {
        return XE_ROFS;
    }
    lock();
    r = walk(from, 0, &ino, redir);
    if (r == 0) {
        r = split_parent(to, &parent, &name, &nl, redir);
    }
    if (r == XE_EXIST) {
        r = XE_EXIST;
    }
    if (r != 0) {
        goto out;
    }
    if (iget(ino, &n) != 0 || iget(parent, &dir) != 0) {
        r = XE_IO;
        goto out;
    }
    if ((i_mode(&n) & S_IFMT) == S_IFDIR) {
        r = -1;                                         /* EPERM: ディレクトリには付けられない */
        goto out;
    }
    if (dir_lookup(&dir, name, nl, &dummy) == 0) {
        r = XE_EXIST;
        goto out;
    }
    if (i_links(&n) >= 32000) {
        r = XE_MLINK;
        goto out;
    }
    r = dir_add(&dir, name, nl, ino, (i_mode(&n) & S_IFMT) == S_IFLNK ? FT_LNK : FT_REG);
    if (r == 0) {
        if (iget(ino, &n) == 0) {
            p16(n.raw + 26, (uint16_t)(i_links(&n) + 1));
            p32(n.raw + 12, now());
            r = iput(&n);
        }
    }
out:
    unlock();
    return r;
}

int ext2_chmod(uint32_t ino, uint16_t perm)
{
    Inode n;
    int r;
    if (readonly) {
        return XE_ROFS;
    }
    lock();
    r = iget(ino, &n);
    if (r == 0) {
        p16(n.raw + 0, (uint16_t)((i_mode(&n) & S_IFMT) | (perm & 07777)));
        p32(n.raw + 12, now());
        r = iput(&n);
    }
    unlock();
    return r;
}

int ext2_utime(uint32_t ino, uint32_t atime, uint32_t mtime)
{
    Inode n;
    int r;
    if (readonly) {
        return XE_ROFS;
    }
    lock();
    r = iget(ino, &n);
    if (r == 0) {
        p32(n.raw + 8, atime);
        p32(n.raw + 16, mtime);
        r = iput(&n);
    }
    unlock();
    return r;
}

/* ディレクトリを 1 つずつ読む。*pos は次に読む位置（最初は 0）。読めたら 1、終わりなら 0 */
int ext2_readdir(uint32_t ino, uint64_t *pos, char *name, size_t max, uint32_t *child, uint8_t *type)
{
    Inode n;
    static uint8_t buf[MAXBS];          /* スタックを使わない（ロックの中なので 1 つで足りる） */
    int r = 0, err;
    lock();
    if (iget(ino, &n) != 0 || (i_mode(&n) & S_IFMT) != S_IFDIR) {
        unlock();
        return XE_NOTDIR;
    }
    while (*pos < i_size(&n)) {
        uint32_t blk = bmap(&n, *pos / bs, 0, &err), off = (uint32_t)(*pos % bs);
        if (!blk || bread(blk, buf) != 0) {
            *pos = (*pos / bs + 1) * bs;
            continue;
        }
        if (off + 8 > bs) {
            *pos = (*pos / bs + 1) * bs;
            continue;
        }
        {
            uint8_t *e = buf + off;
            uint16_t rl = g16(e + 4);
            if (rl < 8 || off + rl > bs) {
                *pos = (*pos / bs + 1) * bs;
                continue;
            }
            *pos += rl;
            if (g32(e) && e[6] < max) {
                mcpy(name, e + 8, e[6]);
                name[e[6]] = 0;
                *child = g32(e);
                *type = e[7];
                r = 1;
                break;
            }
        }
    }
    unlock();
    return r;
}

void ext2_iref(uint32_t ino)
{
    int i;
    lock();
    i = open_slot(ino);
    if (i < 0) {
        for (i = 0; i < NOPEN; i++) {
            if (opened[i].refs == 0) {
                opened[i].ino = ino;
                opened[i].orphan = 0;
                break;
            }
        }
    }
    if (i >= 0 && i < NOPEN) {
        opened[i].refs++;
    }
    unlock();
}

void ext2_iunref(uint32_t ino)
{
    int i;
    lock();
    i = open_slot(ino);
    if (i >= 0 && --opened[i].refs == 0 && opened[i].orphan) {
        Inode n;
        opened[i].orphan = 0;
        if (iget(ino, &n) == 0 && i_links(&n) == 0) {
            destroy(&n);
        }
    }
    unlock();
}

void ext2_statfs(uint64_t *bsize, uint64_t *blocks, uint64_t *bfree, uint64_t *files, uint64_t *ffree)
{
    *bsize = bs;
    *blocks = blocks_count;
    *bfree = g32(sb + 12);
    *files = inodes_count;
    *ffree = g32(sb + 16);
}
