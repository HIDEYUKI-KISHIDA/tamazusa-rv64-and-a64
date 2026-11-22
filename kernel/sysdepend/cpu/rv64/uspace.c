/*
 * uspace.c — 構成 B-2 第5段: ユーザープログラム（ELF）の読み込み（S-mode + MMU のみ）
 *
 *   ページ割り当て: 静的な領域（RV_PAGE_POOL ページ）からの 4KiB 単位の割り当て
 *   rv_uproc_load : ELF64（RISC-V、ET_EXEC）を検査し、PT_LOAD ごとに必要なページだけ
 *                   割り当てて写す。権限は p_flags どおり（R / W / X、U 付き）。
 *                   ファイルにない部分（.bss）は 0。スタックは 2 ページ、その下は未割り当て。
 *   rv_uproc_free : そのプログラムのページ表とページをすべて返す
 *   rv_user_copy_str: 実行中の空間のページ表をたどってユーザーの文字列を読む
 *                   （U と R が立っているページだけ。未割り当てなら途中で止める）
 *
 * ユーザー空間のアドレス（0〜2GiB。ページ表の最上段の 0 番と 1 番をプログラムごとに持つ）
 *   0x0000_1000 〜 0x5FFF_FFFF : ELF のセグメント（Linux 用の ELF は 0x10000 から）と brk のヒープ
 *   0x6000_0000 〜 0x7EFF_FFFF : mmap の領域
 *   0x7FFF_F000 から下        : スタック（従来のプログラムは 2 ページ、Linux 用は 32 ページ）
 * カーネルの部分（RAM と、高いアドレスの装置）はカーネルのページ表と同じ表を共有する。
 * 装置は高いアドレスから触る（rv_priv.h の RV_IO）ので、低い 0〜1GiB もユーザーに渡せる。
 *
 * AArch64 版もこのファイルを使う。ページ表の記述子の形・空間の値（satp / TTBR0）・ユーザー空間の範囲は
 * <mmu_arch.h>（cpu/rv64 と cpu/aarch64 に 1 つずつ）にある。AArch64 では RAM が 0x4000_0000 からなので、
 * ユーザー空間は 0〜1GiB（最上段の 0 番だけ）。
 */
#include <stdint.h>
#include <stddef.h>
#include <rv_priv.h>
#include <mmu_arch.h>               /* ページ表の CPU に依る部分（RISC-V: cpu/rv64、AArch64: cpu/aarch64） */

#define PAGE_SIZE       4096UL
#define STACK_PAGES_PLAIN   2
#define STACK_PAGES_LINUX   32

extern pte_t *rv_mmu_kernel_root(void);

/* ---- ページ割り当て ---- */

#define RV_PAGE_POOL    12288           /* 48MiB */
static uint8_t page_pool[RV_PAGE_POOL][PAGE_SIZE] __attribute__((aligned(4096)));
static uint16_t page_used[RV_PAGE_POOL];    /* 参照の数（0 = 空き）。共有のページは写したところの数だけ増える */
static int page_free_count = RV_PAGE_POOL;
static int page_hint;                       /* 次に探し始める場所 */

static void *page_alloc(void)
{
    int i;
    unsigned k;

    for (k = 0; k < RV_PAGE_POOL; k++) {
        i = (page_hint + (int)k) % RV_PAGE_POOL;
        if (!page_used[i]) {
            uint64_t *q = (uint64_t *)page_pool[i];
            unsigned w;
            page_used[i] = 1;
            page_free_count--;
            page_hint = i + 1;
            for (w = 0; w < PAGE_SIZE / 8; w++) {
                q[w] = 0;
            }
            return page_pool[i];
        }
    }
    return NULL;
}

static void page_free(void *p)
{
    uintptr_t off = (uintptr_t)p - (uintptr_t)page_pool;
    int i = (int)(off / PAGE_SIZE);

    if ((uintptr_t)p < (uintptr_t)page_pool || i >= RV_PAGE_POOL || !page_used[i]) {
        return;
    }
    if (--page_used[i] == 0) {
        page_free_count++;
        if (i < page_hint) {
            page_hint = i;
        }
    }
}

/* 参照を 1 つ増やす（同じページを別のところにも写すとき）。成功 0 */
static int page_ref(void *p)
{
    uintptr_t off = (uintptr_t)p - (uintptr_t)page_pool;
    int i = (int)(off / PAGE_SIZE);

    if ((uintptr_t)p < (uintptr_t)page_pool || i >= RV_PAGE_POOL || !page_used[i] || page_used[i] == 0xffff) {
        return -1;
    }
    page_used[i]++;
    return 0;
}

int rv_page_free_count(void)
{
    return page_free_count;
}

/* /tmp（linux_tmpfs.c）がファイルの中身に使うページ */
extern int tk_dis_dsp(void);
extern int tk_ena_dsp(void);

void *rv_kpage_alloc(void)
{
    void *p;
    tk_dis_dsp();
    p = page_alloc();
    tk_ena_dsp();
    return p;
}

void rv_kpage_free(void *p)
{
    tk_dis_dsp();
    page_free(p);
    tk_ena_dsp();
}

/* ---- ページ表 ---- */

static pte_t *next_table(pte_t *tbl, unsigned idx, int create)
{
    pte_t e = tbl[idx];

    if (e & PTE_V) {
        if (PTE_IS_LEAF(e, 1)) {
            return NULL;                /* 大きなページ（ここでは使わない） */
        }
        return (pte_t *)(uintptr_t)PTE_TO_PA(e);
    }
    if (!create) {
        return NULL;
    }
    {
        pte_t *t = page_alloc();
        if (t == NULL) {
            return NULL;
        }
        tbl[idx] = MK_TABLE(t);
        return t;
    }
}

/* va のページの PTE の場所（なければ表を作る。作れなければ NULL） */
static pte_t *pte_slot(pte_t *root, uintptr_t va)
{
    pte_t *l1 = next_table(root, VPN2(va), 1);
    pte_t *l0 = l1 ? next_table(l1, VPN1(va), 1) : NULL;
    return l0 ? &l0[VPN0(va)] : NULL;
}

/* 4KiB ページを 1 枚写す。すでに写っていれば -1 */
static int map_page(pte_t *root, uintptr_t va, void *pa, pte_t perm)
{
    pte_t *l1 = next_table(root, VPN2(va), 1);
    pte_t *l0 = l1 ? next_table(l1, VPN1(va), 1) : NULL;

    if (l0 == NULL || (l0[VPN0(va)] & PTE_V)) {
        return -1;
    }
    l0[VPN0(va)] = MK_UPTE(pa, perm);
    return 0;
}

/*
 * VA を引く。葉の PTE を返し（なければ 0）、pa に物理アドレスを入れる。
 * 大きなページ（2MiB / 1GiB）の葉にも対応する。
 */
static pte_t translate(const pte_t *root, uintptr_t va, uintptr_t *pa)
{
    const pte_t *t = root;
    int level;

    for (level = 2; level >= 0; level--) {
        unsigned idx = (level == 2) ? VPN2(va) : (level == 1) ? VPN1(va) : VPN0(va);
        pte_t e = t[idx];

        if (!(e & PTE_V)) {
            return 0;
        }
        if (PTE_IS_LEAF(e, level)) {
            uintptr_t span = PAGE_SIZE << (9 * level);
            if (pa != NULL) {
                *pa = (uintptr_t)PTE_TO_PA(e) + (va & (span - 1));
            }
            return e;
        }
        t = (const pte_t *)(uintptr_t)PTE_TO_PA(e);
    }
    return 0;
}

/* ---- プログラム ---- */

#define RV_UPROC_MAX    32
struct rv_uproc {
    int       used;
    pte_t    *root;
    uint64_t  satp;
    uintptr_t entry;
    uintptr_t sp;
    /* Linux 互換のプログラム */
    int       linux_abi;
    int       pid;
    uintptr_t phdr_va;                  /* プログラムヘッダのユーザーのアドレス（AT_PHDR） */
    unsigned  phnum;
    uintptr_t brk_start, brk_cur;       /* brk のヒープ */
    uintptr_t mmap_next;                /* mmap の次の候補 */
};
static struct rv_uproc uprocs[RV_UPROC_MAX];

static void free_user_tables(pte_t *root)
{
    unsigned i, j, top;

    for (top = 0; top < UVA_VPN2_COUNT; top++) {
    pte_t *l1 = next_table(root, top, 0);
    if (l1 != NULL) {
        for (i = 0; i < 512; i++) {
            pte_t *l0 = next_table(l1, i, 0);
            if (l0 == NULL) {
                continue;
            }
            for (j = 0; j < 512; j++) {
                if (l0[j] & PTE_V) {
                    page_free((void *)(uintptr_t)PTE_TO_PA(l0[j]));
                }
            }
            page_free(l0);
        }
        page_free(l1);
    }
    }
    page_free(root);
}

extern void rv_ufile_release(int slot);
extern void rv_linux_fs_release(int slot);
extern void rv_linux_proc_forget(struct rv_uproc *p);

void rv_uproc_free(struct rv_uproc *p)
{
    if (p == NULL || !p->used) {
        return;
    }
    rv_ufile_release((int)(p - uprocs));     /* 開いたままのファイルを閉じる */
    rv_linux_fs_release((int)(p - uprocs));
    rv_linux_proc_forget(p);
    free_user_tables(p->root);
    p->used = 0;
}

/* ELF64 の定義（必要な分だけ） */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#define PT_LOAD     1
#define PF_X        1
#define PF_W        2
#define PF_R        4
#define ET_EXEC     2

/* 失敗の理由（rv_uproc_load の戻り値が NULL のとき） */
int rv_uproc_last_error;
#define UPE_HEADER  1   /* ELF ヘッダが不正 */
#define UPE_PHDR    2   /* プログラムヘッダが不正 */
#define UPE_RANGE   3   /* セグメントがユーザー領域の外 */
#define UPE_NOMEM   4   /* ページが足りない・重なり */
#define UPE_ENTRY   5   /* 入口が実行可能なページにない */
#define UPE_SLOT    6   /* 同時に読み込めるプログラムの数を超えた */

static struct rv_uproc *fail(struct rv_uproc *p, int err)
{
    rv_uproc_last_error = err;
    if (p != NULL && p->root != NULL) {
        free_user_tables(p->root);
        p->root = NULL;
    }
    if (p != NULL) {
        p->used = 0;
    }
    return NULL;
}

static int load_segment(pte_t *root, const uint8_t *img, const Elf64_Phdr *ph)
{
    uintptr_t va_lo = ph->p_vaddr & ~(PAGE_SIZE - 1);
    uintptr_t va_hi = ph->p_vaddr + ph->p_memsz;
    uintptr_t va;
    pte_t perm = 0;

    if (ph->p_flags & PF_R) perm |= UP_R;
    if (ph->p_flags & PF_W) perm |= UP_W | UP_R;
    if (ph->p_flags & PF_X) perm |= UP_X;

    for (va = va_lo; va < va_hi; va += PAGE_SIZE) {
        pte_t *slot = pte_slot(root, va);
        uint8_t *pg;
        uintptr_t k;

        if (slot == NULL) {
            return UPE_NOMEM;
        }
        if (*slot & PTE_V) {
            /* 前のセグメントと同じページを共有する（境界がページの途中）。権限を足す */
            pg = (uint8_t *)(uintptr_t)PTE_TO_PA(*slot);
            *slot = PTE_ADD_PERM(*slot, perm);
        } else {
            pg = page_alloc();
            if (pg == NULL) {
                return UPE_NOMEM;
            }
            *slot = MK_UPTE(pg, perm);
        }
        /* このページのうちファイルにある部分を写す（残りは 0 のまま = .bss） */
        for (k = 0; k < PAGE_SIZE; k++) {
            uintptr_t a = va + k;
            if (a >= ph->p_vaddr && a < ph->p_vaddr + ph->p_filesz) {
                pg[k] = img[ph->p_offset + (a - ph->p_vaddr)];
            }
        }
    }
    return 0;
}

static struct rv_uproc *uproc_load(const void *image, size_t len, unsigned stack_pages)
{
    uintptr_t seg_end = 0;
    const uint8_t *img = image;
    const Elf64_Ehdr *eh = image;
    struct rv_uproc *p = NULL;
    pte_t *kroot = rv_mmu_kernel_root();
    pte_t ent;
    unsigned i;
    int nload = 0;

    if (len < sizeof(Elf64_Ehdr) || ((uintptr_t)image & 7) != 0
        || img[0] != 0x7f || img[1] != 'E' || img[2] != 'L' || img[3] != 'F'
        || img[4] != 2 /* 64 ビット */ || img[5] != 1 /* リトルエンディアン */
        || eh->e_type != ET_EXEC || eh->e_machine != EM_ARCH) {
        return fail(NULL, UPE_HEADER);
    }
    if (eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0 || eh->e_phnum > 16
        || (eh->e_phoff & 7) != 0 || eh->e_phoff > len
        || len - eh->e_phoff < (size_t)eh->e_phnum * sizeof(Elf64_Phdr)) {
        return fail(NULL, UPE_PHDR);
    }

    for (i = 0; i < RV_UPROC_MAX; i++) {
        if (!uprocs[i].used) {
            p = &uprocs[i];
            break;
        }
    }
    if (p == NULL) {
        return fail(NULL, UPE_SLOT);
    }
    p->used = 1;
    p->root = page_alloc();
    if (p->root == NULL) {
        return fail(p, UPE_NOMEM);
    }
    for (i = UVA_VPN2_COUNT; i < 512; i++) {
        p->root[i] = kroot[i];          /* カーネルの部分は共有（ユーザー部分は持ち込まない） */
    }
    p->linux_abi = 0;
    p->phdr_va = 0;
    p->phnum = eh->e_phnum;

    for (i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(img + eh->e_phoff) + i;
        int err;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (ph->p_filesz > ph->p_memsz || ph->p_offset > len || len - ph->p_offset < ph->p_filesz
            || (ph->p_flags & (PF_R | PF_W | PF_X)) == 0) {
            return fail(p, UPE_PHDR);
        }
        if (ph->p_vaddr < UVA_SEG_LO || ph->p_vaddr >= UVA_SEG_END
            || ph->p_memsz > UVA_SEG_END - ph->p_vaddr) {
            return fail(p, UPE_RANGE);
        }
        err = load_segment(p->root, img, ph);
        if (err != 0) {
            return fail(p, err);
        }
        if (eh->e_phoff >= ph->p_offset && eh->e_phoff < ph->p_offset + ph->p_filesz) {
            p->phdr_va = ph->p_vaddr + (eh->e_phoff - ph->p_offset);
        }
        if (ph->p_vaddr + ph->p_memsz > seg_end) {
            seg_end = ph->p_vaddr + ph->p_memsz;
        }
        nload++;
    }
    if (nload == 0) {
        return fail(p, UPE_PHDR);
    }

    ent = translate(p->root, eh->e_entry, NULL);
    if (!PTE_UX(ent)) {
        return fail(p, UPE_ENTRY);
    }

    /* スタック（その下は未割り当て = はみ出すとページフォールト） */
    for (i = 0; i < stack_pages; i++) {
        void *pg = page_alloc();
        if (pg == NULL || map_page(p->root, UVA_STACK_TOP - (i + 1) * PAGE_SIZE, pg, UP_R | UP_W) != 0) {
            if (pg != NULL) {
                page_free(pg);
            }
            return fail(p, UPE_NOMEM);
        }
    }

    p->entry = eh->e_entry;
    p->sp = UVA_STACK_TOP;
    p->brk_start = p->brk_cur = (seg_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    p->mmap_next = UVA_MMAP_LO;
    p->satp = SPACE_FROM_ROOT(p->root);
    asm volatile("" ::: "memory");
    rv_uproc_last_error = 0;
    return p;
}

struct rv_uproc *rv_uproc_load(const void *image, size_t len)
{
    return uproc_load(image, len, STACK_PAGES_PLAIN);
}

uint64_t  rv_uproc_satp(const struct rv_uproc *p)  { return p->satp; }
int       rv_uproc_slot(const struct rv_uproc *p)  { return (int)(p - uprocs); }

/* 実行中の空間（satp）のプログラムのスロット番号。プログラムの空間でなければ -1 */
int rv_uproc_current_slot(void)
{
    uint64_t satp;
    int i;

    satp = arch_cur_space();
    for (i = 0; i < RV_UPROC_MAX; i++) {
        if (uprocs[i].used && uprocs[i].satp == satp) {
            return i;
        }
    }
    return -1;
}
uintptr_t rv_uproc_entry(const struct rv_uproc *p) { return p->entry; }
uintptr_t rv_uproc_sp(const struct rv_uproc *p)    { return p->sp; }

/* ---- ユーザーのメモリを読む ---- */

/*
 * 実行中の空間（satp）のページ表をたどり、ユーザーの文字列を dst に写す。
 * U と R が立っていないページに来たら、そこで止める。戻り値は写した文字数。
 */
size_t rv_user_copy_str(char *dst, uintptr_t uva, size_t max)
{
    uint64_t satp;
    const pte_t *root;
    size_t n = 0;

    if (max == 0) {
        return 0;
    }
    satp = arch_cur_space();
    root = ROOT_FROM_SPACE(satp);

    while (n < max - 1) {
        uintptr_t va = uva + n;
        uintptr_t pa;
        pte_t e;
        char c;

        if (va < uva) {
            break;                      /* 桁あふれ */
        }
        e = translate(root, va, &pa);
        if (!PTE_UR(e)) {
            break;
        }
        c = *(const volatile char *)pa;   /* RAM は恒等マップなので物理アドレスで読める */
        if (c == '\0') {
            break;
        }
        dst[n++] = c;
    }
    dst[n] = '\0';
    return n;
}

/*
 * 実行中の空間のユーザーのメモリ uva へ len バイト書く。
 * 範囲のすべてのページに U と W があることを先に確かめ、1 つでもなければ何も書かずに -1。
 */
int rv_user_copy_out(uintptr_t uva, const void *src, size_t len)
{
    uint64_t satp;
    const pte_t *root;
    const uint8_t *s = src;
    uintptr_t va, pa;
    size_t k;

    if (len == 0) {
        return 0;
    }
    if (uva + len < uva) {
        return -1;
    }
    satp = arch_cur_space();
    root = ROOT_FROM_SPACE(satp);

    for (va = uva & ~(PAGE_SIZE - 1); va < uva + len; va += PAGE_SIZE) {
        pte_t e = translate(root, va, &pa);
        if (!PTE_UW(e)) {
            return -1;
        }
    }
    for (k = 0; k < len; k++) {
        translate(root, uva + k, &pa);
        *(volatile uint8_t *)pa = s[k];
    }
    return 0;
}

/*
 * 文字列の引数をプログラムのスタックの上端に置き、そのユーザーのアドレスを返す。
 * スタックの開始位置（sp）はその下（16 バイト境界）へ下がる。失敗時 0。
 * 呼ぶのはプログラムを動かす前（rv_uproc_load の直後）。
 */
uintptr_t rv_uproc_push_string(struct rv_uproc *p, const char *str)
{
    size_t len = 0, k;
    uintptr_t va, pa;

    while (str[len] != 0) {
        len++;
    }
    if (len + 1 > 512) {
        return 0;
    }
    va = (p->sp - (len + 1)) & ~(uintptr_t)15;
    for (k = 0; k <= len; k++) {
        pte_t e = translate(p->root, va + k, &pa);
        if (!PTE_UW(e)) {
            return 0;
        }
        *(volatile char *)pa = str[k];
    }
    p->sp = va;
    return va;
}

/* ======================================================================
 * Linux 互換のプログラム（RISC-V の Linux の ELF をそのまま動かす）
 * ====================================================================== */

/* プログラム p の空間のユーザーのアドレス va へ len バイト書く（動かす前の準備用） */
static int write_to(struct rv_uproc *p, uintptr_t va, const void *src, size_t len)
{
    const uint8_t *s = src;
    size_t k;

    for (k = 0; k < len; k++) {
        uintptr_t pa;
        pte_t e = translate(p->root, va + k, &pa);
        if (!PTE_UW(e)) {
            return -1;
        }
        *(volatile uint8_t *)pa = s[k];
    }
    return 0;
}

static size_t str_len(const char *s)
{
    size_t n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_EXECFN   31

/*
 * Linux の起動時のスタックを作る（上が高いアドレス）
 *   文字列（argv と envp の中身、プログラム名）、乱数 16 バイト
 *   … 16 バイト境界 …
 *   auxv（種類と値の組、AT_NULL で終わり）
 *   envp[0..] と NULL
 *   argv[0..] と NULL
 *   argc                                  ← sp
 */
static int setup_linux_stack(struct rv_uproc *p, const char *const argv[], const char *const envp[])
{
    uintptr_t argv_va[16], envp_va[16];
    uint64_t words[16 + 1 + 16 + 1 + 2 * 16 + 1];
    uintptr_t sp = UVA_STACK_TOP;
    uintptr_t rand_va, execfn_va;
    unsigned argc = 0, envc = 0, n = 0, i;
    uint64_t seed;

    while (argv != NULL && argv[argc] != NULL && argc < 16) {
        argc++;
    }
    while (envp != NULL && envp[envc] != NULL && envc < 16) {
        envc++;
    }
    for (i = 0; i < envc; i++) {
        size_t l = str_len(envp[i]) + 1;
        sp -= l;
        if (write_to(p, sp, envp[i], l) != 0) return -1;
        envp_va[i] = sp;
    }
    for (i = argc; i > 0; i--) {
        size_t l = str_len(argv[i - 1]) + 1;
        sp -= l;
        if (write_to(p, sp, argv[i - 1], l) != 0) return -1;
        argv_va[i - 1] = sp;
    }
    execfn_va = (argc > 0) ? argv_va[0] : 0;
    sp &= ~(uintptr_t)15;
    sp -= 16;                               /* AT_RANDOM の 16 バイト */
    seed = arch_rdtime();
    {
        uint64_t r[2];
        r[0] = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        r[1] = r[0] * 6364136223846793005ULL + (uintptr_t)p;
        if (write_to(p, sp, r, 16) != 0) return -1;
    }
    rand_va = sp;

    words[n++] = argc;
    for (i = 0; i < argc; i++) words[n++] = argv_va[i];
    words[n++] = 0;
    for (i = 0; i < envc; i++) words[n++] = envp_va[i];
    words[n++] = 0;
#define AUX(t, v) do { words[n++] = (t); words[n++] = (v); } while (0)
    AUX(AT_PHDR, p->phdr_va);
    AUX(AT_PHENT, 56);
    AUX(AT_PHNUM, p->phnum);
    AUX(AT_PAGESZ, PAGE_SIZE);
    AUX(AT_ENTRY, p->entry);
    AUX(AT_UID, 0);
    AUX(AT_EUID, 0);
    AUX(AT_GID, 0);
    AUX(AT_EGID, 0);
    AUX(AT_HWCAP, 0);
    AUX(AT_CLKTCK, 100);
    AUX(AT_SECURE, 0);
    AUX(AT_RANDOM, rand_va);
    AUX(AT_EXECFN, execfn_va);
    AUX(AT_NULL, 0);
#undef AUX
    sp -= n * 8;
    sp &= ~(uintptr_t)15;                   /* sp は 16 バイト境界 */
    if (write_to(p, sp, words, n * 8) != 0) return -1;
    p->sp = sp;
    return 0;
}

static int next_pid = 1;

/* RISC-V の Linux 用 ELF を読み込み、argv と envp を積んだスタックを用意する */
struct rv_uproc *rv_uproc_load_linux(const void *image, size_t len,
                                     const char *const argv[], const char *const envp[])
{
    struct rv_uproc *p = uproc_load(image, len, STACK_PAGES_LINUX);

    if (p == NULL) {
        return NULL;
    }
    p->linux_abi = 1;
    p->pid = next_pid++;
    {
        /* シグナルの処理関数から戻る先: li a7, 139（rt_sigreturn）; ecall
         * （Linux は vDSO に置くもの。musl の RISC-V 版は自分では用意しない） */
        uint32_t *pg = page_alloc();
        if (pg == NULL || map_page(p->root, UVA_SIGTRAMP, pg, UP_R | UP_X) != 0) {
            if (pg != NULL) {
                page_free(pg);
            }
            rv_uproc_free(p);
            rv_uproc_last_error = UPE_NOMEM;
            return NULL;
        }
        pg[0] = ARCH_SIGTRAMP0;  /* RISC-V: addi a7, zero, 139 / AArch64: mov x8, #139 */
        pg[1] = ARCH_SIGTRAMP1;  /* RISC-V: ecall / AArch64: svc #0 */
        arch_icache_sync();
    }
    if (setup_linux_stack(p, argv, envp) != 0) {
        rv_uproc_free(p);
        rv_uproc_last_error = UPE_NOMEM;
        return NULL;
    }
    return p;
}

/* 実行中の空間のプログラム（なければ NULL） */
struct rv_uproc *rv_uproc_current(void)
{
    int i = rv_uproc_current_slot();
    return (i >= 0) ? &uprocs[i] : NULL;
}

int rv_uproc_is_linux(const struct rv_uproc *p) { return p != NULL && p->linux_abi; }
int rv_uproc_pid(const struct rv_uproc *p)      { return p->pid; }

/* 実行中の空間のユーザーのメモリ uva から len バイト読む。U と R のないページがあれば -1 */
int rv_user_copy_in(void *dst, uintptr_t uva, size_t len)
{
    uint64_t satp;
    const pte_t *root;
    uint8_t *d = dst;
    size_t k;

    if (uva + len < uva) {
        return -1;
    }
    satp = arch_cur_space();
    root = ROOT_FROM_SPACE(satp);
    for (k = 0; k < len; k++) {
        uintptr_t pa;
        pte_t e = translate(root, uva + k, &pa);
        if (!PTE_UR(e)) {
            return -1;
        }
        d[k] = *(const volatile uint8_t *)pa;
    }
    return 0;
}

/* [lo, hi) に 0 で埋めたページを R/W で写す（すでにあるページはそのまま） */
static int map_anon(struct rv_uproc *p, uintptr_t lo, uintptr_t hi)
{
    uintptr_t va;

    for (va = lo; va < hi; va += PAGE_SIZE) {
        pte_t *slot = pte_slot(p->root, va);
        void *pg;
        if (slot == NULL) {
            return -1;
        }
        if (*slot & PTE_V) {
            continue;
        }
        pg = page_alloc();
        if (pg == NULL) {
            return -1;
        }
        *slot = MK_UPTE(pg, UP_R | UP_W);
    }
    arch_tlb_flush();
    return 0;
}

static void unmap_range(struct rv_uproc *p, uintptr_t lo, uintptr_t hi)
{
    uintptr_t va;

    for (va = lo; va < hi; va += PAGE_SIZE) {
        pte_t *l1 = next_table(p->root, VPN2(va), 0);
        pte_t *l0 = l1 ? next_table(l1, VPN1(va), 0) : NULL;
        if (l0 != NULL && (l0[VPN0(va)] & PTE_V)) {
            page_free((void *)(uintptr_t)PTE_TO_PA(l0[VPN0(va)]));
            l0[VPN0(va)] = 0;
        }
    }
    arch_tlb_flush();
}

/* brk(addr): 0 なら今の値。広げるときはページを足し、縮めるときは返す。戻り値は新しい（または今の）値 */
uintptr_t rv_linux_brk(struct rv_uproc *p, uintptr_t addr)
{
    uintptr_t old_top = (p->brk_cur + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uintptr_t new_top;

    if (addr == 0 || addr < p->brk_start || addr >= UVA_SEG_END) {
        return p->brk_cur;
    }
    new_top = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (new_top > old_top) {
        if (map_anon(p, old_top, new_top) != 0) {
            return p->brk_cur;
        }
    } else if (new_top < old_top) {
        unmap_range(p, new_top, old_top);
    }
    p->brk_cur = addr;
    return p->brk_cur;
}

/* mmap の領域で、len バイト（ページ単位）の空いている所を探す（前後に 1 ページすき間）。なければ 0 */
static uintptr_t find_gap(struct rv_uproc *p, size_t len)
{
    uintptr_t need = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uintptr_t lo, va;
    int pass;

    if (len == 0 || need > UVA_MMAP_END - UVA_MMAP_LO) {
        return 0;
    }
    for (pass = 0; pass < 2; pass++) {
        lo = pass == 0 ? p->mmap_next : UVA_MMAP_LO;    /* まず続きから、だめなら頭から空きを探す */
        while (lo + need <= UVA_MMAP_END) {
            for (va = lo; va < lo + need + PAGE_SIZE && va < UVA_MMAP_END; va += PAGE_SIZE) {
                uintptr_t pa;
                if (translate(p->root, va, &pa) & PTE_V) {
                    break;
                }
            }
            if (va >= lo + need + PAGE_SIZE || va >= UVA_MMAP_END) {
                return lo;
            }
            lo = va + PAGE_SIZE;
        }
    }
    return 0;
}

/* [lo, hi) に 0 で埋めたページを写す。shared なら共有の印をつける */
static int map_anon_flags(struct rv_uproc *p, uintptr_t lo, uintptr_t hi, int shared)
{
    uintptr_t va;

    for (va = lo; va < hi; va += PAGE_SIZE) {
        pte_t *slot = pte_slot(p->root, va);
        void *pg;
        if (slot == NULL) {
            return -1;
        }
        if (*slot & PTE_V) {
            continue;
        }
        pg = page_alloc();
        if (pg == NULL) {
            return -1;
        }
        *slot = MK_UPTE(pg, UP_R | UP_W | (shared ? UP_SHARED : 0));
    }
    arch_tlb_flush();
    return 0;
}

/* 無名の mmap（ファイルなし）。addr の指定は目安として扱い、mmap の領域の空いている所に割り当てる。
 * shared なら MAP_SHARED（fork した子とページを共有する）。失敗は 0 */
uintptr_t rv_linux_mmap_anon2(struct rv_uproc *p, size_t len, int shared)
{
    uintptr_t lo = find_gap(p, len);
    uintptr_t hi = lo + ((len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));

    if (lo == 0) {
        return 0;
    }
    if (map_anon_flags(p, lo, hi, shared) != 0) {
        unmap_range(p, lo, hi);
        return 0;
    }
    p->mmap_next = hi + PAGE_SIZE;
    return lo;
}

uintptr_t rv_linux_mmap_anon(struct rv_uproc *p, size_t len)
{
    return rv_linux_mmap_anon2(p, len, 0);
}

/* mmap の領域で空いている所を探すだけ（写すのは呼ぶ側）。失敗は 0 */
uintptr_t rv_linux_mmap_reserve(struct rv_uproc *p, size_t len)
{
    uintptr_t lo = find_gap(p, len);
    if (lo != 0) {
        p->mmap_next = lo + ((len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)) + PAGE_SIZE;
    }
    return lo;
}

/* カーネルのページ（ユーザー用のページの領域のもの）を va に共有で写す。参照を 1 つ増やす。成功 0 */
int rv_linux_map_shared(struct rv_uproc *p, uintptr_t va, void *kpage, int writable)
{
    pte_t *slot = pte_slot(p->root, va);
    if (slot == NULL || (*slot & PTE_V) || page_ref(kpage) != 0) {
        return -1;
    }
    *slot = MK_UPTE(kpage, UP_R | (writable ? UP_W : 0) | UP_SHARED);
    arch_tlb_flush();
    return 0;
}

/* [lo, hi) に無名のページを写す（mremap で広げた分など）。成功 0 */
int rv_linux_map_anon_at(struct rv_uproc *p, uintptr_t lo, uintptr_t hi, int shared)
{
    return map_anon_flags(p, lo, hi, shared);
}

/* 写したページを、old から new へ npages 枚動かす（中身は写さない。参照の数も変わらない） */
int rv_linux_move_pages(struct rv_uproc *p, uintptr_t old, uintptr_t nva, size_t npages)
{
    size_t k;
    for (k = 0; k < npages; k++) {
        uintptr_t o = old + k * PAGE_SIZE, n = nva + k * PAGE_SIZE;
        pte_t *l1 = next_table(p->root, VPN2(o), 0);
        pte_t *l0 = l1 ? next_table(l1, VPN1(o), 0) : NULL;
        pte_t *slot;
        if (l0 == NULL || !(l0[VPN0(o)] & PTE_V)) {
            continue;
        }
        slot = pte_slot(p->root, n);
        if (slot == NULL) {
            return -1;
        }
        *slot = l0[VPN0(o)];
        l0[VPN0(o)] = 0;
    }
    arch_tlb_flush();
    return 0;
}

int rv_linux_munmap(struct rv_uproc *p, uintptr_t addr, size_t len)
{
    uintptr_t hi = addr + ((len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));

    if ((addr & (PAGE_SIZE - 1)) != 0 || addr < UVA_MMAP_LO || hi > UVA_MMAP_END || hi < addr) {
        return -22;                         /* EINVAL */
    }
    unmap_range(p, addr, hi);
    return 0;
}

/* テスト用: va のページの参照の数（写っていなければ 0） */
int rv_linux_page_refs(struct rv_uproc *p, uintptr_t va)
{
    uintptr_t pa;
    uintptr_t off;
    if (!(translate(p->root, va, &pa) & PTE_V)) {
        return 0;
    }
    off = (pa & ~(PAGE_SIZE - 1)) - (uintptr_t)page_pool;
    return off / PAGE_SIZE < RV_PAGE_POOL ? page_used[off / PAGE_SIZE] : 0;
}

/* ---- fork と execve のための補助 ---- */

/*
 * fork: 親 p のユーザー空間を丸ごと複製した、新しいプログラムを作る（ページはすべて写す）。
 * レジスタは複製しない（呼ぶ側がトラップフレームを子に渡す）。失敗は NULL
 */
struct rv_uproc *rv_uproc_fork(const struct rv_uproc *p)
{
    pte_t *kroot = rv_mmu_kernel_root();
    struct rv_uproc *c = NULL;
    unsigned i, top, j, k;

    for (i = 0; i < RV_UPROC_MAX; i++) {
        if (!uprocs[i].used) {
            c = &uprocs[i];
            break;
        }
    }
    if (c == NULL) {
        rv_uproc_last_error = UPE_SLOT;
        return NULL;
    }
    c->used = 1;
    c->root = page_alloc();
    if (c->root == NULL) {
        c->used = 0;
        rv_uproc_last_error = UPE_NOMEM;
        return NULL;
    }
    for (i = UVA_VPN2_COUNT; i < 512; i++) {
        c->root[i] = kroot[i];
    }
    for (top = 0; top < UVA_VPN2_COUNT; top++) {
        pte_t *l1 = next_table(p->root, top, 0);
        if (l1 == NULL) {
            continue;
        }
        for (j = 0; j < 512; j++) {
            pte_t *l0 = next_table(l1, j, 0);
            if (l0 == NULL) {
                continue;
            }
            for (k = 0; k < 512; k++) {
                uintptr_t va = ((uintptr_t)top << 30) | ((uintptr_t)j << 21) | ((uintptr_t)k << 12);
                pte_t e = l0[k];
                pte_t *slot;
                uint8_t *pg;
                const uint8_t *src;
                unsigned b;
                if (!(e & PTE_V)) {
                    continue;
                }
                slot = pte_slot(c->root, va);
                if (PTE_IS_SHARED(e) && slot != NULL
                    && page_ref((void *)(uintptr_t)PTE_TO_PA(e)) == 0) {
                    *slot = e;                  /* 共有のページ: 同じページを指す */
                    continue;
                }
                pg = page_alloc();
                if (slot == NULL || pg == NULL) {
                    if (pg != NULL) {
                        page_free(pg);
                    }
                    free_user_tables(c->root);
                    c->used = 0;
                    rv_uproc_last_error = UPE_NOMEM;
                    return NULL;
                }
                src = (const uint8_t *)(uintptr_t)PTE_TO_PA(e);
                for (b = 0; b < PAGE_SIZE; b++) {
                    pg[b] = src[b];
                }
                *slot = PTE_WITH_PA(e, pg);    /* 権限などはそのまま */
            }
        }
    }
    c->satp = SPACE_FROM_ROOT(c->root);
    c->entry = p->entry;
    c->sp = p->sp;
    c->linux_abi = p->linux_abi;
    c->pid = 0;                         /* 呼ぶ側が決める */
    c->phdr_va = p->phdr_va;
    c->phnum = p->phnum;
    c->brk_start = p->brk_start;
    c->brk_cur = p->brk_cur;
    c->mmap_next = p->mmap_next;
    asm volatile("" ::: "memory");
    return c;
}

void rv_uproc_set_pid(struct rv_uproc *p, int pid) { p->pid = pid; }
int  rv_uproc_next_pid(void)                        { return next_pid++; }
