/*
 * tk_port_mem.c — カーネルのメモリ（Imalloc）
 *
 *   タスクのスタック・TCB・ミューテックスなどの表はここから取る。
 *   以前は取りっぱなし（解放しない）だったので、fork のたびにスタックが
 *   漏れていた。いまは解放した領域を空きリストに戻し、隣が空いていれば
 *   つなげる（先頭から最初に入るところを使う）。
 *
 *   各ブロックの先頭（ヘッダの直後）に見張りのバイト列を置く。スタックは
 *   下に伸びるので、あふれると最初にここが壊れる。rv_heap_check() で
 *   全部のブロックを調べ、壊れていれば場所を報告する。
 */
#include <sys/imalloc.h>
#include <tk/errno.h>
#include <libstr.h>
#include <stdint.h>

#define TK_PORT_HEAP_SIZE  (4U * 1024U * 1024U)
#define HDR_MAGIC_USED     0x55534544A110C8EDULL
#define HDR_MAGIC_FREE     0x46524545F4EEB10CULL
#define GUARD_SIZE         64U
#define GUARD_BYTE         0xA5U

struct blk {
    uint64_t magic;
    uint64_t size;          /* ヘッダを含むブロック全体の大きさ（16 の倍数） */
    struct blk *next_free;  /* 空きのときだけ使う */
    uint64_t owner;         /* 取った所（呼び出し元のアドレス、調査用） */
};

static uint8_t g_heap[TK_PORT_HEAP_SIZE] __attribute__((aligned(16)));
static struct blk *g_free;
static int g_ready;
size_t rv_heap_in_use;
size_t rv_heap_peak;

extern void uart_puts(const char *s);
extern void uart_putc(char c);

static void uart_puthex64(uint64_t v)
{
    int i;
    uart_puts("0x");
    for (i = 60; i >= 0; i -= 4) {
        uart_putc("0123456789abcdef"[(v >> i) & 15]);
    }
}

static void heap_init(void)
{
    g_free = (struct blk *)g_heap;
    g_free->magic = HDR_MAGIC_FREE;
    g_free->size = TK_PORT_HEAP_SIZE;
    g_free->next_free = 0;
    rv_heap_in_use = 0;
    g_ready = 1;
}

/* 呼ぶ側（T-Kernel）は割り込みを止めた中で呼ぶので、ここでは排他しない */
static void *imalloc_from(size_t size, uint64_t owner);

void *Imalloc(size_t size)
{
    return imalloc_from(size, (uint64_t)(uintptr_t)__builtin_return_address(0));
}

static void *imalloc_from(size_t size, uint64_t owner)
{
    struct blk **pp, *b;
    uint64_t need;

    if (!g_ready) {
        heap_init();
    }
    if (size == 0) {
        return 0;
    }
    need = (sizeof(struct blk) + GUARD_SIZE + size + 15U) & ~(uint64_t)15U;
    for (pp = &g_free; (b = *pp) != 0; pp = &b->next_free) {
        if (b->size < need) {
            continue;
        }
        if (b->size - need >= sizeof(struct blk) + GUARD_SIZE + 64U) {
            struct blk *rest = (struct blk *)((uint8_t *)b + need);
            rest->magic = HDR_MAGIC_FREE;
            rest->size = b->size - need;
            rest->next_free = b->next_free;
            *pp = rest;
            b->size = need;
        } else {
            *pp = b->next_free;
        }
        b->magic = HDR_MAGIC_USED;
        b->next_free = 0;
        b->owner = owner;
        memset((uint8_t *)(b + 1), GUARD_BYTE, GUARD_SIZE);
        rv_heap_in_use += b->size;
        if (rv_heap_in_use > rv_heap_peak) {
            rv_heap_peak = rv_heap_in_use;
        }
        return (uint8_t *)(b + 1) + GUARD_SIZE;
    }
    return 0;
}

void Ifree(void *ptr)
{
    struct blk *b, **pp, *q, *prev = 0;

    if (ptr == 0) {
        return;
    }
    b = (struct blk *)((uint8_t *)ptr - GUARD_SIZE) - 1;
    if (b->magic != HDR_MAGIC_USED) {
        uart_puts("\r\n[heap] bad free ");
        uart_puthex64((uint64_t)(uintptr_t)ptr);
        uart_puts("\r\n");
        return;
    }
    rv_heap_in_use -= b->size;
    b->magic = HDR_MAGIC_FREE;

    /* アドレス順の空きリストに入れ、前後とつなげる */
    for (pp = &g_free; (q = *pp) != 0 && q < b; pp = &q->next_free) {
        prev = q;
    }
    b->next_free = q;
    *pp = b;
    if (q != 0 && (uint8_t *)b + b->size == (uint8_t *)q) {
        b->size += q->size;
        b->next_free = q->next_free;
    }
    if (prev != 0) {
        if ((uint8_t *)prev + prev->size == (uint8_t *)b) {
            prev->size += b->size;
            prev->next_free = b->next_free;
        }
    }
}

/* 全ブロックの見張りを調べる。壊れていた数を返す（見つけたら表示する） */
int rv_heap_check(void)
{
    uint8_t *p = g_heap, *end = g_heap + TK_PORT_HEAP_SIZE;
    int bad = 0;

    if (!g_ready) {
        return 0;
    }
    while (p < end) {
        struct blk *b = (struct blk *)p;
        unsigned i;

        if ((b->magic != HDR_MAGIC_USED && b->magic != HDR_MAGIC_FREE)
            || b->size < sizeof(struct blk) || b->size > (uint64_t)(end - p)) {
            uart_puts("\r\n[heap] broken header at ");
            uart_puthex64((uint64_t)(uintptr_t)p);
            uart_puts("\r\n");
            return bad + 1;
        }
        if (b->magic == HDR_MAGIC_USED) {
            for (i = 0; i < GUARD_SIZE; i++) {
                if (((uint8_t *)(b + 1))[i] != GUARD_BYTE) {
                    uart_puts("\r\n[heap] guard overwritten, block ");
                    uart_puthex64((uint64_t)(uintptr_t)((uint8_t *)(b + 1) + GUARD_SIZE));
                    uart_puts(" size ");
                    uart_puthex64(b->size);
                    uart_puts(" owner ");
                    uart_puthex64(b->owner);
                    uart_puts(" at+");
                    uart_puthex64(i);
                    uart_puts(" w=");
                    uart_puthex64(*(uint64_t *)((uint8_t *)(b + 1) + (i & ~7U)));
                    uart_puts("\r\n");
                    bad++;
                    break;
                }
            }
        }
        p += b->size;
    }
    return bad;
}

void *IAmalloc(size_t size, UINT attr)
{
    (void)attr;
    return imalloc_from(size, (uint64_t)(uintptr_t)__builtin_return_address(0));
}

void *IAcalloc(size_t nmemb, size_t size, UINT attr)
{
    void *p;
    (void)attr;
    p = Imalloc(nmemb * size);
    if (p != 0) {
        memset(p, 0, nmemb * size);
    }
    return p;
}

void IAfree(void *ptr, UINT attr)
{
    (void)attr;
    Ifree(ptr);
}

void *Icalloc(size_t nmemb, size_t size)
{
    return IAcalloc(nmemb, size, 0);
}

ER init_Imalloc(void)
{
    heap_init();
    return E_OK;
}

/* 調査用: 使用中のブロックを全部表示する */
void rv_heap_dump(void)
{
    uint8_t *p = g_heap, *end = g_heap + TK_PORT_HEAP_SIZE;

    while (p < end) {
        struct blk *b = (struct blk *)p;
        if ((b->magic != HDR_MAGIC_USED && b->magic != HDR_MAGIC_FREE)
            || b->size < sizeof(struct blk) || b->size > (uint64_t)(end - p)) {
            return;
        }
        if (b->magic == HDR_MAGIC_USED) {
            uart_puts("[heap] ");
            uart_puthex64((uint64_t)(uintptr_t)((uint8_t *)(b + 1) + GUARD_SIZE));
            uart_puts(" ");
            uart_puthex64(b->size);
            uart_puts(" ");
            uart_puthex64(b->owner);
            uart_puts("\r\n");
        }
        p += b->size;
    }
}
