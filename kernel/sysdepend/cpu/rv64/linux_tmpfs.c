/* linux_tmpfs.c — メモリの上の書き込めるフォルダ /tmp（Linux 互換層）
 *
 *   ディスク（tar）は読み取り専用なので、書き込みはここで受ける。電源を切ると消える。
 *   ・/tmp の直下にファイルだけ置ける（中にフォルダは作れない）
 *   ・ファイルの中身はユーザー用のページ（4KiB）を借りて持つ。1 つ 8MiB まで、全部で 64 個まで
 *   ・名前のないファイル（memfd_create）もここに作る。中身のページは mmap で共有して写せる（linux_mm.c）
 *   ・開いている間に消されたファイルは、最後に閉じたときに中身を返す（Unix と同じ）
 *   linux_fs.c から呼ばれる。名前は「/tmp/」を外したもの。
 */
#include <stdint.h>
#include <stddef.h>

#define TMP_MAX     64
#define TMP_PAGES   2048            /* 1 ファイル 8MiB まで（1024×768 の 32 ビットの絵 2 枚分） */
#define TMP_NAME    64
#define PAGE_SIZE   4096

#define ENOENT  2
#define EFAULT  14
#define EEXIST  17
#define EINVAL  22
#define EFBIG   27
#define ENOSPC  28
#define ENAMETOOLONG 36

extern void *rv_kpage_alloc(void);
extern void  rv_kpage_free(void *p);
extern int   rv_user_copy_in(void *dst, uintptr_t usrc, size_t n);
extern int   rv_user_copy_out(uintptr_t udst, const void *src, size_t n);
extern int   tk_dis_dsp(void);
extern int   tk_ena_dsp(void);

struct tmpf {
    int used;                       /* 1 = 名前がある、2 = 消されたが開いている */
    char name[TMP_NAME];
    uint64_t size;
    int refs;                       /* 開いている fd の数 */
    uint8_t *pg[TMP_PAGES];
};
static struct tmpf tf[TMP_MAX];

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int name_ok(const char *n)
{
    int k;
    if (n[0] == 0 || (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0)))) {
        return -EINVAL;
    }
    for (k = 0; n[k]; k++) {
        if (n[k] == '/') {
            return -ENOENT;         /* /tmp の中のフォルダはない */
        }
    }
    return k < TMP_NAME ? 0 : -ENAMETOOLONG;
}

static void drop_pages(struct tmpf *t, uint64_t keep)
{
    unsigned i, first = (unsigned)((keep + PAGE_SIZE - 1) / PAGE_SIZE);
    for (i = first; i < TMP_PAGES; i++) {
        if (t->pg[i]) {
            rv_kpage_free(t->pg[i]);
            t->pg[i] = NULL;
        }
    }
}

static void release(struct tmpf *t)
{
    drop_pages(t, 0);
    t->used = 0;
    t->size = 0;
    t->refs = 0;
    t->name[0] = 0;
}

/* 名前で探す。番号か -ENOENT */
int rv_tmp_lookup(const char *name)
{
    int i;
    for (i = 0; i < TMP_MAX; i++) {
        if (tf[i].used == 1 && same(tf[i].name, name)) {
            return i;
        }
    }
    return -ENOENT;
}

/* 作る（あれば -EEXIST）。番号か負のエラー */
int rv_tmp_create(const char *name)
{
    int i, e = name_ok(name);
    if (e < 0) {
        return e;
    }
    tk_dis_dsp();
    if (rv_tmp_lookup(name) >= 0) {
        tk_ena_dsp();
        return -EEXIST;
    }
    for (i = 0; i < TMP_MAX; i++) {
        if (tf[i].used == 0) {
            int k;
            tf[i].used = 1;
            for (k = 0; name[k]; k++) {
                tf[i].name[k] = name[k];
            }
            tf[i].name[k] = 0;
            tf[i].size = 0;
            tf[i].refs = 0;
            tk_ena_dsp();
            return i;
        }
    }
    tk_ena_dsp();
    return -ENOSPC;
}

/* 名前のないファイル（memfd_create）。最初から「消されたが開いている」形で、最後に閉じたら中身を返す。番号か負のエラー */
int rv_tmp_create_anon(void)
{
    int i;
    tk_dis_dsp();
    for (i = 0; i < TMP_MAX; i++) {
        if (tf[i].used == 0) {
            tf[i].used = 2;
            tf[i].name[0] = 0;
            tf[i].size = 0;
            tf[i].refs = 0;
            tk_ena_dsp();
            return i;
        }
    }
    tk_ena_dsp();
    return -ENOSPC;
}

/* 中身のページ pi（なければ alloc のとき 0 で埋めて作る）。mmap で写すため。なければ NULL */
void *rv_tmp_page(int i, unsigned pi, int alloc)
{
    struct tmpf *t;
    if (i < 0 || i >= TMP_MAX || pi >= TMP_PAGES) {
        return NULL;
    }
    t = &tf[i];
    if (!t->pg[pi] && alloc) {
        t->pg[pi] = rv_kpage_alloc();
    }
    return t->pg[pi];
}

uint64_t rv_tmp_max_size(void)
{
    return (uint64_t)TMP_PAGES * PAGE_SIZE;
}

void rv_tmp_ref(int i)
{
    if (i >= 0 && i < TMP_MAX) {
        tf[i].refs++;
    }
}

void rv_tmp_unref(int i)
{
    if (i < 0 || i >= TMP_MAX) {
        return;
    }
    tk_dis_dsp();
    if (--tf[i].refs <= 0 && tf[i].used == 2) {
        release(&tf[i]);
    }
    tk_ena_dsp();
}

uint64_t rv_tmp_size(int i)
{
    return (i >= 0 && i < TMP_MAX) ? tf[i].size : 0;
}

const char *rv_tmp_name(int i)
{
    return (i >= 0 && i < TMP_MAX) ? tf[i].name : "";
}

long rv_tmp_read(int i, uint64_t off, uintptr_t ubuf, size_t len)
{
    struct tmpf *t = &tf[i];
    size_t done = 0;

    if (off >= t->size) {
        return 0;
    }
    if (len > t->size - off) {
        len = (size_t)(t->size - off);
    }
    while (done < len) {
        uint64_t pos = off + done;
        unsigned pi = (unsigned)(pos / PAGE_SIZE), po = (unsigned)(pos % PAGE_SIZE);
        size_t n = PAGE_SIZE - po;
        if (n > len - done) {
            n = len - done;
        }
        if (t->pg[pi]) {
            if (rv_user_copy_out(ubuf + done, t->pg[pi] + po, n) != 0) {
                return done > 0 ? (long)done : -EFAULT;
            }
        } else {
            static const uint8_t zero[256];
            size_t z = 0;
            while (z < n) {         /* 書かれていない穴は 0 */
                size_t m = n - z > sizeof(zero) ? sizeof(zero) : n - z;
                if (rv_user_copy_out(ubuf + done + z, zero, m) != 0) {
                    return -EFAULT;
                }
                z += m;
            }
        }
        done += n;
    }
    return (long)done;
}

long rv_tmp_write(int i, uint64_t off, uintptr_t ubuf, size_t len)
{
    struct tmpf *t = &tf[i];
    size_t done = 0;

    if (off >= (uint64_t)TMP_PAGES * PAGE_SIZE) {
        return len ? -EFBIG : 0;
    }
    if (off + len > (uint64_t)TMP_PAGES * PAGE_SIZE) {
        len = (size_t)((uint64_t)TMP_PAGES * PAGE_SIZE - off);
    }
    while (done < len) {
        uint64_t pos = off + done;
        unsigned pi = (unsigned)(pos / PAGE_SIZE), po = (unsigned)(pos % PAGE_SIZE);
        size_t n = PAGE_SIZE - po;
        if (n > len - done) {
            n = len - done;
        }
        if (!t->pg[pi]) {
            t->pg[pi] = rv_kpage_alloc();
            if (!t->pg[pi]) {
                break;              /* ページが足りない */
            }
        }
        if (rv_user_copy_in(t->pg[pi] + po, ubuf + done, n) != 0) {
            return done > 0 ? (long)done : -EFAULT;
        }
        done += n;
        if (off + done > t->size) {
            t->size = off + done;
        }
    }
    if (done == 0 && len > 0) {
        return -ENOSPC;
    }
    return (long)done;
}

int rv_tmp_truncate(int i, uint64_t len)
{
    struct tmpf *t = &tf[i];
    if (len > (uint64_t)TMP_PAGES * PAGE_SIZE) {
        return -EFBIG;
    }
    if (len < t->size) {
        drop_pages(t, len);
        if (len % PAGE_SIZE && t->pg[len / PAGE_SIZE]) {
            uint8_t *p = t->pg[len / PAGE_SIZE];
            unsigned k;
            for (k = (unsigned)(len % PAGE_SIZE); k < PAGE_SIZE; k++) {
                p[k] = 0;           /* あとで伸ばしたとき 0 に見えるように */
            }
        }
    }
    t->size = len;
    return 0;
}

int rv_tmp_unlink(const char *name)
{
    int i;
    tk_dis_dsp();
    i = rv_tmp_lookup(name);
    if (i < 0) {
        tk_ena_dsp();
        return i;
    }
    if (tf[i].refs > 0) {
        tf[i].used = 2;             /* 開いている: 最後に閉じたときに消す */
        tf[i].name[0] = 0;
    } else {
        release(&tf[i]);
    }
    tk_ena_dsp();
    return 0;
}

int rv_tmp_rename(const char *from, const char *to)
{
    int i, j, k, e = name_ok(to);
    if (e < 0) {
        return e;
    }
    tk_dis_dsp();
    i = rv_tmp_lookup(from);
    if (i < 0) {
        tk_ena_dsp();
        return i;
    }
    j = rv_tmp_lookup(to);
    if (j == i) {
        tk_ena_dsp();
        return 0;
    }
    if (j >= 0) {                   /* 行き先があれば置き換える（Unix の rename と同じ） */
        if (tf[j].refs > 0) {
            tf[j].used = 2;
            tf[j].name[0] = 0;
        } else {
            release(&tf[j]);
        }
    }
    for (k = 0; to[k]; k++) {
        tf[i].name[k] = to[k];
    }
    tf[i].name[k] = 0;
    tk_ena_dsp();
    return 0;
}

/* 一覧: index 番目以降で最初にあるファイル。見つかった番号（次は +1 から）、なければ -1 */
int rv_tmp_next(int index)
{
    int i;
    for (i = index < 0 ? 0 : index; i < TMP_MAX; i++) {
        if (tf[i].used == 1) {
            return i;
        }
    }
    return -1;
}

/* テスト用: ファイルの数 */
int rv_tmp_count(void)
{
    int i, n = 0;
    for (i = 0; i < TMP_MAX; i++) {
        n += tf[i].used != 0;
    }
    return n;
}
