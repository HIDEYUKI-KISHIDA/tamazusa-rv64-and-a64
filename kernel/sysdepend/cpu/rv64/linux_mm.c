/* linux_mm.c — Linux 互換層のメモリの写し方（mmap・munmap・mremap・memfd_create）（S-mode + MMU のみ）
 *
 *   Wayland の下回りの 1 つ目。Wayland では、アプリが窓の絵を「名前のないファイル」（memfd）に描き、
 *   その fd を合成役へ渡し、合成役も同じファイルを mmap して読む。そのための形:
 *     ・memfd_create: /tmp と同じメモリの上のファイルを、名前なしで作る（linux_tmpfs.c）
 *     ・mmap(MAP_SHARED, fd): そのファイルの中身のページを、そのまま写す（写した所どうしで同じページ）。
 *       fork した子にも同じページのまま引き継ぐ（ページ表の印 PTE_SHARED、uspace.c）
 *     ・mmap(MAP_PRIVATE, fd): ファイルの中身を写し取った自分だけのページ（書き換えてもファイルは変わらない）
 *     ・mmap(MAP_SHARED | MAP_ANONYMOUS): fork した親子で共有する無名のページ
 *     ・mremap(MREMAP_MAYMOVE): 写した所を広げる・縮める（Wayland の合成役が、アプリの共有のプールを広げるとき）
 *   写した所（VMA）をプロセスごとに 64 個まで覚えておく（mremap で広げるとき、中身がどこから来たかを知るため）。
 */
#include <stdint.h>
#include <stddef.h>
#include <rv_priv.h>

struct rv_uproc;
extern int rv_uproc_slot(const struct rv_uproc *p);
extern uintptr_t rv_linux_mmap_anon2(struct rv_uproc *p, size_t len, int shared);
extern uintptr_t rv_linux_mmap_reserve(struct rv_uproc *p, size_t len);
extern int rv_linux_map_shared(struct rv_uproc *p, uintptr_t va, void *kpage, int writable);
extern int rv_linux_map_anon_at(struct rv_uproc *p, uintptr_t lo, uintptr_t hi, int shared);
extern int rv_linux_move_pages(struct rv_uproc *p, uintptr_t old, uintptr_t nva, size_t npages);
extern int rv_linux_munmap(struct rv_uproc *p, uintptr_t addr, size_t len);
extern int rv_linux_fd_kind(struct rv_uproc *p, long fd, int *idx, int *acc);
extern long rv_linux_fd_pread(struct rv_uproc *p, long fd, uint64_t off, uintptr_t ubuf, size_t len);
extern int rv_linux_fd_install_tmp(struct rv_uproc *p, int idx, int cloexec);
extern int rv_tmp_create_anon(void);
extern void *rv_tmp_page(int i, unsigned pi, int alloc);
extern uint64_t rv_tmp_size(int i);
extern uint64_t rv_tmp_max_size(void);
extern void rv_tmp_ref(int i);
extern void rv_tmp_unref(int i);
extern int rv_user_copy_in(void *dst, uintptr_t uva, size_t len);
extern int tk_dis_dsp(void);
extern int tk_ena_dsp(void);

#define PAGE        4096UL
#define SLOTS       32
#define NVMA        64

#define PROT_WRITE      2
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_SHARED_VALIDATE 0x03
#define MAP_TYPE        0x0f
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MREMAP_MAYMOVE  1
#define MREMAP_FIXED    2
#define MFD_CLOEXEC     1

#define EBADF   9
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define ENODEV  19
#define EINVAL  22

/* fd の種類（rv_linux_fd_kind の戻り値） */
#define FK_NONE 0
#define FK_TMP  1           /* メモリの上のファイル（/tmp・memfd）: 共有で写せる */
#define FK_FILE 2           /* 読めるファイル（tar・/data）: 写し取りだけ */

struct vma {
    uintptr_t lo, hi;       /* [lo, hi)。lo = 0 は空き */
    int tmp;                /* 中身のファイル（memfd・/tmp）の番号。無名は -1 */
    uint64_t off;           /* ファイルの何バイト目から */
    uint8_t shared, writable;
};
static struct vma vmas[SLOTS][NVMA];

static size_t pages(size_t len)
{
    return (len + PAGE - 1) / PAGE;
}

static struct vma *vma_new(int s)
{
    int i;
    for (i = 0; i < NVMA; i++) {
        if (vmas[s][i].lo == 0) {
            return &vmas[s][i];
        }
    }
    return NULL;
}

static void vma_drop(struct vma *v)
{
    if (v->tmp >= 0) {
        rv_tmp_unref(v->tmp);               /* 写している間はファイルを生かしておく */
    }
    v->lo = v->hi = 0;
    v->tmp = -1;
}

/* [lo, hi) を覚えている所から外す（一部だけなら切り詰める・2 つに分ける） */
static void vma_cut(int s, uintptr_t lo, uintptr_t hi)
{
    int i;
    for (i = 0; i < NVMA; i++) {
        struct vma *v = &vmas[s][i];
        if (v->lo == 0 || hi <= v->lo || lo >= v->hi) {
            continue;
        }
        if (lo <= v->lo && hi >= v->hi) {
            vma_drop(v);
        } else if (lo <= v->lo) {           /* 頭を切る */
            v->off += hi - v->lo;
            v->lo = hi;
        } else if (hi >= v->hi) {           /* 尻を切る */
            v->hi = lo;
        } else {                            /* 真ん中に穴: 後ろ半分を別に */
            struct vma *w = vma_new(s);
            if (w != NULL) {
                *w = *v;
                w->lo = hi;
                w->off = v->off + (hi - v->lo);
                if (w->tmp >= 0) {
                    rv_tmp_ref(w->tmp);
                }
            }
            v->hi = lo;
        }
    }
}

/* ファイルのページ [off, off+len) を va から共有で写す */
static int map_tmp_pages(struct rv_uproc *p, uintptr_t va, int tmp, uint64_t off, size_t len, int writable)
{
    size_t k, n = pages(len);
    uint64_t size = rv_tmp_size(tmp);
    for (k = 0; k < n; k++) {
        uint64_t fo = off + k * PAGE;
        void *pg;
        if (fo >= size) {
            break;                          /* ファイルの終わりより先は写さない（触ると止まる。Linux の SIGBUS） */
        }
        pg = rv_tmp_page(tmp, (unsigned)(fo / PAGE), 1);
        if (pg == NULL || rv_linux_map_shared(p, va + k * PAGE, pg, writable) != 0) {
            return -1;
        }
    }
    return 0;
}

static long do_mmap(struct rv_uproc *p, uintptr_t addr, size_t len, long prot, long flags, long fd, uint64_t off)
{
    int s = rv_uproc_slot(p);
    int type = (int)(flags & MAP_TYPE);
    int shared = (type == MAP_SHARED || type == MAP_SHARED_VALIDATE);
    struct vma *v = NULL, tmpv;
    uintptr_t va;
    (void)addr;

    if (len == 0 || (off & (PAGE - 1)) || s < 0 || s >= SLOTS || (type != MAP_PRIVATE && !shared)) {
        return -EINVAL;
    }
    if (flags & MAP_FIXED) {
        return -EINVAL;                     /* 場所の指定は、まだ受けない */
    }
    v = &tmpv;
    if (flags & MAP_ANONYMOUS) {
        va = rv_linux_mmap_anon2(p, len, shared);
        if (va == 0) {
            return -ENOMEM;
        }
        v->tmp = -1;
    } else {
        int idx = -1, acc = 0;
        int kind = rv_linux_fd_kind(p, fd, &idx, &acc);
        if (kind == FK_NONE) {
            return fd < 0 ? -EBADF : -ENODEV;
        }
        if (acc == 1 || (shared && (prot & PROT_WRITE) && acc != 2)) {
            return -EACCES;                 /* 書くだけで開いた fd・読むだけの fd を書ける形で共有 */
        }
        if (shared && kind == FK_TMP) {
            if (off > rv_tmp_max_size()) {
                return -EINVAL;
            }
            if (vma_new(s) == NULL) {
                return -ENOMEM;                 /* 共有の写しは覚えておかないと、ファイルを返す時が分からない */
            }
            va = rv_linux_mmap_reserve(p, len);
            if (va == 0) {
                return -ENOMEM;
            }
            tk_dis_dsp();
            if (map_tmp_pages(p, va, idx, off, len, (prot & PROT_WRITE) != 0) != 0) {
                tk_ena_dsp();
                rv_linux_munmap(p, va, len);
                return -ENOMEM;
            }
            tk_ena_dsp();
            rv_tmp_ref(idx);
            v->tmp = idx;
        } else if (shared && (prot & PROT_WRITE)) {
            return -ENODEV;                 /* /data などのファイルを書ける形で共有するのは、まだ */
        } else {
            long r;
            va = rv_linux_mmap_anon2(p, len, 0);
            if (va == 0) {
                return -ENOMEM;
            }
            r = rv_linux_fd_pread(p, fd, off, va, len);  /* 写し取る（足りない所は 0 のまま） */
            if (r < 0) {
                rv_linux_munmap(p, va, len);
                return r;
            }
            v->tmp = -1;
            shared = 0;
        }
    }
    v->lo = va;
    v->hi = va + pages(len) * PAGE;
    v->off = off;
    v->shared = (uint8_t)shared;
    v->writable = (prot & PROT_WRITE) != 0;
    {
        struct vma *w = vma_new(s);         /* 覚える（いっぱいなら、無名・写し取りは覚えずに使える） */
        if (w != NULL) {
            *w = *v;
        }
    }
    return (long)va;
}

static long do_munmap(struct rv_uproc *p, uintptr_t addr, size_t len)
{
    int s = rv_uproc_slot(p);
    long r = rv_linux_munmap(p, addr, len);
    if (r == 0 && s >= 0 && s < SLOTS) {
        vma_cut(s, addr, addr + pages(len) * PAGE);
    }
    return r;
}

static long do_mremap(struct rv_uproc *p, uintptr_t old, size_t olen, size_t nlen, long flags)
{
    int s = rv_uproc_slot(p), i;
    struct vma *v = NULL;
    size_t op = pages(olen), np = pages(nlen);
    uintptr_t nva;

    if ((old & (PAGE - 1)) || nlen == 0 || s < 0 || s >= SLOTS || (flags & ~(long)MREMAP_MAYMOVE)) {
        return -EINVAL;
    }
    for (i = 0; i < NVMA; i++) {
        if (vmas[s][i].lo == old && vmas[s][i].hi == old + op * PAGE) {
            v = &vmas[s][i];
        }
    }
    if (v == NULL) {
        return -EFAULT;                     /* 1 つの mmap の全体だけを扱う */
    }
    if (np <= op) {                         /* 縮める: 尻を外す */
        if (np < op) {
            rv_linux_munmap(p, old + np * PAGE, (op - np) * PAGE);
            v->hi = old + np * PAGE;
        }
        return (long)old;
    }
    if (!(flags & MREMAP_MAYMOVE)) {
        return -ENOMEM;                     /* その場で広げるのは、まだ */
    }
    nva = rv_linux_mmap_reserve(p, np * PAGE);
    if (nva == 0 || rv_linux_move_pages(p, old, nva, op) != 0) {
        return -ENOMEM;
    }
    if (v->tmp >= 0) {
        tk_dis_dsp();
        i = map_tmp_pages(p, nva + op * PAGE, v->tmp, v->off + op * PAGE, (np - op) * PAGE, v->writable);
        tk_ena_dsp();
    } else {
        i = rv_linux_map_anon_at(p, nva + op * PAGE, nva + np * PAGE, v->shared);
    }
    if (i != 0) {
        rv_linux_munmap(p, nva, np * PAGE);
        vma_drop(v);
        return -ENOMEM;
    }
    v->lo = nva;
    v->hi = nva + np * PAGE;
    return (long)nva;
}

static long do_memfd(struct rv_uproc *p, uintptr_t uname, long flags)
{
    char name[8];
    int idx, fd;
    if (uname != 0 && rv_user_copy_in(name, uname, 1) != 0) {
        return -EFAULT;
    }
    idx = rv_tmp_create_anon();
    if (idx < 0) {
        return idx;
    }
    fd = rv_linux_fd_install_tmp(p, idx, (flags & MFD_CLOEXEC) != 0);
    if (fd < 0) {
        rv_tmp_ref(idx);
        rv_tmp_unref(idx);                  /* 誰も使っていないので返す */
    }
    return fd;
}

/* linux_syscall.c から。扱ったら 1 */
int rv_linux_mm_syscall(struct rv_uproc *p, long nr, const uint64_t *a, long *ret)
{
    switch (nr) {
    case 222:                               /* mmap */
        *ret = do_mmap(p, (uintptr_t)a[0], (size_t)a[1], (long)a[2], (long)a[3], (long)a[4], a[5]);
        return 1;
    case 215:                               /* munmap */
        *ret = do_munmap(p, (uintptr_t)a[0], (size_t)a[1]);
        return 1;
    case 216:                               /* mremap */
        *ret = do_mremap(p, (uintptr_t)a[0], (size_t)a[1], (size_t)a[2], (long)a[3]);
        return 1;
    case 279:                               /* memfd_create */
        *ret = do_memfd(p, (uintptr_t)a[0], (long)a[1]);
        return 1;
    }
    return 0;
}

/* fork: 覚えている所を子へ写す（ページ表は uspace.c が写す） */
void rv_linux_mm_fork(int ps, int cs)
{
    int i;
    if (ps < 0 || ps >= SLOTS || cs < 0 || cs >= SLOTS) {
        return;
    }
    for (i = 0; i < NVMA; i++) {
        vmas[cs][i] = vmas[ps][i];
        if (vmas[cs][i].lo != 0 && vmas[cs][i].tmp >= 0) {
            rv_tmp_ref(vmas[cs][i].tmp);
        }
    }
}

/* 終わり・execve: 覚えている所を忘れる（ページは uspace.c が返す） */
void rv_linux_mm_release(int s)
{
    int i;
    if (s < 0 || s >= SLOTS) {
        return;
    }
    for (i = 0; i < NVMA; i++) {
        if (vmas[s][i].lo != 0) {
            vma_drop(&vmas[s][i]);
        }
    }
}
